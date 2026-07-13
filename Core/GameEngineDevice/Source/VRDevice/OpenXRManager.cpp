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
// GeneralsVR @feature OpenXR instance/system bootstrap. See OpenXRManager.h.
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "VRDevice/OpenXRManager.h"
#include "VRDevice/DxvkInterop.h"

#include "Common/Debug.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string.h>
#include <vector>

OpenXRManager* TheOpenXR = nullptr;

OpenXRManager::OpenXRManager()
	: m_instance(XR_NULL_HANDLE)
	, m_systemId(XR_NULL_SYSTEM_ID)
	, m_eyeWidth(0)
	, m_eyeHeight(0)
	, m_supportsVulkan(FALSE)
	, m_supportsVulkan1(FALSE)
	, m_supportsD3D11(FALSE)
	, m_dxvkInterop(nullptr)
	, m_vkInstance(nullptr)
	, m_vkPhysicalDevice(nullptr)
	, m_vkDevice(nullptr)
	, m_vkQueue(nullptr)
	, m_vkQueueIndex(0)
	, m_vkQueueFamilyIndex(0)
{
}

OpenXRManager::~OpenXRManager()
{
	shutdown();
}

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

Bool OpenXRManager::init()
{
	m_supportsVulkan = hasExtension("XR_KHR_vulkan_enable2");
	m_supportsVulkan1 = hasExtension("XR_KHR_vulkan_enable");
	m_supportsD3D11 = hasExtension("XR_KHR_D3D11_enable");
	DEBUG_LOG(("OpenXR: runtime graphics bindings: vulkan2=%d vulkan1=%d d3d11=%d",
		m_supportsVulkan, m_supportsVulkan1, m_supportsD3D11));

	if (!m_supportsVulkan && !m_supportsVulkan1 && !m_supportsD3D11)
	{
		DEBUG_LOG(("OpenXR: no usable graphics binding extension, VR unavailable"));
		return FALSE;
	}

	// GeneralsVR: vulkan_enable (v1) is our primary path - it accepts the VkDevice DXVK
	// already created, whereas vulkan_enable2 requires the runtime to create the device.
	std::vector<const char*> enabledExts;
	if (m_supportsVulkan1)
		enabledExts.push_back("XR_KHR_vulkan_enable");

	XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
	strcpy(ci.applicationInfo.applicationName, "GeneralsVR");
	ci.applicationInfo.applicationVersion = 1;
	strcpy(ci.applicationInfo.engineName, "SAGE-W3D");
	ci.applicationInfo.engineVersion = 1;
	ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	ci.enabledExtensionCount = (uint32_t)enabledExts.size();
	ci.enabledExtensionNames = enabledExts.empty() ? nullptr : enabledExts.data();

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
	if (viewCount > 0)
	{
		std::vector<XrViewConfigurationView> views(viewCount);
		for (uint32_t i = 0; i < viewCount; ++i)
		{
			views[i] = XrViewConfigurationView{};
			views[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
		}
		if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews(m_instance, m_systemId,
			XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, views.data())))
		{
			m_eyeWidth = views[0].recommendedImageRectWidth;
			m_eyeHeight = views[0].recommendedImageRectHeight;
			DEBUG_LOG(("OpenXR: %u views, recommended eye target %dx%d", viewCount, m_eyeWidth, m_eyeHeight));
		}
	}

	if (m_supportsVulkan1)
		probeVulkanRequirements();

	DEBUG_LOG(("OpenXR: bootstrap complete, VR available"));
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
// GeneralsVR: XR_KHR_vulkan_enable function types and the graphics-requirements struct, declared
// locally so the bootstrap needs no Vulkan headers (openxr_platform.h would demand vulkan.h).
typedef XrResult (XRAPI_PTR *PFN_local_xrGetVulkanExtensionsKHR)(
	XrInstance, XrSystemId, uint32_t, uint32_t*, char*);

struct LocalXrGraphicsRequirementsVulkanKHR
{
	XrStructureType type;
	void* next;
	XrVersion minApiVersionSupported;
	XrVersion maxApiVersionSupported;
};
typedef XrResult (XRAPI_PTR *PFN_local_xrGetVulkanGraphicsRequirementsKHR)(
	XrInstance, XrSystemId, LocalXrGraphicsRequirementsVulkanKHR*);

