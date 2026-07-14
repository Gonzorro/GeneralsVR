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
	, m_stereoReady(FALSE)
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
	, m_copyInFlight(FALSE)
	, m_d3d8Device(nullptr)
	, m_depthSurface(nullptr)
	, m_eyeImageLayout(VK_IMAGE_LAYOUT_UNDEFINED)
	, m_uiSwapchain(XR_NULL_HANDLE)
	, m_uiTexture(nullptr)
	, m_uiSurface(nullptr)
	, m_uiCompositeTexture(nullptr)
	, m_uiCompositeSurface(nullptr)
	, m_uiWidth(0)
	, m_uiHeight(0)
	, m_uiInGame(FALSE)
	, m_uiReady(FALSE)
	, m_showFlatFrame(FALSE)
	, m_groupBarTexture(nullptr)
	, m_groupBarSurface(nullptr)
	, m_groupBarSwapchain(XR_NULL_HANDLE)
	, m_groupBarWidth(1024)
	, m_groupBarHeight(128)
	, m_groupBarReady(FALSE)
	, m_actionSet(XR_NULL_HANDLE)
	, m_aimPoseAction(XR_NULL_HANDLE)
	, m_triggerAction(XR_NULL_HANDLE)
	, m_gripAction(XR_NULL_HANDLE)
	, m_stickAction(XR_NULL_HANDLE)
	, m_primaryAction(XR_NULL_HANDLE)
	, m_secondaryAction(XR_NULL_HANDLE)
	, m_menuAction(XR_NULL_HANDLE)
	, m_stickClickAction(XR_NULL_HANDLE)
	, m_menuButtonDown(FALSE)
	, m_actionsReady(FALSE)
	, m_framesSubmitted(0)
	, m_submitFailLogged(FALSE)
{
	for (int i = 0; i < VR_HAND_COUNT; ++i)
	{
		m_handPaths[i] = XR_NULL_PATH;
		m_aimSpaces[i] = XR_NULL_HANDLE;
		m_controllers[i] = VRControllerState();
	}
	for (int i = 0; i < UI_PANEL_COUNT; ++i)
	{
		m_uiPanels[i] = UiPanel();
		m_uiPanels[i].pose.orientation.w = 1.0f;
	}
	m_wristPanelOpen[VR_HAND_LEFT] = FALSE;
	m_wristPanelOpen[VR_HAND_RIGHT] = FALSE;
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
// GeneralsVR: controller input. Everything here is best-effort - if the runtime has no
// controllers, or bindings fail, the game simply keeps playing with mouse and keyboard.
//-------------------------------------------------------------------------------------------------
Bool OpenXRManager::createActions()
{
	XrActionSetCreateInfo asci = {XR_TYPE_ACTION_SET_CREATE_INFO};
	strcpy(asci.actionSetName, "gameplay");
	strcpy(asci.localizedActionSetName, "Gameplay");
	asci.priority = 0;
	if (XR_FAILED(xrCreateActionSet(m_instance, &asci, &m_actionSet)))
	{
		DEBUG_LOG(("OpenXR: input: xrCreateActionSet failed"));
		return FALSE;
	}

	xrStringToPath(m_instance, "/user/hand/left", &m_handPaths[VR_HAND_LEFT]);
	xrStringToPath(m_instance, "/user/hand/right", &m_handPaths[VR_HAND_RIGHT]);

	struct ActionDef { XrAction* action; const char* name; const char* localized; XrActionType type; };
	const ActionDef defs[] =
	{
		{ &m_aimPoseAction, "aim_pose", "Aim Pose",      XR_ACTION_TYPE_POSE_INPUT    },
		{ &m_triggerAction, "trigger",  "Trigger",       XR_ACTION_TYPE_BOOLEAN_INPUT },
		{ &m_gripAction,    "grip",     "Grip",          XR_ACTION_TYPE_BOOLEAN_INPUT },
		{ &m_stickAction,   "stick",    "Thumbstick",    XR_ACTION_TYPE_VECTOR2F_INPUT},
		{ &m_primaryAction, "primary",  "Primary Button",XR_ACTION_TYPE_BOOLEAN_INPUT },
		{ &m_secondaryAction, "secondary", "Secondary Button", XR_ACTION_TYPE_BOOLEAN_INPUT },
		{ &m_menuAction,    "menu",     "Menu Button",   XR_ACTION_TYPE_BOOLEAN_INPUT },
		{ &m_stickClickAction, "stickclick", "Thumbstick Click", XR_ACTION_TYPE_BOOLEAN_INPUT },
	};

	for (size_t i = 0; i < sizeof(defs)/sizeof(defs[0]); ++i)
	{
		XrActionCreateInfo aci = {XR_TYPE_ACTION_CREATE_INFO};
		strcpy(aci.actionName, defs[i].name);
		strcpy(aci.localizedActionName, defs[i].localized);
		aci.actionType = defs[i].type;
		aci.countSubactionPaths = VR_HAND_COUNT;
		aci.subactionPaths = m_handPaths;
		if (XR_FAILED(xrCreateAction(m_actionSet, &aci, defs[i].action)))
		{
			DEBUG_LOG(("OpenXR: input: xrCreateAction '%s' failed", defs[i].name));
			return FALSE;
		}
	}

	// Bindings for the Touch controllers. The runtime remaps these for any other hardware it
	// supports, so this one profile is enough to get every mainstream controller working.
	struct BindingDef { XrAction action; const char* path; };
	const char* bindingPaths[] =
	{
		"/user/hand/left/input/aim/pose",            "/user/hand/right/input/aim/pose",
		"/user/hand/left/input/trigger",             "/user/hand/right/input/trigger",
		"/user/hand/left/input/squeeze/value",       "/user/hand/right/input/squeeze/value",
		"/user/hand/left/input/thumbstick",          "/user/hand/right/input/thumbstick",
		"/user/hand/left/input/x/click",             "/user/hand/right/input/a/click",
		"/user/hand/left/input/y/click",             "/user/hand/right/input/b/click",
		// The three-bar button lives on the LEFT controller only (the right one belongs to the
		// system), so both hands' menu action is bound to it.
		"/user/hand/left/input/menu/click",          "/user/hand/left/input/menu/click",
		"/user/hand/left/input/thumbstick/click",    "/user/hand/right/input/thumbstick/click",
	};
	XrAction bindingActions[] =
	{
		m_aimPoseAction, m_aimPoseAction,
		m_triggerAction, m_triggerAction,
		m_gripAction,    m_gripAction,
		m_stickAction,   m_stickAction,
		m_primaryAction, m_primaryAction,
		m_secondaryAction, m_secondaryAction,
		m_menuAction,    m_menuAction,
		m_stickClickAction, m_stickClickAction,
	};

	std::vector<XrActionSuggestedBinding> bindings;
	for (size_t i = 0; i < sizeof(bindingPaths)/sizeof(bindingPaths[0]); ++i)
	{
		XrPath path = XR_NULL_PATH;
		if (XR_FAILED(xrStringToPath(m_instance, bindingPaths[i], &path)))
			continue;

		// The menu button exists on one controller only, so its two entries collapse into one
		// binding. Suggesting the same action/path pair twice is asking for trouble.
		Bool duplicate = FALSE;
		for (size_t j = 0; j < bindings.size(); ++j)
		{
			if (bindings[j].action == bindingActions[i] && bindings[j].binding == path)
			{
				duplicate = TRUE;
				break;
			}
		}
		if (duplicate)
			continue;

		XrActionSuggestedBinding b = {};
		b.action = bindingActions[i];
		b.binding = path;
		bindings.push_back(b);
	}

	XrPath profilePath = XR_NULL_PATH;
	xrStringToPath(m_instance, "/interaction_profiles/oculus/touch_controller", &profilePath);

	XrInteractionProfileSuggestedBinding suggested = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
	suggested.interactionProfile = profilePath;
	suggested.countSuggestedBindings = (uint32_t)bindings.size();
	suggested.suggestedBindings = bindings.data();
	XrResult bindResult = xrSuggestInteractionProfileBindings(m_instance, &suggested);
	if (XR_FAILED(bindResult))
	{
		DEBUG_LOG(("OpenXR: input: suggested bindings rejected (%d)", (int)bindResult));
		return FALSE;
	}

	// One space per hand, tracking that hand's aim pose.
	for (Int hand = 0; hand < VR_HAND_COUNT; ++hand)
	{
		XrActionSpaceCreateInfo asci2 = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
		asci2.action = m_aimPoseAction;
		asci2.subactionPath = m_handPaths[hand];
		asci2.poseInActionSpace.orientation.w = 1.0f;
		if (XR_FAILED(xrCreateActionSpace(m_session, &asci2, &m_aimSpaces[hand])))
		{
			DEBUG_LOG(("OpenXR: input: xrCreateActionSpace failed for hand %d", hand));
			return FALSE;
		}
	}

	XrSessionActionSetsAttachInfo attach = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
	attach.countActionSets = 1;
	attach.actionSets = &m_actionSet;
	if (XR_FAILED(xrAttachSessionActionSets(m_session, &attach)))
	{
		DEBUG_LOG(("OpenXR: input: xrAttachSessionActionSets failed"));
		return FALSE;
	}

	m_actionsReady = TRUE;
	DEBUG_LOG(("OpenXR: input: controllers armed (aim, trigger, grip, stick, button)"));
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
void OpenXRManager::syncControllers()
{
	if (!m_actionsReady || !m_sessionRunning)
		return;

	XrActiveActionSet active = {};
	active.actionSet = m_actionSet;
	active.subactionPath = XR_NULL_PATH;

	XrActionsSyncInfo sync = {XR_TYPE_ACTIONS_SYNC_INFO};
	sync.countActiveActionSets = 1;
	sync.activeActionSets = &active;
	if (XR_FAILED(xrSyncActions(m_session, &sync)))
		return;

	// The three-bar menu button: recenter. Edge-triggered.
	{
		XrActionStateGetInfo get = {XR_TYPE_ACTION_STATE_GET_INFO};
		get.subactionPath = m_handPaths[VR_HAND_LEFT];
		get.action = m_menuAction;

		XrActionStateBoolean state = {XR_TYPE_ACTION_STATE_BOOLEAN};
		xrGetActionStateBoolean(m_session, &get, &state);
		const Bool down = state.isActive && state.currentState;
		if (down && !m_menuButtonDown)
			recenter();
		m_menuButtonDown = down;
	}

	for (Int hand = 0; hand < VR_HAND_COUNT; ++hand)
	{
		VRControllerState& c = m_controllers[hand];
		const Bool wasTrigger = c.trigger;
		const Bool wasGrip = c.grip;
		const Bool wasPrimary = c.primaryButton;

		XrActionStateGetInfo get = {XR_TYPE_ACTION_STATE_GET_INFO};
		get.subactionPath = m_handPaths[hand];

		get.action = m_triggerAction;
		XrActionStateBoolean triggerState = {XR_TYPE_ACTION_STATE_BOOLEAN};
		xrGetActionStateBoolean(m_session, &get, &triggerState);
		c.trigger = triggerState.isActive && triggerState.currentState;

		get.action = m_gripAction;
		XrActionStateBoolean gripState = {XR_TYPE_ACTION_STATE_BOOLEAN};
		xrGetActionStateBoolean(m_session, &get, &gripState);
		c.grip = gripState.isActive && gripState.currentState;

		get.action = m_primaryAction;
		XrActionStateBoolean primaryState = {XR_TYPE_ACTION_STATE_BOOLEAN};
		xrGetActionStateBoolean(m_session, &get, &primaryState);
		c.primaryButton = primaryState.isActive && primaryState.currentState;

		get.action = m_stickClickAction;
		XrActionStateBoolean stickClickState = {XR_TYPE_ACTION_STATE_BOOLEAN};
		xrGetActionStateBoolean(m_session, &get, &stickClickState);
		const Bool stickDown = stickClickState.isActive && stickClickState.currentState;
		c.stickClickPressed = stickDown && !c.stickClick;
		c.stickClick = stickDown;

		get.action = m_secondaryAction;
		XrActionStateBoolean secondaryState = {XR_TYPE_ACTION_STATE_BOOLEAN};
		xrGetActionStateBoolean(m_session, &get, &secondaryState);
		const Bool wasSecondary = c.secondaryButton;
		c.secondaryButton = secondaryState.isActive && secondaryState.currentState;
		c.secondaryPressed = c.secondaryButton && !wasSecondary;

		get.action = m_stickAction;
		XrActionStateVector2f stickState = {XR_TYPE_ACTION_STATE_VECTOR2F};
		xrGetActionStateVector2f(m_session, &get, &stickState);
		c.stickX = stickState.isActive ? stickState.currentState.x : 0.0f;
		c.stickY = stickState.isActive ? stickState.currentState.y : 0.0f;

		c.triggerPressed = c.trigger && !wasTrigger;
		c.triggerReleased = !c.trigger && wasTrigger;
		c.gripPressed = c.grip && !wasGrip;
		c.gripReleased = !c.grip && wasGrip;
		c.primaryPressed = c.primaryButton && !wasPrimary;

		// The aim pose: where the controller is pointing, in our reference space.
		get.action = m_aimPoseAction;
		XrActionStatePose poseState = {XR_TYPE_ACTION_STATE_POSE};
		xrGetActionStatePose(m_session, &get, &poseState);

		c.poseValid = FALSE;
		if (poseState.isActive && m_aimSpaces[hand] != XR_NULL_HANDLE)
		{
			XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
			if (XR_SUCCEEDED(xrLocateSpace(m_aimSpaces[hand], m_appSpace, m_predictedDisplayTime, &loc))
				&& (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
				&& (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
			{
				c.poseValid = TRUE;
				c.quatX = loc.pose.orientation.x;
				c.quatY = loc.pose.orientation.y;
				c.quatZ = loc.pose.orientation.z;
				c.quatW = loc.pose.orientation.w;
				c.posX = loc.pose.position.x;
				c.posY = loc.pose.position.y;
				c.posZ = loc.pose.position.z;
			}
		}
	}
}

//-------------------------------------------------------------------------------------------------
/** The intro movie plays inside a loop that blocks the whole engine, so none of our per-frame
	* machinery runs while it does. The engine itself handles Escape there by reaching straight
	* into the keyboard; this is the same move, reaching straight into the runtime. Edge-triggered,
	* so holding the button cannot skip several movies in a row. */
//-------------------------------------------------------------------------------------------------
Bool OpenXRManager::pollSkipRequest()
{
	if (!m_actionsReady || !m_sessionRunning)
		return FALSE;

	XrActiveActionSet active = {};
	active.actionSet = m_actionSet;
	active.subactionPath = XR_NULL_PATH;

	XrActionsSyncInfo sync = {XR_TYPE_ACTIONS_SYNC_INFO};
	sync.countActiveActionSets = 1;
	sync.activeActionSets = &active;
	if (XR_FAILED(xrSyncActions(m_session, &sync)))
		return FALSE;

	Bool skip = FALSE;

	for (Int hand = 0; hand < VR_HAND_COUNT; ++hand)
	{
		XrActionStateGetInfo get = {XR_TYPE_ACTION_STATE_GET_INFO};
		get.subactionPath = m_handPaths[hand];
		get.action = m_secondaryAction;

		XrActionStateBoolean state = {XR_TYPE_ACTION_STATE_BOOLEAN};
		if (XR_FAILED(xrGetActionStateBoolean(m_session, &get, &state)))
			continue;

		const Bool down = state.isActive && state.currentState;
		VRControllerState& c = m_controllers[hand];
		if (down && !c.secondaryButton)
			skip = TRUE;
		c.secondaryButton = down;
	}

	return skip;
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
	// The engine's own depth buffer is backbuffer-sized, which is smaller than an eye target,
	// so the eye passes need their own.
	if (FAILED(device->CreateDepthStencilSurface(m_eyeWidth, m_eyeHeight, D3DFMT_D24S8,
		D3DMULTISAMPLE_NONE, &m_depthSurface)))
	{
		DEBUG_LOG(("OpenXR: eye targets: CreateDepthStencilSurface FAILED (%dx%d)",
			m_eyeWidth, m_eyeHeight));
		return FALSE;
	}

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

		// DXVK exposes the Vulkan image on both the texture and its surface. Which one carries
		// it depends on how the D3D8 wrapper backs the resource, so try the texture and fall
		// back to the surface.
		m_eyeImages[eye] = getVulkanImage(m_eyeTextures[eye], &m_eyeImageLayout);
		if (m_eyeImages[eye] == VK_NULL_HANDLE)
		{
			DEBUG_LOG(("OpenXR: eye targets: texture %d has no VkImage, trying its surface", eye));
			m_eyeImages[eye] = getVulkanImage(m_eyeSurfaces[eye], &m_eyeImageLayout);
		}
		if (m_eyeImages[eye] == VK_NULL_HANDLE)
		{
			DEBUG_LOG(("OpenXR: eye targets: no VkImage behind eye %d - stereo unavailable", eye));
			return FALSE;
		}
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
	{
		DEBUG_LOG(("OpenXR: interop: no ID3D9VkInteropTexture behind D3D8 resource %p", d3d8Resource));
		return VK_NULL_HANDLE;
	}

	VkImage image = VK_NULL_HANDLE;
	VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;

	// DXVK requires the caller to pre-set sType (and a null pNext) or it rejects the whole
	// call with D3DERR_INVALIDCALL - a zeroed struct is not enough.
	VkImageCreateInfo info = {};
	info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	info.pNext = nullptr;

	HRESULT hr = interopTex->GetVulkanImageInfo(&image, &layout, &info);
	interopTex->Release();

	if (FAILED(hr) || image == VK_NULL_HANDLE)
	{
		DEBUG_LOG(("OpenXR: interop: GetVulkanImageInfo failed (hr=0x%08X, image=0x%llX)",
			(unsigned)hr, (unsigned long long)image));
		return VK_NULL_HANDLE;
	}

	// This is re-queried every frame per eye; only report the first few so the log stays useful.
	static Int logBudget = 4;
	if (logBudget > 0)
	{
		--logBudget;
		DEBUG_LOG(("OpenXR: interop: VkImage 0x%llX (%ux%u, format %d, layout %d, usage 0x%X, transferSrc=%d)",
			(unsigned long long)image, info.extent.width, info.extent.height,
			(int)info.format, (int)layout, (unsigned)info.usage,
			(info.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ? 1 : 0));
	}

	if (outLayout != nullptr)
		*outLayout = layout;
	return image;
}

//-------------------------------------------------------------------------------------------------
// GeneralsVR: the game's own 2D UI in VR.
//
// The engine draws its entire interface - menus, command bar, minimap, cursor - as screen-space
// 2D on top of the flat frame. Rather than re-implementing any of that, we capture the finished
// backbuffer and hang it in VR: as a cinema screen while in the menus, and as cropped wrist
// panels during a battle (an OpenXR quad layer can show a sub-rectangle of an image, so the
// minimap and the command bar are simply two different crops of the same captured frame).
//-------------------------------------------------------------------------------------------------
namespace
{
	// Rotate a vector by a quaternion.
	XrVector3f rotate(const XrQuaternionf& q, const XrVector3f& v)
	{
		const Real x = q.x, y = q.y, z = q.z, w = q.w;
		// t = 2 * cross(q.xyz, v); v' = v + w*t + cross(q.xyz, t)
		const Real tx = 2.0f * (y * v.z - z * v.y);
		const Real ty = 2.0f * (z * v.x - x * v.z);
		const Real tz = 2.0f * (x * v.y - y * v.x);
		XrVector3f out;
		out.x = v.x + w * tx + (y * tz - z * ty);
		out.y = v.y + w * ty + (z * tx - x * tz);
		out.z = v.z + w * tz + (x * ty - y * tx);
		return out;
	}

	XrQuaternionf conjugate(const XrQuaternionf& q)
	{
		XrQuaternionf out;
		out.x = -q.x; out.y = -q.y; out.z = -q.z; out.w = q.w;
		return out;
	}

	XrQuaternionf multiply(const XrQuaternionf& a, const XrQuaternionf& b)
	{
		XrQuaternionf out;
		out.w = a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z;
		out.x = a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y;
		out.y = a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x;
		out.z = a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w;
		return out;
	}

	XrQuaternionf quatFromAxisAngle(Real ax, Real ay, Real az, Real radians)
	{
		const Real h = radians * 0.5f;
		const Real s = sinf(h);
		XrQuaternionf out;
		out.x = ax * s; out.y = ay * s; out.z = az * s; out.w = cosf(h);
		return out;
	}
}

Bool OpenXRManager::createUiSwapchain()
{
	if (m_d3d8Device == nullptr)
		return FALSE;

	// Capture at the backbuffer's own size: this is the frame the game already drew.
	IDirect3DSurface8* backbuffer = nullptr;
	if (FAILED(m_d3d8Device->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &backbuffer)) || backbuffer == nullptr)
	{
		DEBUG_LOG(("OpenXR: ui: could not get the backbuffer"));
		return FALSE;
	}
	D3DSURFACE_DESC desc = {};
	backbuffer->GetDesc(&desc);
	backbuffer->Release();

	m_uiWidth = (Int)desc.Width;
	m_uiHeight = (Int)desc.Height;
	if (m_uiWidth <= 0 || m_uiHeight <= 0)
		return FALSE;

	// The engine paints its real interface into this, on a transparent background.
	if (FAILED(m_d3d8Device->CreateTexture(m_uiWidth, m_uiHeight, 1, D3DUSAGE_RENDERTARGET,
		D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_uiTexture)))
	{
		DEBUG_LOG(("OpenXR: ui: CreateTexture failed (%dx%d)", m_uiWidth, m_uiHeight));
		return FALSE;
	}
	if (FAILED(m_uiTexture->GetSurfaceLevel(0, &m_uiSurface)))
		return FALSE;

	// And this is where the black copy and the interface are put together. It has to be a
	// separate target: a texture cannot be read and written in the same draw.
	if (FAILED(m_d3d8Device->CreateTexture(m_uiWidth, m_uiHeight, 1, D3DUSAGE_RENDERTARGET,
		D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_uiCompositeTexture)))
	{
		DEBUG_LOG(("OpenXR: ui: composite CreateTexture failed"));
		return FALSE;
	}
	if (FAILED(m_uiCompositeTexture->GetSurfaceLevel(0, &m_uiCompositeSurface)))
		return FALSE;

	XrSwapchainCreateInfo swci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
	swci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	swci.format = VK_FORMAT_B8G8R8A8_SRGB;
	swci.sampleCount = 1;
	swci.width = m_uiWidth;
	swci.height = m_uiHeight;
	swci.faceCount = 1;
	swci.arraySize = 1;
	swci.mipCount = 1;

	if (XR_FAILED(xrCreateSwapchain(m_session, &swci, &m_uiSwapchain)))
	{
		DEBUG_LOG(("OpenXR: ui: xrCreateSwapchain failed (%dx%d)", m_uiWidth, m_uiHeight));
		m_uiSwapchain = XR_NULL_HANDLE;
		return FALSE;
	}

	uint32_t imageCount = 0;
	xrEnumerateSwapchainImages(m_uiSwapchain, 0, &imageCount, nullptr);
	std::vector<XrSwapchainImageVulkanKHR> images(imageCount);
	for (uint32_t i = 0; i < imageCount; ++i)
	{
		images[i] = XrSwapchainImageVulkanKHR{};
		images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
	}
	if (XR_FAILED(xrEnumerateSwapchainImages(m_uiSwapchain, imageCount, &imageCount,
		(XrSwapchainImageBaseHeader*)images.data())))
	{
		DEBUG_LOG(("OpenXR: ui: swapchain image enumeration failed"));
		return FALSE;
	}

	m_uiImages.clear();
	for (uint32_t i = 0; i < imageCount; ++i)
		m_uiImages.push_back(images[i].image);

	m_uiReady = TRUE;
	DEBUG_LOG(("OpenXR: ui: capture swapchain %dx%d, %u images", m_uiWidth, m_uiHeight, imageCount));
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
/** The control-group bar: a render target the engine draws ten numbered slots into. It hangs
	* under the wrist panel, giving squads a home without a keyboard. */
//-------------------------------------------------------------------------------------------------
Bool OpenXRManager::createGroupBar()
{
	if (m_d3d8Device == nullptr)
		return FALSE;

	if (FAILED(m_d3d8Device->CreateTexture(m_groupBarWidth, m_groupBarHeight, 1,
		D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_groupBarTexture)))
	{
		DEBUG_LOG(("OpenXR: groupbar: CreateTexture failed"));
		return FALSE;
	}
	if (FAILED(m_groupBarTexture->GetSurfaceLevel(0, &m_groupBarSurface)))
		return FALSE;

	XrSwapchainCreateInfo swci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
	swci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	swci.format = VK_FORMAT_B8G8R8A8_SRGB;
	swci.sampleCount = 1;
	swci.width = m_groupBarWidth;
	swci.height = m_groupBarHeight;
	swci.faceCount = 1;
	swci.arraySize = 1;
	swci.mipCount = 1;
	if (XR_FAILED(xrCreateSwapchain(m_session, &swci, &m_groupBarSwapchain)))
	{
		DEBUG_LOG(("OpenXR: groupbar: xrCreateSwapchain failed"));
		m_groupBarSwapchain = XR_NULL_HANDLE;
		return FALSE;
	}

	uint32_t imageCount = 0;
	xrEnumerateSwapchainImages(m_groupBarSwapchain, 0, &imageCount, nullptr);
	std::vector<XrSwapchainImageVulkanKHR> images(imageCount);
	for (uint32_t i = 0; i < imageCount; ++i)
	{
		images[i] = XrSwapchainImageVulkanKHR{};
		images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
	}
	if (XR_FAILED(xrEnumerateSwapchainImages(m_groupBarSwapchain, imageCount, &imageCount,
		(XrSwapchainImageBaseHeader*)images.data())))
		return FALSE;

	m_groupBarImages.clear();
	for (uint32_t i = 0; i < imageCount; ++i)
		m_groupBarImages.push_back(images[i].image);

	m_groupBarReady = TRUE;
	DEBUG_LOG(("OpenXR: groupbar: %dx%d ready", m_groupBarWidth, m_groupBarHeight));
	return TRUE;
}

Bool OpenXRManager::copyGroupBar(UnsignedInt imageIndex)
{
	if (!m_groupBarReady)
		return FALSE;

	VkImageLayout srcLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkImage src = getVulkanImage(m_groupBarTexture, &srcLayout);
	if (src == VK_NULL_HANDLE)
		return FALSE;

	VkImage dst = m_groupBarImages[imageIndex];

	VkImageSubresourceRange range = {};
	range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	range.levelCount = 1;
	range.layerCount = 1;

	VkImageMemoryBarrier pre[2] = {};
	pre[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	pre[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	pre[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	pre[0].oldLayout = srcLayout;
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
	copy.extent.width = (uint32_t)m_groupBarWidth;
	copy.extent.height = (uint32_t)m_groupBarHeight;
	copy.extent.depth = 1;

	g_vk.cmdCopyImage(m_vkCommandBuffer,
		src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1, &copy);

	VkImageMemoryBarrier post[2] = {};
	post[0] = pre[0];
	post[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	post[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	post[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	post[0].newLayout = srcLayout;

	post[1] = pre[1];
	post[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	post[1].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
	post[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	post[1].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	g_vk.cmdPipelineBarrier(m_vkCommandBuffer,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		0, 0, nullptr, 0, nullptr, 2, post);

	return TRUE;
}

//-------------------------------------------------------------------------------------------------
/** Move the VR origin to wherever the player is now, facing wherever they are facing. Play drifts
	* - people turn in their chair, or start the game facing the wrong way - and without this the
	* only cure is to physically shuffle back to where the session began. Only yaw is taken: tipping
	* the world to match a tilted head would be exactly the horizon-tilt we are careful to avoid. */
//-------------------------------------------------------------------------------------------------
void OpenXRManager::recenter()
{
	if (m_session == XR_NULL_HANDLE || m_appSpace == XR_NULL_HANDLE)
		return;

	// Where is the head right now, in the space we are about to replace?
	const XrPosef& head = m_eyePoses[0];

	// Keep yaw only. A quaternion's yaw about the up axis (+Y in OpenXR) comes straight out of
	// the atan2 of its Y/W terms once pitch and roll are dropped.
	const Real yaw = atan2f(2.0f * (head.orientation.w * head.orientation.y
			+ head.orientation.x * head.orientation.z),
		1.0f - 2.0f * (head.orientation.y * head.orientation.y
			+ head.orientation.x * head.orientation.x));

	XrReferenceSpaceCreateInfo spaceInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	spaceInfo.poseInReferenceSpace.orientation = quatFromAxisAngle(0.0f, 1.0f, 0.0f, yaw);
	spaceInfo.poseInReferenceSpace.position = head.position;

	XrSpace newSpace = XR_NULL_HANDLE;
	if (XR_FAILED(xrCreateReferenceSpace(m_session, &spaceInfo, &newSpace)))
	{
		DEBUG_LOG(("OpenXR: recenter FAILED"));
		return;
	}

	// The old space is still referenced by this frame's layers, so retire it only once the new
	// one is safely in place.
	XrSpace oldSpace = m_appSpace;
	m_appSpace = newSpace;
	if (oldSpace != XR_NULL_HANDLE)
		xrDestroySpace(oldSpace);

	DEBUG_LOG(("OpenXR: recentered (yaw %.0f degrees)", yaw * 57.2958f));
}

//-------------------------------------------------------------------------------------------------
void OpenXRManager::toggleWristPanel(Int hand)
{
	if (hand >= 0 && hand < VR_HAND_COUNT)
	{
		m_wristPanelOpen[hand] = !m_wristPanelOpen[hand];
		DEBUG_LOG(("OpenXR: ui: %s panel %s",
			hand == VR_HAND_LEFT ? "left" : "right",
			m_wristPanelOpen[hand] ? "opened" : "closed"));
	}
}

//-------------------------------------------------------------------------------------------------
void OpenXRManager::layoutUiPanels()
{
	for (Int i = 0; i < UI_PANEL_COUNT; ++i)
		m_uiPanels[i].active = FALSE;

	if (!m_uiReady)
		return;

	if (!m_uiInGame)
	{
		// Menus: the whole frame as a cinema screen, straight ahead at eye height. Sized so it
		// fills a comfortable chunk of the view without forcing the player to sweep their head.
		UiPanel& p = m_uiPanels[UI_PANEL_SCREEN];
		p.active = TRUE;
		p.ownerHand = -1;	// belongs to no hand: both rays may use it
		p.isGroupBar = FALSE;
		p.pose.orientation.x = p.pose.orientation.y = p.pose.orientation.z = 0.0f;
		p.pose.orientation.w = 1.0f;
		p.pose.position.x = 0.0f;
		p.pose.position.y = 0.0f;
		p.pose.position.z = -2.4f;	// the app space looks down -Z
		p.widthMeters = 3.0f;
		p.heightMeters = p.widthMeters * (Real)m_uiHeight / (Real)m_uiWidth;
		p.cropX = 0;
		p.cropY = 0;
		p.cropW = m_uiWidth;
		p.cropH = m_uiHeight;
		return;
	}

	// In a battle the panels stay OUT OF THE WAY until summoned: a panel floating permanently
	// over your hand is in the way exactly when you are moving units. Each hand's secondary
	// button (B / Y) calls up that hand's panel, which carries the game's whole bottom HUD -
	// minimap, command bar, everything - big enough to actually read and click.
	const Int wristPanelIds[VR_HAND_COUNT] = { UI_PANEL_LEFT_WRIST, UI_PANEL_RIGHT_WRIST };
	const Int groupPanelIds[VR_HAND_COUNT] = { UI_PANEL_LEFT_GROUPS, UI_PANEL_RIGHT_GROUPS };

	for (Int hand = 0; hand < VR_HAND_COUNT; ++hand)
	{
		if (!m_wristPanelOpen[hand])
			continue;

		const VRControllerState& c = m_controllers[hand];
		if (!c.poseValid)
			continue;

		const XrQuaternionf handQuat = { c.quatX, c.quatY, c.quatZ, c.quatW };

		// Tip the panel back toward the player, the way you tilt a wristwatch to read it.
		const XrQuaternionf tilt = quatFromAxisAngle(1.0f, 0.0f, 0.0f, -0.9f);	// ~50 degrees
		const XrQuaternionf panelQuat = multiply(handQuat, tilt);

		// The WHOLE interface, not a rectangle cut out of the bottom of the screen. Because the
		// engine draws it for us on a transparent background, the battlefield shows through
		// everywhere the UI is not - and a full-screen menu (the Generals promotion screen, say)
		// appears in full instead of being sliced in half.
		// Only the bottom strip - the control bar, the minimap, the money. That IS the in-game
		// interface; the rest of the screen is battlefield. Showing the whole frame meant the
		// panel's backing became a black rectangle the size of a monitor hanging in the air.
		UiPanel& p = m_uiPanels[wristPanelIds[hand]];
		p.isGroupBar = FALSE;
		p.ownerHand = hand;
		p.cropX = 0;
		// The whole frame. That used to mean a monitor-sized slab of black, but the background is
		// transparent now and only what the interface painted survives - so the panel is the
		// control bar's own shape, and a full-screen menu simply appears where it opens. No crop
		// to guess at, and nothing to detect.
		p.cropY = 0;
		p.cropW = m_uiWidth;
		p.cropH = m_uiHeight;
		p.widthMeters = 0.55f * 0.75f;	// three quarters of what it was
		p.heightMeters = p.widthMeters * (Real)p.cropH / (Real)p.cropW;
		p.pose.orientation = panelQuat;

		// Close to the hand, like something you are holding rather than something hovering
		// out of reach.
		const XrVector3f offsetLocal = { 0.0f, 0.05f, -0.13f };
		const XrVector3f offsetWorld = rotate(handQuat, offsetLocal);
		p.pose.position.x = c.posX + offsetWorld.x;
		p.pose.position.y = c.posY + offsetWorld.y;
		p.pose.position.z = c.posZ + offsetWorld.z;
		p.active = TRUE;

		// The control-group bar sits directly under it.
		if (FALSE && m_groupBarReady)	// off: it read as an unexplained black slab under the menu
		{
			UiPanel& g = m_uiPanels[groupPanelIds[hand]];
			g.isGroupBar = TRUE;
			g.ownerHand = hand;
			g.cropX = 0;
			g.cropY = 0;
			g.cropW = m_groupBarWidth;
			g.cropH = m_groupBarHeight;
			g.widthMeters = p.widthMeters;
			g.heightMeters = g.widthMeters * (Real)m_groupBarHeight / (Real)m_groupBarWidth;
			g.pose.orientation = panelQuat;

			// Just below the HUD panel, in the panel's own frame (its -Y is "down").
			const XrVector3f belowLocal = { 0.0f,
				-(p.heightMeters * 0.5f + g.heightMeters * 0.5f + 0.01f), 0.0f };
			const XrVector3f belowWorld = rotate(panelQuat, belowLocal);
			g.pose.position.x = p.pose.position.x + belowWorld.x;
			g.pose.position.y = p.pose.position.y + belowWorld.y;
			g.pose.position.z = p.pose.position.z + belowWorld.z;
			g.active = TRUE;
		}
	}
}

//-------------------------------------------------------------------------------------------------
Bool OpenXRManager::getPanelInfo(Int index, VRPanelInfo &out) const
{
	if (index < 0 || index >= UI_PANEL_COUNT)
		return FALSE;

	const UiPanel& p = m_uiPanels[index];
	if (!p.active)
		return FALSE;

	// During a movie the panel is a compositor layer showing the flat frame, not geometry: the
	// interface texture holds nothing to draw.
	if (m_showFlatFrame)
		return FALSE;

	out.quatX = p.pose.orientation.x;
	out.quatY = p.pose.orientation.y;
	out.quatZ = p.pose.orientation.z;
	out.quatW = p.pose.orientation.w;
	out.posX = p.pose.position.x;
	out.posY = p.pose.position.y;
	out.posZ = p.pose.position.z;
	out.widthMeters = p.widthMeters;
	out.heightMeters = p.heightMeters;
	out.isGroupBar = p.isGroupBar;

	const Real texW = (Real)(p.isGroupBar ? m_groupBarWidth : m_uiWidth);
	const Real texH = (Real)(p.isGroupBar ? m_groupBarHeight : m_uiHeight);
	out.u0 = (texW > 0.0f) ? (Real)p.cropX / texW : 0.0f;
	out.v0 = (texH > 0.0f) ? (Real)p.cropY / texH : 0.0f;
	out.u1 = (texW > 0.0f) ? (Real)(p.cropX + p.cropW) / texW : 1.0f;
	out.v1 = (texH > 0.0f) ? (Real)(p.cropY + p.cropH) / texH : 1.0f;
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
OpenXRManager::VRPickKind OpenXRManager::pickUiPanel(Int hand, Int &outX, Int &outY,
	Real *outDistanceMeters) const
{
	if (!m_uiReady || hand < 0 || hand >= VR_HAND_COUNT)
		return VR_PICK_NONE;

	const VRControllerState& c = m_controllers[hand];
	if (!c.poseValid)
		return VR_PICK_NONE;

	const XrQuaternionf handQuat = { c.quatX, c.quatY, c.quatZ, c.quatW };
	const XrVector3f origin = { c.posX, c.posY, c.posZ };
	const XrVector3f forwardLocal = { 0.0f, 0.0f, -1.0f };	// controllers point down their -Z
	const XrVector3f dir = rotate(handQuat, forwardLocal);

	Real bestDistance = 1.0e9f;
	VRPickKind hit = VR_PICK_NONE;

	for (Int i = 0; i < UI_PANEL_COUNT; ++i)
	{
		const UiPanel& p = m_uiPanels[i];
		if (!p.active)
			continue;

		// A hand cannot point at its own panel: the panel hangs out in front of that hand, so
		// its ray would strike it immediately and never reach anything else. You aim at a panel
		// with the OTHER hand, which is how you would hold a tablet and tap it anyway.
		if (p.ownerHand == hand)
			continue;

		// Move the ray into the panel's own frame, where the panel is the z=0 plane.
		const XrQuaternionf inv = conjugate(p.pose.orientation);
		const XrVector3f rel = { origin.x - p.pose.position.x,
		                         origin.y - p.pose.position.y,
		                         origin.z - p.pose.position.z };
		const XrVector3f localOrigin = rotate(inv, rel);
		const XrVector3f localDir = rotate(inv, dir);

		if (fabsf(localDir.z) < 0.0001f)
			continue;	// parallel to the panel

		const Real t = -localOrigin.z / localDir.z;
		if (t <= 0.0f || t >= bestDistance)
			continue;	// behind the hand, or further than a panel we already hit

		const Real hx = localOrigin.x + localDir.x * t;
		const Real hy = localOrigin.y + localDir.y * t;
		const Real halfW = p.widthMeters * 0.5f;
		const Real halfH = p.heightMeters * 0.5f;
		if (fabsf(hx) > halfW || fabsf(hy) > halfH)
			continue;	// missed the panel

		// Panel space is +X right, +Y up; image space is +X right, +Y DOWN.
		const Real u = (hx + halfW) / p.widthMeters;
		const Real v = 1.0f - (hy + halfH) / p.heightMeters;

		if (p.isGroupBar)
		{
			// Ten slots side by side: which one is under the ray?
			Int slot = (Int)(u * VR_GROUP_COUNT);
			if (slot < 0) slot = 0;
			if (slot >= VR_GROUP_COUNT) slot = VR_GROUP_COUNT - 1;
			outX = slot;
			outY = 0;
			hit = VR_PICK_GROUP_SLOT;
		}
		else
		{
			outX = p.cropX + (Int)(u * p.cropW);
			outY = p.cropY + (Int)(v * p.cropH);
			hit = VR_PICK_SCREEN;
		}

		bestDistance = t;
		if (outDistanceMeters != nullptr)
			*outDistanceMeters = t;
	}

	return hit;
}

//-------------------------------------------------------------------------------------------------
Bool OpenXRManager::captureUiFrame(UnsignedInt uiImageIndex)
{
	if (!m_uiReady || m_uiTexture == nullptr)
		return FALSE;

	// The composed panel: a black copy of the interface with the interface standing on it. While
	// a movie plays there is no interface at all - the film goes straight to the backbuffer - so
	// we show the finished flat frame instead.
	VkImageLayout srcLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkImage src = VK_NULL_HANDLE;

	if (m_showFlatFrame && m_d3d8Device != nullptr)
	{
		IDirect3DSurface8* backbuffer = nullptr;
		if (SUCCEEDED(m_d3d8Device->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &backbuffer))
			&& backbuffer != nullptr)
		{
			src = getVulkanImage(backbuffer, &srcLayout);
			backbuffer->Release();
		}
	}

	// The plain interface, as the engine drew it. The composed version - a black copy of the
	// interface standing behind itself - is the right idea and is not working yet, and a backing
	// that only half arrives is worse than no backing at all.
	if (src == VK_NULL_HANDLE)
		src = getVulkanImage(m_uiTexture, &srcLayout);

	if (src == VK_NULL_HANDLE)
		return FALSE;

	VkImage dst = m_uiImages[uiImageIndex];

	VkImageSubresourceRange range = {};
	range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	range.levelCount = 1;
	range.layerCount = 1;

	VkImageMemoryBarrier pre[2] = {};
	pre[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	pre[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	pre[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	pre[0].oldLayout = srcLayout;
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
	copy.extent.width = (uint32_t)m_uiWidth;
	copy.extent.height = (uint32_t)m_uiHeight;
	copy.extent.depth = 1;

	g_vk.cmdCopyImage(m_vkCommandBuffer,
		src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1, &copy);

	VkImageMemoryBarrier post[2] = {};
	post[0] = pre[0];
	post[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	post[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	post[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	post[0].newLayout = srcLayout;

	post[1] = pre[1];
	post[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	post[1].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
	post[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	post[1].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	g_vk.cmdPipelineBarrier(m_vkCommandBuffer,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		0, 0, nullptr, 0, nullptr, 2, post);

	return TRUE;
}

//-------------------------------------------------------------------------------------------------
void OpenXRManager::initGraphics(IDirect3DDevice8* d3d8Device)
{
	m_d3d8Device = d3d8Device;
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

	// Controllers are a bonus, not a requirement: if this fails the headset still renders and
	// the game is still playable with mouse and keyboard.
	createActions();

	if (!createSwapchains())
		return;
	if (!createEyeTargets(d3d8Device))
	{
		// The session still runs (the headset holds the app and the game plays on the
		// monitor), but no eye pass may be attempted: half the resources do not exist.
		DEBUG_LOG(("OpenXR: eye targets unavailable - VR session runs but stereo is OFF"));
		return;
	}

	// The UI panels are a bonus on top of stereo; failing to set them up must not cost us the
	// battlefield, so this is deliberately not fatal.
	if (createUiSwapchain())
		createGroupBar();

	m_stereoReady = TRUE;
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

	// A frame was opened last tick but never finished (game paused, render gated, window
	// minimised). Close it out, or the runtime starves waiting for it.
	if (m_frameActive)
		submitFrame(FALSE);

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

	// Controllers are sampled here so that everything the frame uses - eyes and hands - is
	// located against the same predicted display time.
	syncControllers();

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
Bool OpenXRManager::recordAndSubmitCopies(const UnsignedInt* imageIndices, Bool captureUi,
	UnsignedInt uiImageIndex, Bool captureGroupBar, UnsignedInt groupBarImageIndex)
{
	// Wait for OUR PREVIOUS copy to finish before touching the command buffer again. This has
	// to happen here, not after submitting: resetting or re-recording a command buffer that the
	// GPU is still executing corrupts it and takes the whole device down. The wait is unbounded
	// on purpose - a frame can legitimately take seconds while the game streams in a map or
	// plays a video, and a timeout here would put us right back into reusing a live buffer.
	if (m_copyInFlight)
	{
		VkResult waitResult = g_vk.waitForFences(m_vkDevice, 1, &m_vkFence, VK_TRUE, UINT64_MAX);
		if (waitResult != VK_SUCCESS)
		{
			DEBUG_LOG(("OpenXR: eye copy: fence wait failed (%d) - disabling stereo", (int)waitResult));
			m_stereoReady = FALSE;
			return FALSE;
		}
		m_copyInFlight = FALSE;
	}
	g_vk.resetFences(m_vkDevice, 1, &m_vkFence);

	VkCommandBufferBeginInfo cbBegin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	cbBegin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

	g_vk.resetCommandBuffer(m_vkCommandBuffer, 0);
	if (g_vk.beginCommandBuffer(m_vkCommandBuffer, &cbBegin) != VK_SUCCESS)
		return FALSE;

	for (Int eye = 0; imageIndices != nullptr && eye < m_eyeCount; ++eye)
	{
		// Re-query the eye image each frame: DXVK may recreate the backing image (a device
		// reset, a resource move), which would leave a cached handle dangling.
		VkImage src = getVulkanImage(m_eyeTextures[eye], &m_eyeImageLayout);
		if (src == VK_NULL_HANDLE)
		{
			DEBUG_LOG(("OpenXR: eye copy: eye %d lost its VkImage - disabling stereo", eye));
			m_stereoReady = FALSE;
			g_vk.endCommandBuffer(m_vkCommandBuffer);
			return FALSE;
		}
		m_eyeImages[eye] = src;

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

	// The game's finished 2D frame - and our own control-group bar - ride along in the same
	// command buffer.
	if (captureUi)
		captureUiFrame(uiImageIndex);
	if (captureGroupBar)
		copyGroupBar(groupBarImageIndex);

	if (g_vk.endCommandBuffer(m_vkCommandBuffer) != VK_SUCCESS)
		return FALSE;

	VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &m_vkCommandBuffer;

	// DXVK owns the queue: flush its pending work (our source images were just rendered by it,
	// and FlushRenderingCommands also synchronizes its CS thread, so the eye renders are
	// guaranteed to be submitted ahead of us), take the queue, submit, give it back.
	m_dxvkInterop->FlushRenderingCommands();
	m_dxvkInterop->LockSubmissionQueue();
	VkResult submitResult = g_vk.queueSubmit(m_vkQueue, 1, &submit, m_vkFence);
	m_dxvkInterop->ReleaseSubmissionQueue();

	if (submitResult != VK_SUCCESS)
	{
		// VK_ERROR_DEVICE_LOST (-4) and friends are terminal for the GPU. Turning stereo off
		// keeps the game itself alive and playable on the monitor rather than wedging it.
		DEBUG_LOG(("OpenXR: eye copy: vkQueueSubmit failed (%d) - disabling stereo", (int)submitResult));
		m_stereoReady = FALSE;
		return FALSE;
	}

	// Deliberately NOT waiting here. The runtime consumes the image on the same queue, so
	// submission order already guarantees the copy lands before the compositor reads it, and
	// blocking the render thread on the GPU every frame would cost us the frame budget.
	m_copyInFlight = TRUE;
	return TRUE;
}

void OpenXRManager::submitFrame(Bool worldRendered)
{
	if (!m_frameActive)
		return;

	XrCompositionLayerProjectionView projViews[MAX_EYES];
	XrCompositionLayerProjection worldLayer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
	XrCompositionLayerQuad quadLayers[UI_PANEL_COUNT];
	const XrCompositionLayerBaseHeader* layers[1 + UI_PANEL_COUNT];
	uint32_t layerCount = 0;

	layoutUiPanels();

	// Acquire everything we intend to write this frame, record it all into one command buffer,
	// then submit once.
	Bool haveEyes = FALSE;
	Bool haveUi = FALSE;
	UnsignedInt eyeIndices[MAX_EYES] = {0, 0};
	UnsignedInt uiIndex = 0;

	if (m_stereoReady && worldRendered)
	{
		haveEyes = TRUE;
		for (Int eye = 0; eye < m_eyeCount; ++eye)
		{
			XrSwapchainImageAcquireInfo acquireInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
			uint32_t index = 0;
			XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
			waitInfo.timeout = XR_INFINITE_DURATION;
			if (XR_FAILED(xrAcquireSwapchainImage(m_swapchains[eye], &acquireInfo, &index))
				|| XR_FAILED(xrWaitSwapchainImage(m_swapchains[eye], &waitInfo)))
			{
				haveEyes = FALSE;
				break;
			}
			eyeIndices[eye] = index;
		}
	}

	// Panels are compositor quad layers. Drawing them as geometry in the eye pass instead was an
	// attempt to let the laser sort in front of them - but it lost the panel entirely, and a
	// menu you cannot see is worse than a menu the laser hides behind. Layers work; the beam is
	// the thing to solve, not this.
	// Compositor quad layers. Drawing the panels as geometry let the laser sort in front of them,
	// but it meant reaching into the device with raw D3D calls in the middle of the eye pass, and
	// the state that leaked out of that came back as ONE EYE RENDERING FLAT. In a headset that is
	// not a cosmetic bug, it is a way to make someone ill. The beam can wait.
	Bool anyFramePanel = FALSE;
	Bool anyGroupPanel = FALSE;
	for (Int i = 0; i < UI_PANEL_COUNT; ++i)
	{
		if (!m_uiPanels[i].active)
			continue;
		if (m_uiPanels[i].isGroupBar)
			anyGroupPanel = TRUE;
		else
			anyFramePanel = TRUE;
	}

	if (m_uiReady && anyFramePanel)
	{
		XrSwapchainImageAcquireInfo acquireInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
		uint32_t index = 0;
		XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
		waitInfo.timeout = XR_INFINITE_DURATION;
		if (XR_SUCCEEDED(xrAcquireSwapchainImage(m_uiSwapchain, &acquireInfo, &index))
			&& XR_SUCCEEDED(xrWaitSwapchainImage(m_uiSwapchain, &waitInfo)))
		{
			haveUi = TRUE;
			uiIndex = index;
		}
	}

	Bool haveGroupBar = FALSE;
	UnsignedInt groupIndex = 0;
	if (m_groupBarReady && anyGroupPanel)
	{
		XrSwapchainImageAcquireInfo acquireInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
		uint32_t index = 0;
		XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
		waitInfo.timeout = XR_INFINITE_DURATION;
		if (XR_SUCCEEDED(xrAcquireSwapchainImage(m_groupBarSwapchain, &acquireInfo, &index))
			&& XR_SUCCEEDED(xrWaitSwapchainImage(m_groupBarSwapchain, &waitInfo)))
		{
			haveGroupBar = TRUE;
			groupIndex = index;
		}
	}

	Bool recorded = FALSE;
	if (haveEyes || haveUi || haveGroupBar)
	{
		recorded = recordAndSubmitCopies(haveEyes ? eyeIndices : nullptr, haveUi, uiIndex,
			haveGroupBar, groupIndex);
	}

	if (haveEyes)
	{
		for (Int eye = 0; eye < m_eyeCount; ++eye)
		{
			XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
			xrReleaseSwapchainImage(m_swapchains[eye], &releaseInfo);
		}
	}
	if (haveUi)
	{
		XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
		xrReleaseSwapchainImage(m_uiSwapchain, &releaseInfo);
	}
	if (haveGroupBar)
	{
		XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
		xrReleaseSwapchainImage(m_groupBarSwapchain, &releaseInfo);
	}

	if (recorded && haveEyes)
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

		worldLayer.space = m_appSpace;
		worldLayer.viewCount = (uint32_t)m_eyeCount;
		worldLayer.views = projViews;
		layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&worldLayer;
	}

	if (recorded)
	{
		// Panels go on top of the world, in the order they were laid out.
		for (Int i = 0; i < UI_PANEL_COUNT; ++i)
		{
			const UiPanel& p = m_uiPanels[i];
			if (!p.active)
				continue;
			if (p.isGroupBar && !haveGroupBar)
				continue;
			if (!p.isGroupBar && !haveUi)
				continue;

			XrCompositionLayerQuad& q = quadLayers[i];
			q = XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
			// Blend on the interface's own alpha, so what floats in VR is the menu's sprites and
			// not a rectangle of screen.
			q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
			q.space = m_appSpace;
			q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
			q.subImage.swapchain = p.isGroupBar ? m_groupBarSwapchain : m_uiSwapchain;
			q.subImage.imageArrayIndex = 0;
			q.subImage.imageRect.offset = {p.cropX, p.cropY};
			q.subImage.imageRect.extent = {p.cropW, p.cropH};
			q.pose = p.pose;
			q.size.width = p.widthMeters;
			q.size.height = p.heightMeters;

			layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&q;
		}
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
			DEBUG_LOG(("OpenXR: %u frames submitted (layers=%u, ui=%d)",
				m_framesSubmitted, layerCount, haveUi ? 1 : 0));
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
	// Nothing may be destroyed while our copy is still executing on the GPU.
	if (m_vkDevice != VK_NULL_HANDLE && g_vk.deviceWaitIdle != nullptr)
		g_vk.deviceWaitIdle(m_vkDevice);
	m_copyInFlight = FALSE;
	m_stereoReady = FALSE;

	if (m_uiSwapchain != XR_NULL_HANDLE)
	{
		xrDestroySwapchain(m_uiSwapchain);
		m_uiSwapchain = XR_NULL_HANDLE;
	}
	m_uiImages.clear();
	if (m_uiSurface != nullptr) { m_uiSurface->Release(); m_uiSurface = nullptr; }
	if (m_uiTexture != nullptr) { m_uiTexture->Release(); m_uiTexture = nullptr; }
	if (m_uiCompositeSurface != nullptr) { m_uiCompositeSurface->Release(); m_uiCompositeSurface = nullptr; }
	if (m_uiCompositeTexture != nullptr) { m_uiCompositeTexture->Release(); m_uiCompositeTexture = nullptr; }
	m_uiReady = FALSE;

	if (m_groupBarSwapchain != XR_NULL_HANDLE)
	{
		xrDestroySwapchain(m_groupBarSwapchain);
		m_groupBarSwapchain = XR_NULL_HANDLE;
	}
	m_groupBarImages.clear();
	if (m_groupBarSurface != nullptr) { m_groupBarSurface->Release(); m_groupBarSurface = nullptr; }
	if (m_groupBarTexture != nullptr) { m_groupBarTexture->Release(); m_groupBarTexture = nullptr; }
	m_groupBarReady = FALSE;

	m_d3d8Device = nullptr;

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
	for (Int hand = 0; hand < VR_HAND_COUNT; ++hand)
	{
		if (m_aimSpaces[hand] != XR_NULL_HANDLE)
		{
			xrDestroySpace(m_aimSpaces[hand]);
			m_aimSpaces[hand] = XR_NULL_HANDLE;
		}
	}
	if (m_actionSet != XR_NULL_HANDLE)
	{
		xrDestroyActionSet(m_actionSet);	// destroys its actions too
		m_actionSet = XR_NULL_HANDLE;
	}
	m_actionsReady = FALSE;

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
