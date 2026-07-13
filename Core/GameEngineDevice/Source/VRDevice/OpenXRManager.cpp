/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 GeneralsVR contributors
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// FILE: OpenXRManager.cpp ////////////////////////////////////////////////////////////////////////
// GeneralsVR @feature OpenXR session, swapchains and stereo frame submission. See OpenXRManager.h.
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "VRDevice/OpenXRManager.h"
#include "VRDevice/DxvkInterop.h"

#include "Common/Debug.h"
#include "Common/GlobalData.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d8.h>

#include <string.h>
#include <vector>

OpenXRManager* TheOpenXR = nullptr;

//-------------------------------------------------------------------------------------------------
// Vulkan entry points, resolved from the loader at runtime. We never create a Vulkan instance or
// device ourselves - both belong to DXVK - so only the handful of functions used by the eye copy
// are needed.
//-------------------------------------------------------------------------------------------------
namespace
{
	struct VulkanApi
	{
		PFN_vkGetDeviceProcAddr getDeviceProcAddr;
		PFN_vkCreateCommandPool createCommandPool;
		PFN_vkDestroyCommandPool destroyCommandPool;
		PFN_vkAllocateCommandBuffers allocateCommandBuffers;
		PFN_vkBeginCommandBuffer beginCommandBuffer;
		PFN_vkEndCommandBuffer endCommandBuffer;
		PFN_vkResetCommandBuffer resetCommandBuffer;
		PFN_vkCmdPipelineBarrier cmdPipelineBarrier;
		PFN_vkCmdCopyImage cmdCopyImage;
		PFN_vkCmdBlitImage cmdBlitImage;
		PFN_vkQueueSubmit queueSubmit;
		PFN_vkCreateFence createFence;
		PFN_vkDestroyFence destroyFence;
		PFN_vkWaitForFences waitForFences;
		PFN_vkResetFences resetFences;
		PFN_vkDeviceWaitIdle deviceWaitIdle;
	};

	VulkanApi g_vk = {};

	// GeneralsVR: the eye render targets are D3DFMT_A8R8G8B8, which DXVK backs with
	// VK_FORMAT_B8G8R8A8_UNORM. Copying those bytes verbatim into a *_SRGB swapchain image of
	// the same layout is what we want: the game already writes sRGB-encoded colour, and the
	// compositor expects sRGB-encoded content in an sRGB format. Preferring a BGRA_SRGB
	// swapchain therefore lets us use a raw vkCmdCopyImage (no channel swap, no gamma applied).
	const int64_t kPreferredSwapchainFormats[] =
	{
		VK_FORMAT_B8G8R8A8_SRGB,   // ideal: byte-identical to the eye targets
		VK_FORMAT_R8G8B8A8_SRGB,   // needs a blit (channel swap); gamma will be slightly off
		VK_FORMAT_B8G8R8A8_UNORM,
		VK_FORMAT_R8G8B8A8_UNORM,
	};
}

//-------------------------------------------------------------------------------------------------
OpenXRManager::OpenXRManager()
	: m_instance(XR_NULL_HANDLE)
	, m_systemId(XR_NULL_SYSTEM_ID)
	, m_session(XR_NULL_HANDLE)
	, m_appSpace(XR_NULL_HANDLE)
	, m_sessionState(XR_SESSION_STATE_UNKNOWN)
	, m_blendMode(XR_ENVIRONMENT_BLEND_MODE_OPAQUE)
	, m_sessionRunning(FALSE)
	, m_frameActive(FALSE)
	, m_predictedDisplayTime(0)
	, m_eyeCount(0)
	, m_eyeWidth(0)
	, m_eyeHeight(0)
	, m_worldUnitsPerMeter(500.0f)
	, m_supportsVulkan(FALSE)
	, m_supportsVulkan1(FALSE)
	, m_supportsD3D11(FALSE)
	, m_dxvkInterop(nullptr)
	, m_vkInstance(VK_NULL_HANDLE)
	, m_vkPhysicalDevice(VK_NULL_HANDLE)
	, m_vkDevice(VK_NULL_HANDLE)
	, m_vkQueue(VK_NULL_HANDLE)
	, m_vkQueueIndex(0)
	, m_vkQueueFamilyIndex(0)
	, m_vkCommandPool(VK_NULL_HANDLE)
	, m_vkCommandBuffer(VK_NULL_HANDLE)
	, m_vkFence(VK_NULL_HANDLE)
	, m_depthSurface(nullptr)
	, m_eyeImageLayout(VK_IMAGE_LAYOUT_UNDEFINED)
	, m_framesSubmitted(0)
	, m_submitFailLogged(FALSE)
{
	for (int i = 0; i < MAX_EYES; ++i)
	{
		m_swapchains[i] = XR_NULL_HANDLE;
		m_eyeTextures[i] = nullptr;
		m_eyeSurfaces[i] = nullptr;
		m_eyeImages[i] = VK_NULL_HANDLE;
		m_eyeViews[i] = VREyeView();
		m_eyePoses[i] = XrPosef();
		m_eyeFovs[i] = XrFovf();
	}
}

OpenXRManager::~OpenXRManager()
{
	shutdown();
}

//-------------------------------------------------------------------------------------------------
Bool OpenXRManager::hasExtension(const char* name) const
{
	uint32_t count = 0;
	if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr)) || count == 0)
		return FALSE;

	std::vector<XrExtensionProperties> exts(count);
	for (uint32_t i = 0; i < count; ++i)
	{
		exts[i] = XrExtensionProperties{};
		exts[i].type = XR_TYPE_EXTENSION_PROPERTIES;
	}
	if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, exts.data())))
		return FALSE;

	for (uint32_t i = 0; i < count; ++i)
	{
		if (strcmp(exts[i].extensionName, name) == 0)
			return TRUE;
	}
	return FALSE;
}