void OpenXRManager::probeVulkanRequirements()
{
	PFN_local_xrGetVulkanGraphicsRequirementsKHR pGetReqs = nullptr;
	PFN_local_xrGetVulkanExtensionsKHR pGetInstExts = nullptr;
	PFN_local_xrGetVulkanExtensionsKHR pGetDevExts = nullptr;
	xrGetInstanceProcAddr(m_instance, "xrGetVulkanGraphicsRequirementsKHR", (PFN_xrVoidFunction*)&pGetReqs);
	xrGetInstanceProcAddr(m_instance, "xrGetVulkanInstanceExtensionsKHR", (PFN_xrVoidFunction*)&pGetInstExts);
	xrGetInstanceProcAddr(m_instance, "xrGetVulkanDeviceExtensionsKHR", (PFN_xrVoidFunction*)&pGetDevExts);

	if (pGetReqs != nullptr)
	{
		LocalXrGraphicsRequirementsVulkanKHR reqs = {};
		reqs.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR;
		if (XR_SUCCEEDED(pGetReqs(m_instance, m_systemId, &reqs)))
		{
			DEBUG_LOG(("OpenXR: vulkan API version required: min %u.%u.%u max %u.%u.%u",
				XR_VERSION_MAJOR(reqs.minApiVersionSupported), XR_VERSION_MINOR(reqs.minApiVersionSupported),
				XR_VERSION_PATCH(reqs.minApiVersionSupported),
				XR_VERSION_MAJOR(reqs.maxApiVersionSupported), XR_VERSION_MINOR(reqs.maxApiVersionSupported),
				XR_VERSION_PATCH(reqs.maxApiVersionSupported)));
		}
	}

	struct { const char* label; PFN_local_xrGetVulkanExtensionsKHR fn; } queries[] =
	{
		{ "instance", pGetInstExts },
		{ "device",   pGetDevExts },
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
// GeneralsVR: DXVK's D3D8 device wraps its D3D9 device as a private member with no public
// accessor (verified against dxvk v3.0.1 and master 2026-07). We recover it by scanning the
// first few pointer slots of the D3D8Device object for a pointer whose vtable lives inside
// d3d9.dll and which answers QueryInterface(ID3D9VkInteropDevice). Every dereference is
// SEH-guarded and the vtable-module check runs before any call, so a miss is just a log line.

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

static IUnknown* tryQueryInteropDevice(void* candidate)
{
	__try
	{
		IUnknown* unk = (IUnknown*)candidate;
		void* out = nullptr;
		if (SUCCEEDED(unk->QueryInterface(IID_ID3D9VkInteropDevice, &out)) && out != nullptr)
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

void OpenXRManager::probeDxvkInterop(void* d3d8Device)
{
	if (d3d8Device == nullptr)
	{
		DEBUG_LOG(("OpenXR: dxvk probe: no D3D8 device, skipping"));
		return;
	}

	HMODULE d3d9Module = GetModuleHandleA("d3d9.dll");
	if (d3d9Module == nullptr)
	{
		DEBUG_LOG(("OpenXR: dxvk probe: d3d9.dll not loaded - not running under DXVK d3d8, skipping"));
		return;
	}

	ID3D9VkInteropDevice* interop = nullptr;
	for (int offset = sizeof(void*); offset <= 64 && interop == nullptr; offset += sizeof(void*))
	{
		void* candidate = readPtrGuarded((char*)d3d8Device + offset);
		if (candidate == nullptr || candidate == d3d8Device)
			continue;
		if (!vtableLivesInModule(candidate, d3d9Module))
			continue;
		interop = (ID3D9VkInteropDevice*)tryQueryInteropDevice(candidate);
		if (interop != nullptr)
		{
			DEBUG_LOG(("OpenXR: dxvk probe: found D3D9 device at D3D8Device+%d, interop acquired", offset));
		}
	}

	if (interop == nullptr)
	{
		DEBUG_LOG(("OpenXR: dxvk probe: no ID3D9VkInteropDevice found behind the D3D8 device"));
		return;
	}

	m_dxvkInterop = interop;
	interop->GetVulkanHandles(&m_vkInstance, &m_vkPhysicalDevice, &m_vkDevice);
	uint32_t queueIndex = 0, queueFamily = 0;
	VkQueue queue = nullptr;
	interop->GetSubmissionQueue(&queue, &queueIndex, &queueFamily);
	m_vkQueue = queue;
	m_vkQueueIndex = queueIndex;
	m_vkQueueFamilyIndex = queueFamily;

	DEBUG_LOG(("OpenXR: dxvk vulkan handles: instance=%p physicalDevice=%p device=%p queue=%p family=%u index=%u",
		m_vkInstance, m_vkPhysicalDevice, m_vkDevice, m_vkQueue, m_vkQueueFamilyIndex, m_vkQueueIndex));
}

void OpenXRManager::shutdown()
{
	if (m_dxvkInterop != nullptr)
	{
		m_dxvkInterop->Release();
		m_dxvkInterop = nullptr;
	}
	m_vkInstance = nullptr;
	m_vkPhysicalDevice = nullptr;
	m_vkDevice = nullptr;
	m_vkQueue = nullptr;

	if (m_instance != XR_NULL_HANDLE)
	{
		xrDestroyInstance(m_instance);
		m_instance = XR_NULL_HANDLE;
	}
	m_systemId = XR_NULL_SYSTEM_ID;
}
