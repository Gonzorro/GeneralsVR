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

#include "Common/Debug.h"

#include <string.h>
#include <vector>

OpenXRManager* TheOpenXR = nullptr;

OpenXRManager::OpenXRManager()
	: m_instance(XR_NULL_HANDLE)
	, m_systemId(XR_NULL_SYSTEM_ID)
	, m_eyeWidth(0)
	, m_eyeHeight(0)
	, m_supportsVulkan(FALSE)
	, m_supportsD3D11(FALSE)
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
	m_supportsD3D11 = hasExtension("XR_KHR_D3D11_enable");
	DEBUG_LOG(("OpenXR: runtime graphics bindings: vulkan2=%d d3d11=%d", m_supportsVulkan, m_supportsD3D11));

	if (!m_supportsVulkan && !m_supportsD3D11)
	{
		DEBUG_LOG(("OpenXR: no usable graphics binding extension, VR unavailable"));
		return FALSE;
	}

	XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
	strcpy(ci.applicationInfo.applicationName, "GeneralsVR");
	ci.applicationInfo.applicationVersion = 1;
	strcpy(ci.applicationInfo.engineName, "SAGE-W3D");
	ci.applicationInfo.engineVersion = 1;
	ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;

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

	DEBUG_LOG(("OpenXR: bootstrap complete, VR available"));
	return TRUE;
}

void OpenXRManager::shutdown()
{
	if (m_instance != XR_NULL_HANDLE)
	{
		xrDestroyInstance(m_instance);
		m_instance = XR_NULL_HANDLE;
	}
	m_systemId = XR_NULL_SYSTEM_ID;
}