//-------------------------------------------------------------------------------------------------
Bool OpenXRManager::init()
{
	m_supportsVulkan = hasExtension(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
	m_supportsVulkan1 = hasExtension(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
	m_supportsD3D11 = hasExtension("XR_KHR_D3D11_enable");
	DEBUG_LOG(("OpenXR: runtime graphics bindings: vulkan2=%d vulkan1=%d d3d11=%d",
		m_supportsVulkan, m_supportsVulkan1, m_supportsD3D11));

	if (!m_supportsVulkan1)
	{
		DEBUG_LOG(("OpenXR: XR_KHR_vulkan_enable missing - cannot bind DXVK's device, VR unavailable"));
		return FALSE;
	}

	const char* enabledExts[] = { XR_KHR_VULKAN_ENABLE_EXTENSION_NAME };

	XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
	strcpy(ci.applicationInfo.applicationName, "GeneralsVR");
	ci.applicationInfo.applicationVersion = 1;
	strcpy(ci.applicationInfo.engineName, "SAGE-W3D");
	ci.applicationInfo.engineVersion = 1;
	ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	ci.enabledExtensionCount = 1;
	ci.enabledExtensionNames = enabledExts;

	XrResult result = xrCreateInstance(&ci, &m_instance);
	if (XR_FAILED(result))
	{
		DEBUG_LOG(("OpenXR: xrCreateInstance failed (%d), VR unavailable", (int)result));
		m_instance = XR_NULL_HANDLE;
		return FALSE;
	}

	XrInstanceProperties ip = {XR_TYPE_INSTANCE_PROPERTIES};
	if (XR_SUCCEEDED(xrGetInstanceProperties(m_instance, &ip)))
	{
		DEBUG_LOG(("OpenXR: runtime '%s' version %u.%u.%u", ip.runtimeName,
			XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion),
			XR_VERSION_PATCH(ip.runtimeVersion)));
	}

	XrSystemGetInfo sgi = {XR_TYPE_SYSTEM_GET_INFO};
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	result = xrGetSystem(m_instance, &sgi, &m_systemId);
	if (XR_FAILED(result))
	{
		DEBUG_LOG(("OpenXR: xrGetSystem failed (%d) - headset not connected? VR unavailable", (int)result));
		m_systemId = XR_NULL_SYSTEM_ID;
		return FALSE;
	}

	XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
	if (XR_SUCCEEDED(xrGetSystemProperties(m_instance, m_systemId, &sp)))
	{
		DEBUG_LOG(("OpenXR: system '%s'", sp.systemName));
	}

	uint32_t viewCount = 0;
	xrEnumerateViewConfigurationViews(m_instance, m_systemId,
		XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr);
	if (viewCount == 0)
	{
		DEBUG_LOG(("OpenXR: no stereo views reported, VR unavailable"));
		return FALSE;
	}

	std::vector<XrViewConfigurationView> views(viewCount);
	for (uint32_t i = 0; i < viewCount; ++i)
	{
		views[i] = XrViewConfigurationView{};
		views[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
	}
	if (XR_FAILED(xrEnumerateViewConfigurationViews(m_instance, m_systemId,
		XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, views.data())))
	{
		DEBUG_LOG(("OpenXR: could not enumerate view configurations, VR unavailable"));
		return FALSE;
	}

	m_eyeCount = (Int)(viewCount < MAX_EYES ? viewCount : MAX_EYES);
	m_eyeWidth = views[0].recommendedImageRectWidth;
	m_eyeHeight = views[0].recommendedImageRectHeight;
	DEBUG_LOG(("OpenXR: %d views, recommended eye target %dx%d", m_eyeCount, m_eyeWidth, m_eyeHeight));

	// The scene is drawn once per eye through a CPU-heavy fixed-function path, so trading a
	// little sharpness for frame rate is often the right call.
	if (TheGlobalData != nullptr && TheGlobalData->m_vrResolutionScale > 0.0f
		&& TheGlobalData->m_vrResolutionScale != 1.0f)
	{
		m_eyeWidth = (Int)(m_eyeWidth * TheGlobalData->m_vrResolutionScale);
		m_eyeHeight = (Int)(m_eyeHeight * TheGlobalData->m_vrResolutionScale);
		// Keep the dimensions even; some runtimes dislike odd swapchain extents.
		m_eyeWidth &= ~1;
		m_eyeHeight &= ~1;
		DEBUG_LOG(("OpenXR: eye target scaled by %.2f -> %dx%d",
			TheGlobalData->m_vrResolutionScale, m_eyeWidth, m_eyeHeight));
	}

	if (TheGlobalData != nullptr && TheGlobalData->m_vrWorldUnitsPerMeter > 0.0f)
		m_worldUnitsPerMeter = TheGlobalData->m_vrWorldUnitsPerMeter;
	DEBUG_LOG(("OpenXR: tabletop scale: %.1f world units per metre", m_worldUnitsPerMeter));

	probeVulkanRequirements();

	DEBUG_LOG(("OpenXR: bootstrap complete, VR available"));
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
void OpenXRManager::probeVulkanRequirements()
{
	PFN_xrGetVulkanGraphicsRequirementsKHR pGetReqs = nullptr;
	PFN_xrGetVulkanInstanceExtensionsKHR pGetInstExts = nullptr;
	PFN_xrGetVulkanDeviceExtensionsKHR pGetDevExts = nullptr;
	xrGetInstanceProcAddr(m_instance, "xrGetVulkanGraphicsRequirementsKHR", (PFN_xrVoidFunction*)&pGetReqs);
	xrGetInstanceProcAddr(m_instance, "xrGetVulkanInstanceExtensionsKHR", (PFN_xrVoidFunction*)&pGetInstExts);
	xrGetInstanceProcAddr(m_instance, "xrGetVulkanDeviceExtensionsKHR", (PFN_xrVoidFunction*)&pGetDevExts);

	if (pGetReqs != nullptr)
	{
		XrGraphicsRequirementsVulkanKHR reqs = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
		if (XR_SUCCEEDED(pGetReqs(m_instance, m_systemId, &reqs)))
		{
			DEBUG_LOG(("OpenXR: vulkan API version required: min %u.%u.%u max %u.%u.%u",
				XR_VERSION_MAJOR(reqs.minApiVersionSupported), XR_VERSION_MINOR(reqs.minApiVersionSupported),
				XR_VERSION_PATCH(reqs.minApiVersionSupported),
				XR_VERSION_MAJOR(reqs.maxApiVersionSupported), XR_VERSION_MINOR(reqs.maxApiVersionSupported),
				XR_VERSION_PATCH(reqs.maxApiVersionSupported)));
		}
	}

	struct { const char* label; PFN_xrGetVulkanInstanceExtensionsKHR fn; } queries[] =
	{
		{ "instance", pGetInstExts },
		{ "device",   (PFN_xrGetVulkanInstanceExtensionsKHR)pGetDevExts },
	};
	for (int i = 0; i < 2; ++i)
	{
		if (queries[i].fn == nullptr)
			continue;
		uint32_t len = 0;
		if (XR_FAILED(queries[i].fn(m_instance, m_systemId, 0, &len, nullptr)) || len == 0)
			continue;
		std::vector<char> buf(len);
		if (XR_SUCCEEDED(queries[i].fn(m_instance, m_systemId, len, &len, buf.data())))
		{
			DEBUG_LOG(("OpenXR: required vulkan %s extensions: %s", queries[i].label, buf.data()));
		}
	}
}

//-------------------------------------------------------------------------------------------------
// DXVK's D3D8 objects wrap their D3D9 counterparts as a private member with no public accessor.
// We recover the wrapped object by scanning the first few pointer slots for one whose vtable
// lives inside d3d9.dll and which answers the interop QueryInterface. Every dereference is
// SEH-guarded and the vtable check runs before any call, so a miss is just a log line.
//-------------------------------------------------------------------------------------------------
static void* readPtrGuarded(void* addr)
{
	__try
	{
		return *(void**)addr;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return nullptr;
	}
}

static IUnknown* tryQueryInterfaceGuarded(void* candidate, const GUID& iid)
{
	__try
	{
		IUnknown* unk = (IUnknown*)candidate;
		void* out = nullptr;
		if (SUCCEEDED(unk->QueryInterface(iid, &out)) && out != nullptr)
			return (IUnknown*)out;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
	}
	return nullptr;
}

static Bool vtableLivesInModule(void* object, HMODULE module)
{
	void* vtbl = readPtrGuarded(object);
	if (vtbl == nullptr)
		return FALSE;
	HMODULE owner = nullptr;
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCSTR)vtbl, &owner))
		return FALSE;
	return owner == module;
}

/// Find the D3D9 object wrapped by a DXVK D3D8 object and query \a iid on it.
static IUnknown* queryWrappedD3D9Interface(void* d3d8Object, const GUID& iid)
{
	HMODULE d3d9Module = GetModuleHandleA("d3d9.dll");
	if (d3d9Module == nullptr || d3d8Object == nullptr)
		return nullptr;

	for (int offset = sizeof(void*); offset <= 64; offset += sizeof(void*))
	{
		void* candidate = readPtrGuarded((char*)d3d8Object + offset);
		if (candidate == nullptr || candidate == d3d8Object)
			continue;
		if (!vtableLivesInModule(candidate, d3d9Module))
			continue;
		IUnknown* found = tryQueryInterfaceGuarded(candidate, iid);
		if (found != nullptr)
			return found;
	}
	return nullptr;
}

Bool OpenXRManager::findDxvkInterop(IDirect3DDevice8* d3d8Device)
{
	if (d3d8Device == nullptr)
	{
		DEBUG_LOG(("OpenXR: dxvk: no D3D8 device"));
		return FALSE;
	}
	if (GetModuleHandleA("d3d9.dll") == nullptr)
	{
		DEBUG_LOG(("OpenXR: dxvk: d3d9.dll not loaded - not running under DXVK, VR unavailable"));
		return FALSE;
	}

	m_dxvkInterop = (ID3D9VkInteropDevice*)queryWrappedD3D9Interface(d3d8Device, IID_ID3D9VkInteropDevice);
	if (m_dxvkInterop == nullptr)
	{
		DEBUG_LOG(("OpenXR: dxvk: no ID3D9VkInteropDevice behind the D3D8 device, VR unavailable"));
		return FALSE;
	}

	m_dxvkInterop->GetVulkanHandles(&m_vkInstance, &m_vkPhysicalDevice, &m_vkDevice);
	uint32_t queueIndex = 0, queueFamily = 0;
	VkQueue queue = VK_NULL_HANDLE;
	m_dxvkInterop->GetSubmissionQueue(&queue, &queueIndex, &queueFamily);
	m_vkQueue = queue;
	m_vkQueueIndex = queueIndex;
	m_vkQueueFamilyIndex = queueFamily;

	DEBUG_LOG(("OpenXR: dxvk vulkan handles: instance=%p physicalDevice=%p device=%p queue=%p family=%u index=%u",
		m_vkInstance, m_vkPhysicalDevice, m_vkDevice, m_vkQueue, m_vkQueueFamilyIndex, m_vkQueueIndex));
	return m_vkDevice != VK_NULL_HANDLE && m_vkQueue != VK_NULL_HANDLE;
}

//-------------------------------------------------------------------------------------------------
Bool OpenXRManager::loadVulkanFunctions()
{
	HMODULE vulkanModule = GetModuleHandleA("vulkan-1.dll");
	if (vulkanModule == nullptr)
		vulkanModule = LoadLibraryA("vulkan-1.dll");
	if (vulkanModule == nullptr)
	{
		DEBUG_LOG(("OpenXR: vulkan-1.dll not available"));
		return FALSE;
	}

	PFN_vkGetInstanceProcAddr getInstanceProcAddr =
		(PFN_vkGetInstanceProcAddr)GetProcAddress(vulkanModule, "vkGetInstanceProcAddr");
	if (getInstanceProcAddr == nullptr)
		return FALSE;

	g_vk.getDeviceProcAddr =
		(PFN_vkGetDeviceProcAddr)getInstanceProcAddr(m_vkInstance, "vkGetDeviceProcAddr");
	if (g_vk.getDeviceProcAddr == nullptr)
		return FALSE;

	#define LOAD_VK(member, name) \
		g_vk.member = (PFN_##name)g_vk.getDeviceProcAddr(m_vkDevice, #name); \
		if (g_vk.member == nullptr) { DEBUG_LOG(("OpenXR: vulkan: missing %s", #name)); return FALSE; }

	LOAD_VK(createCommandPool, vkCreateCommandPool)
	LOAD_VK(destroyCommandPool, vkDestroyCommandPool)
	LOAD_VK(allocateCommandBuffers, vkAllocateCommandBuffers)
	LOAD_VK(beginCommandBuffer, vkBeginCommandBuffer)
	LOAD_VK(endCommandBuffer, vkEndCommandBuffer)
	LOAD_VK(resetCommandBuffer, vkResetCommandBuffer)
	LOAD_VK(cmdPipelineBarrier, vkCmdPipelineBarrier)
	LOAD_VK(cmdCopyImage, vkCmdCopyImage)
	LOAD_VK(cmdBlitImage, vkCmdBlitImage)
	LOAD_VK(queueSubmit, vkQueueSubmit)
	LOAD_VK(createFence, vkCreateFence)
	LOAD_VK(destroyFence, vkDestroyFence)
	LOAD_VK(waitForFences, vkWaitForFences)
	LOAD_VK(resetFences, vkResetFences)
	LOAD_VK(deviceWaitIdle, vkDeviceWaitIdle)

	#undef LOAD_VK

	DEBUG_LOG(("OpenXR: vulkan entry points resolved"));
	return TRUE;
}

Bool OpenXRManager::createVulkanCopyResources()
{
	VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	poolInfo.queueFamilyIndex = m_vkQueueFamilyIndex;
	if (g_vk.createCommandPool(m_vkDevice, &poolInfo, nullptr, &m_vkCommandPool) != VK_SUCCESS)
	{
		DEBUG_LOG(("OpenXR: vulkan: vkCreateCommandPool failed"));
		return FALSE;
	}

	VkCommandBufferAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	allocInfo.commandPool = m_vkCommandPool;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = 1;
	if (g_vk.allocateCommandBuffers(m_vkDevice, &allocInfo, &m_vkCommandBuffer) != VK_SUCCESS)
	{
		DEBUG_LOG(("OpenXR: vulkan: vkAllocateCommandBuffers failed"));
		return FALSE;
	}

	VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	if (g_vk.createFence(m_vkDevice, &fenceInfo, nullptr, &m_vkFence) != VK_SUCCESS)
	{
		DEBUG_LOG(("OpenXR: vulkan: vkCreateFence failed"));
		return FALSE;
	}

	return TRUE;
}

//-------------------------------------------------------------------------------------------------
Bool OpenXRManager::createSession()
{
	PFN_xrGetVulkanGraphicsDeviceKHR pGetGraphicsDevice = nullptr;
	xrGetInstanceProcAddr(m_instance, "xrGetVulkanGraphicsDeviceKHR", (PFN_xrVoidFunction*)&pGetGraphicsDevice);
	if (pGetGraphicsDevice == nullptr)
	{
		DEBUG_LOG(("OpenXR: session: xrGetVulkanGraphicsDeviceKHR did not resolve"));
		return FALSE;
	}

	VkPhysicalDevice runtimePhysDev = VK_NULL_HANDLE;
	XrResult devResult = pGetGraphicsDevice(m_instance, m_systemId, m_vkInstance, &runtimePhysDev);
	if (XR_FAILED(devResult))
	{
		DEBUG_LOG(("OpenXR: session: xrGetVulkanGraphicsDeviceKHR FAILED (%d)", (int)devResult));
		return FALSE;
	}
	if (runtimePhysDev != m_vkPhysicalDevice)
	{
		DEBUG_LOG(("OpenXR: session: runtime wants physicalDevice=%p but DXVK renders on %p - aborting",
			runtimePhysDev, m_vkPhysicalDevice));
		return FALSE;
	}
	DEBUG_LOG(("OpenXR: session: runtime confirmed DXVK's physical device"));

	XrGraphicsBindingVulkanKHR binding = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
	binding.instance = m_vkInstance;
	binding.physicalDevice = m_vkPhysicalDevice;
	binding.device = m_vkDevice;
	binding.queueFamilyIndex = m_vkQueueFamilyIndex;
	binding.queueIndex = m_vkQueueIndex;

	XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
	sci.next = &binding;
	sci.systemId = m_systemId;

	// The runtime may touch the shared queue; follow DXVK's interop contract.
	m_dxvkInterop->FlushRenderingCommands();
	m_dxvkInterop->LockSubmissionQueue();
	XrResult result = xrCreateSession(m_instance, &sci, &m_session);
	m_dxvkInterop->ReleaseSubmissionQueue();

	if (XR_FAILED(result))
	{
		DEBUG_LOG(("OpenXR: session: xrCreateSession FAILED (%d)", (int)result));
		m_session = XR_NULL_HANDLE;
		return FALSE;
	}
	DEBUG_LOG(("OpenXR: session: created over DXVK's Vulkan device"));

	// Reference space: STAGE would pin us to the play area; LOCAL is head-relative-at-start,
	// which is what a seated tabletop view wants.
	XrReferenceSpaceCreateInfo spaceInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
	if (XR_FAILED(xrCreateReferenceSpace(m_session, &spaceInfo, &m_appSpace)))
	{
		DEBUG_LOG(("OpenXR: session: xrCreateReferenceSpace FAILED"));
		return FALSE;
	}

	uint32_t blendCount = 0;
	xrEnumerateEnvironmentBlendModes(m_instance, m_systemId,
		XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &blendCount, nullptr);
	if (blendCount > 0)
	{
		std::vector<XrEnvironmentBlendMode> modes(blendCount);
		if (XR_SUCCEEDED(xrEnumerateEnvironmentBlendModes(m_instance, m_systemId,
			XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, blendCount, &blendCount, modes.data())))
		{
			m_blendMode = modes[0];
		}
	}

	return TRUE;
}

//-------------------------------------------------------------------------------------------------
Bool OpenXRManager::createSwapchains()
{
	uint32_t formatCount = 0;
	if (XR_FAILED(xrEnumerateSwapchainFormats(m_session, 0, &formatCount, nullptr)) || formatCount == 0)
	{
		DEBUG_LOG(("OpenXR: swapchain: no formats"));
		return FALSE;
	}
	std::vector<int64_t> formats(formatCount);
	if (XR_FAILED(xrEnumerateSwapchainFormats(m_session, formatCount, &formatCount, formats.data())))
		return FALSE;

	int64_t chosen = 0;
	for (size_t p = 0; p < sizeof(kPreferredSwapchainFormats)/sizeof(int64_t) && chosen == 0; ++p)
	{
		for (uint32_t i = 0; i < formatCount; ++i)
		{
			if (formats[i] == kPreferredSwapchainFormats[p])
			{
				chosen = formats[i];
				break;
			}
		}
	}
	if (chosen == 0)
	{
		chosen = formats[0];
		DEBUG_LOG(("OpenXR: swapchain: no preferred format offered, falling back to %lld", chosen));
	}
	// A raw copy is only correct when the swapchain is BGRA (the layout D3DFMT_A8R8G8B8 gives us).
	DEBUG_LOG(("OpenXR: swapchain: format %lld (%s)", chosen,
		(chosen == VK_FORMAT_B8G8R8A8_SRGB || chosen == VK_FORMAT_B8G8R8A8_UNORM)
			? "BGRA - raw copy" : "non-BGRA - channels may swap"));

	for (Int eye = 0; eye < m_eyeCount; ++eye)
	{
		XrSwapchainCreateInfo swci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
		swci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
		swci.format = chosen;
		swci.sampleCount = 1;
		swci.width = m_eyeWidth;
		swci.height = m_eyeHeight;
		swci.faceCount = 1;
		swci.arraySize = 1;
		swci.mipCount = 1;

		if (XR_FAILED(xrCreateSwapchain(m_session, &swci, &m_swapchains[eye])))
		{
			DEBUG_LOG(("OpenXR: swapchain: xrCreateSwapchain FAILED for eye %d", eye));
			return FALSE;
		}

		uint32_t imageCount = 0;
		xrEnumerateSwapchainImages(m_swapchains[eye], 0, &imageCount, nullptr);
		std::vector<XrSwapchainImageVulkanKHR> images(imageCount);
		for (uint32_t i = 0; i < imageCount; ++i)
		{
			images[i] = XrSwapchainImageVulkanKHR{};
			images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
		}
		if (XR_FAILED(xrEnumerateSwapchainImages(m_swapchains[eye], imageCount, &imageCount,
			(XrSwapchainImageBaseHeader*)images.data())))
		{
			DEBUG_LOG(("OpenXR: swapchain: image enumeration FAILED for eye %d", eye));
			return FALSE;
		}

		m_swapchainImages[eye].clear();
		for (uint32_t i = 0; i < imageCount; ++i)
			m_swapchainImages[eye].push_back(images[i].image);

		DEBUG_LOG(("OpenXR: swapchain: eye %d, %dx%d, %u images", eye, m_eyeWidth, m_eyeHeight, imageCount));
	}

	return TRUE;
}

//-------------------------------------------------------------------------------------------------
Bool OpenXRManager::createEyeTargets(IDirect3DDevice8* device)
{
	for (Int eye = 0; eye < m_eyeCount; ++eye)
	{
		if (FAILED(device->CreateTexture(m_eyeWidth, m_eyeHeight, 1, D3DUSAGE_RENDERTARGET,
			D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_eyeTextures[eye])))
		{
			DEBUG_LOG(("OpenXR: eye targets: CreateTexture FAILED for eye %d (%dx%d)",
				eye, m_eyeWidth, m_eyeHeight));
			return FALSE;
		}
		if (FAILED(m_eyeTextures[eye]->GetSurfaceLevel(0, &m_eyeSurfaces[eye])))
		{
			DEBUG_LOG(("OpenXR: eye targets: GetSurfaceLevel FAILED for eye %d", eye));
			return FALSE;
		}

		m_eyeImages[eye] = getVulkanImage(m_eyeTextures[eye], &m_eyeImageLayout);
		if (m_eyeImages[eye] == VK_NULL_HANDLE)
		{
			DEBUG_LOG(("OpenXR: eye targets: no VkImage behind eye texture %d", eye));
			return FALSE;
		}
	}

	// The engine's own depth buffer is backbuffer-sized, which is smaller than an eye target,
	// so the eye passes need their own.
	if (FAILED(device->CreateDepthStencilSurface(m_eyeWidth, m_eyeHeight, D3DFMT_D24S8,
		D3DMULTISAMPLE_NONE, &m_depthSurface)))
	{
		DEBUG_LOG(("OpenXR: eye targets: CreateDepthStencilSurface FAILED"));
		return FALSE;
	}

	DEBUG_LOG(("OpenXR: eye targets: %d D3D8 render targets %dx%d, VkImages bound (layout %d)",
		m_eyeCount, m_eyeWidth, m_eyeHeight, (int)m_eyeImageLayout));
	return TRUE;
}

VkImage OpenXRManager::getVulkanImage(IUnknown* d3d8Resource, VkImageLayout* outLayout)
{
	ID3D9VkInteropTexture* interopTex =
		(ID3D9VkInteropTexture*)queryWrappedD3D9Interface(d3d8Resource, IID_ID3D9VkInteropTexture);
	if (interopTex == nullptr)
		return VK_NULL_HANDLE;

	VkImage image = VK_NULL_HANDLE;
	VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkImageCreateInfo info = {};
	HRESULT hr = interopTex->GetVulkanImageInfo(&image, &layout, &info);
	interopTex->Release();

	if (FAILED(hr))
		return VK_NULL_HANDLE;
	if (outLayout != nullptr)
		*outLayout = layout;
	return image;
}

//-------------------------------------------------------------------------------------------------
void OpenXRManager::initGraphics(IDirect3DDevice8* d3d8Device)
{
	if (!isAvailable() || m_session != XR_NULL_HANDLE)
		return;

	if (!findDxvkInterop(d3d8Device))
		return;
	if (!loadVulkanFunctions())
		return;
	if (!createSession())
		return;
	if (!createVulkanCopyResources())
		return;
	if (!createSwapchains())
		return;
	if (!createEyeTargets(d3d8Device))
		return;

	DEBUG_LOG(("OpenXR: graphics ready - stereo path armed"));
}

//-------------------------------------------------------------------------------------------------
void OpenXRManager::beginFrame()
{
	if (m_session == XR_NULL_HANDLE)
		return;

	for (;;)
	{
		XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
		if (xrPollEvent(m_instance, &ev) != XR_SUCCESS)
			break;

		if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
		{
			const XrEventDataSessionStateChanged* sc = (const XrEventDataSessionStateChanged*)&ev;
			m_sessionState = sc->state;
			DEBUG_LOG(("OpenXR: session state -> %d", (int)m_sessionState));

			if (m_sessionState == XR_SESSION_STATE_READY)
			{
				XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO};
				bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				XrResult br = xrBeginSession(m_session, &bi);
				m_sessionRunning = XR_SUCCEEDED(br);
				DEBUG_LOG(("OpenXR: xrBeginSession %s (%d)",
					m_sessionRunning ? "OK - session running" : "FAILED", (int)br));
			}
			else if (m_sessionState == XR_SESSION_STATE_STOPPING)
			{
				xrEndSession(m_session);
				m_sessionRunning = FALSE;
			}
			else if (m_sessionState == XR_SESSION_STATE_EXITING
				|| m_sessionState == XR_SESSION_STATE_LOSS_PENDING)
			{
				m_sessionRunning = FALSE;
			}
		}
		else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
		{
			m_sessionRunning = FALSE;
		}
	}

	// A frame was opened last tick but never rendered (game paused, render gated, window
	// minimised). Close it out with no layers, or the runtime starves waiting for it.
	if (m_frameActive)
		submitEyes();

	if (!m_sessionRunning)
		return;

	XrFrameWaitInfo waitInfo = {XR_TYPE_FRAME_WAIT_INFO};
	XrFrameState frameState = {XR_TYPE_FRAME_STATE};
	if (XR_FAILED(xrWaitFrame(m_session, &waitInfo, &frameState)))
		return;
	m_predictedDisplayTime = frameState.predictedDisplayTime;

	XrFrameBeginInfo beginInfo = {XR_TYPE_FRAME_BEGIN_INFO};
	if (XR_FAILED(xrBeginFrame(m_session, &beginInfo)))
		return;

	m_frameActive = TRUE;

	// Locate the eyes for this frame. If the runtime cannot (tracking lost), the frame is
	// still submitted - just without layers - so the session stays alive.
	XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
	locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locateInfo.displayTime = m_predictedDisplayTime;
	locateInfo.space = m_appSpace;

	XrViewState viewState = {XR_TYPE_VIEW_STATE};
	uint32_t viewCount = 0;
	XrView views[MAX_EYES];
	for (int i = 0; i < MAX_EYES; ++i)
	{
		views[i] = XrView{};
		views[i].type = XR_TYPE_VIEW;
	}

	if (XR_FAILED(xrLocateViews(m_session, &locateInfo, &viewState, MAX_EYES, &viewCount, views)))
		return;
	if ((viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) == 0
		|| (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0)
		return;

	for (Int eye = 0; eye < m_eyeCount && (uint32_t)eye < viewCount; ++eye)
	{
		m_eyePoses[eye] = views[eye].pose;
		m_eyeFovs[eye] = views[eye].fov;

		VREyeView& v = m_eyeViews[eye];
		v.quatX = views[eye].pose.orientation.x;
		v.quatY = views[eye].pose.orientation.y;
		v.quatZ = views[eye].pose.orientation.z;
		v.quatW = views[eye].pose.orientation.w;
		v.posX = views[eye].pose.position.x;
		v.posY = views[eye].pose.position.y;
		v.posZ = views[eye].pose.position.z;
		v.angleLeft = views[eye].fov.angleLeft;
		v.angleRight = views[eye].fov.angleRight;
		v.angleUp = views[eye].fov.angleUp;
		v.angleDown = views[eye].fov.angleDown;
	}
}

//-------------------------------------------------------------------------------------------------
Bool OpenXRManager::copyEyesToSwapchains(const UnsignedInt* imageIndices)
{
	VkCommandBufferBeginInfo cbBegin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	cbBegin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

	g_vk.resetCommandBuffer(m_vkCommandBuffer, 0);
	if (g_vk.beginCommandBuffer(m_vkCommandBuffer, &cbBegin) != VK_SUCCESS)
		return FALSE;

	for (Int eye = 0; eye < m_eyeCount; ++eye)
	{
		VkImage src = m_eyeImages[eye];
		VkImage dst = m_swapchainImages[eye][imageIndices[eye]];

		VkImageSubresourceRange range = {};
		range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		range.levelCount = 1;
		range.layerCount = 1;

		// src: whatever layout DXVK keeps its render targets in -> TRANSFER_SRC
		// dst: swapchain images are handed to us in an undefined layout -> TRANSFER_DST
		VkImageMemoryBarrier pre[2] = {};
		pre[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		pre[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		pre[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		pre[0].oldLayout = m_eyeImageLayout;
		pre[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		pre[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		pre[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		pre[0].image = src;
		pre[0].subresourceRange = range;

		pre[1] = pre[0];
		pre[1].srcAccessMask = 0;
		pre[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		pre[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		pre[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		pre[1].image = dst;

		g_vk.cmdPipelineBarrier(m_vkCommandBuffer,
			VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			0, 0, nullptr, 0, nullptr, 2, pre);

		VkImageCopy copy = {};
		copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copy.srcSubresource.layerCount = 1;
		copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copy.dstSubresource.layerCount = 1;
		copy.extent.width = (uint32_t)m_eyeWidth;
		copy.extent.height = (uint32_t)m_eyeHeight;
		copy.extent.depth = 1;

		g_vk.cmdCopyImage(m_vkCommandBuffer,
			src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1, &copy);

		// Restore the source to the layout DXVK believes it is in, and hand the swapchain
		// image to the compositor in the layout OpenXR requires.
		VkImageMemoryBarrier post[2] = {};
		post[0] = pre[0];
		post[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		post[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		post[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		post[0].newLayout = m_eyeImageLayout;

		post[1] = pre[1];
		post[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		post[1].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
		post[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		post[1].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		g_vk.cmdPipelineBarrier(m_vkCommandBuffer,
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			0, 0, nullptr, 0, nullptr, 2, post);
	}

	if (g_vk.endCommandBuffer(m_vkCommandBuffer) != VK_SUCCESS)
		return FALSE;

	VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &m_vkCommandBuffer;

	// DXVK owns the queue: flush its pending work (our source images were just rendered by it),
	// take the queue, submit, give it back.
	m_dxvkInterop->FlushRenderingCommands();
	m_dxvkInterop->LockSubmissionQueue();
	g_vk.resetFences(m_vkDevice, 1, &m_vkFence);
	VkResult submitResult = g_vk.queueSubmit(m_vkQueue, 1, &submit, m_vkFence);
	m_dxvkInterop->ReleaseSubmissionQueue();

	if (submitResult != VK_SUCCESS)
	{
		DEBUG_LOG(("OpenXR: eye copy: vkQueueSubmit failed (%d)", (int)submitResult));
		return FALSE;
	}

	g_vk.waitForFences(m_vkDevice, 1, &m_vkFence, VK_TRUE, 1000000000ull);
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
void OpenXRManager::submitEyes()
{
	if (!m_frameActive)
		return;

	XrCompositionLayerProjectionView projViews[MAX_EYES];
	XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
	const XrCompositionLayerBaseHeader* layers[1];
	uint32_t layerCount = 0;

	Bool copied = FALSE;
	UnsignedInt imageIndices[MAX_EYES] = {0, 0};

	if (m_swapchains[0] != XR_NULL_HANDLE && m_eyeImages[0] != VK_NULL_HANDLE)
	{
		Bool acquiredAll = TRUE;
		for (Int eye = 0; eye < m_eyeCount; ++eye)
		{
			XrSwapchainImageAcquireInfo acquireInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
			uint32_t index = 0;
			if (XR_FAILED(xrAcquireSwapchainImage(m_swapchains[eye], &acquireInfo, &index)))
			{
				acquiredAll = FALSE;
				break;
			}
			imageIndices[eye] = index;

			XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
			waitInfo.timeout = XR_INFINITE_DURATION;
			if (XR_FAILED(xrWaitSwapchainImage(m_swapchains[eye], &waitInfo)))
			{
				acquiredAll = FALSE;
				break;
			}
		}

		if (acquiredAll)
		{
			copied = copyEyesToSwapchains(imageIndices);

			for (Int eye = 0; eye < m_eyeCount; ++eye)
			{
				XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
				xrReleaseSwapchainImage(m_swapchains[eye], &releaseInfo);
			}
		}
	}

	if (copied)
	{
		for (Int eye = 0; eye < m_eyeCount; ++eye)
		{
			projViews[eye] = XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
			projViews[eye].pose = m_eyePoses[eye];
			projViews[eye].fov = m_eyeFovs[eye];
			projViews[eye].subImage.swapchain = m_swapchains[eye];
			projViews[eye].subImage.imageArrayIndex = 0;
			projViews[eye].subImage.imageRect.offset = {0, 0};
			projViews[eye].subImage.imageRect.extent = {m_eyeWidth, m_eyeHeight};
		}

		layer.space = m_appSpace;
		layer.viewCount = (uint32_t)m_eyeCount;
		layer.views = projViews;
		layers[0] = (const XrCompositionLayerBaseHeader*)&layer;
		layerCount = 1;
	}

	XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
	endInfo.displayTime = m_predictedDisplayTime;
	endInfo.environmentBlendMode = m_blendMode;
	endInfo.layerCount = layerCount;
	endInfo.layers = layerCount > 0 ? layers : nullptr;

	m_dxvkInterop->FlushRenderingCommands();
	m_dxvkInterop->LockSubmissionQueue();
	XrResult endResult = xrEndFrame(m_session, &endInfo);
	m_dxvkInterop->ReleaseSubmissionQueue();

	m_frameActive = FALSE;

	if (XR_SUCCEEDED(endResult))
	{
		if (m_framesSubmitted == 0 && layerCount > 0)
		{
			DEBUG_LOG(("OpenXR: FIRST STEREO FRAME submitted - the game is in the headset"));
		}
		++m_framesSubmitted;
		if ((m_framesSubmitted % 1000) == 0)
		{
			DEBUG_LOG(("OpenXR: %u frames submitted (layers=%u)", m_framesSubmitted, layerCount));
		}
	}
	else if (!m_submitFailLogged)
	{
		m_submitFailLogged = TRUE;
		DEBUG_LOG(("OpenXR: xrEndFrame FAILED (%d), layers=%u", (int)endResult, layerCount));
	}
}

//-------------------------------------------------------------------------------------------------
void OpenXRManager::shutdown()
{
	if (m_vkDevice != VK_NULL_HANDLE && g_vk.deviceWaitIdle != nullptr)
		g_vk.deviceWaitIdle(m_vkDevice);

	for (Int eye = 0; eye < MAX_EYES; ++eye)
	{
		if (m_eyeSurfaces[eye] != nullptr) { m_eyeSurfaces[eye]->Release(); m_eyeSurfaces[eye] = nullptr; }
		if (m_eyeTextures[eye] != nullptr) { m_eyeTextures[eye]->Release(); m_eyeTextures[eye] = nullptr; }
		if (m_swapchains[eye] != XR_NULL_HANDLE)
		{
			xrDestroySwapchain(m_swapchains[eye]);
			m_swapchains[eye] = XR_NULL_HANDLE;
		}
		m_eyeImages[eye] = VK_NULL_HANDLE;
	}
	if (m_depthSurface != nullptr) { m_depthSurface->Release(); m_depthSurface = nullptr; }

	if (m_vkDevice != VK_NULL_HANDLE)
	{
		if (m_vkFence != VK_NULL_HANDLE && g_vk.destroyFence != nullptr)
			g_vk.destroyFence(m_vkDevice, m_vkFence, nullptr);
		if (m_vkCommandPool != VK_NULL_HANDLE && g_vk.destroyCommandPool != nullptr)
			g_vk.destroyCommandPool(m_vkDevice, m_vkCommandPool, nullptr);
	}
	m_vkFence = VK_NULL_HANDLE;
	m_vkCommandPool = VK_NULL_HANDLE;
	m_vkCommandBuffer = VK_NULL_HANDLE;

	if (m_sessionRunning)
	{
		xrEndSession(m_session);
		m_sessionRunning = FALSE;
	}
	if (m_appSpace != XR_NULL_HANDLE)
	{
		xrDestroySpace(m_appSpace);
		m_appSpace = XR_NULL_HANDLE;
	}
	if (m_session != XR_NULL_HANDLE)
	{
		xrDestroySession(m_session);
		m_session = XR_NULL_HANDLE;
	}

	if (m_dxvkInterop != nullptr)
	{
		m_dxvkInterop->Release();
		m_dxvkInterop = nullptr;
	}
	m_vkInstance = VK_NULL_HANDLE;
	m_vkPhysicalDevice = VK_NULL_HANDLE;
	m_vkDevice = VK_NULL_HANDLE;
	m_vkQueue = VK_NULL_HANDLE;

	if (m_instance != XR_NULL_HANDLE)
	{
		xrDestroyInstance(m_instance);
		m_instance = XR_NULL_HANDLE;
	}
	m_systemId = XR_NULL_SYSTEM_ID;
}
