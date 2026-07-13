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

// FILE: OpenXRManager.h /////////////////////////////////////////////////////////////////////////
// GeneralsVR @feature OpenXR session, swapchains and frame loop for the VR port.
//
// The game renders through DXVK (D3D8 -> Vulkan), so the OpenXR session is created over the
// VkDevice DXVK already owns (XR_KHR_vulkan_enable). Per frame the engine renders the 3D scene
// once per eye into D3D8 render targets we own; those are copied on the Vulkan level into the
// runtime's swapchain images and submitted as a projection layer.
//
// Everything degrades gracefully: with no runtime, no headset, or no DXVK the game keeps
// rendering flat and every VR entry point becomes a no-op.
///////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Lib/BaseType.h"

#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <vector>

struct IDirect3DDevice8;
struct IDirect3DTexture8;
struct IDirect3DSurface8;

/// One eye's head pose and projection for the current frame, in OpenXR conventions:
/// right-handed, -Z forward, +Y up, metres. Angles are the (signed) frustum half-angles.
struct VREyeView
{
	Real quatX, quatY, quatZ, quatW;
	Real posX, posY, posZ;          ///< metres, relative to the VR reference space origin
	Real angleLeft, angleRight;     ///< radians; left is negative
	Real angleUp, angleDown;        ///< radians; down is negative
};

class OpenXRManager
{
public:
	OpenXRManager();
	~OpenXRManager();

	/// Create the OpenXR instance and query the HMD system. Returns false when VR is
	/// unavailable (no runtime, no headset); the game must keep running flat in that case.
	Bool init();
	void shutdown();

	Bool isAvailable() const { return m_systemId != XR_NULL_SYSTEM_ID; }
	Bool hasSession() const { return m_session != XR_NULL_HANDLE; }

	/// True between beginFrame() and submitEyes() while the runtime wants frames. The engine
	/// only renders eyes when this is true.
	Bool isFrameActive() const { return m_frameActive; }

	/// Locate DXVK's Vulkan device behind the game's D3D8 device, create the session,
	/// swapchains, eye render targets and the Vulkan copy machinery. Safe to call with a
	/// null device or without DXVK: VR simply stays unavailable.
	/// Pass DX8Wrapper::_Get_D3D_Device8().
	void initGraphics(IDirect3DDevice8* d3d8Device);

	/// Per-frame, from the engine's main update, before rendering: drains runtime events,
	/// drives the session lifecycle, and (when running) starts a frame and locates the eyes.
	void beginFrame();

	Int getEyeCount() const { return m_eyeCount; }
	Int getEyeWidth() const { return m_eyeWidth; }
	Int getEyeHeight() const { return m_eyeHeight; }

	/// Valid only while isFrameActive().
	const VREyeView& getEyeView(Int eye) const { return m_eyeViews[eye]; }

	/// The D3D8 surface the engine should render this eye into (eye-sized, colour only;
	/// pair it with getDepthSurface()).
	IDirect3DSurface8* getEyeSurface(Int eye) const { return m_eyeSurfaces[eye]; }
	IDirect3DSurface8* getDepthSurface() const { return m_depthSurface; }

	/// Copy the rendered eye render targets into the runtime's swapchain images and submit
	/// them as a projection layer. Ends the frame either way, so a frame with nothing
	/// rendered still keeps the session alive.
	void submitEyes();

	/// World units per real-world metre - the tabletop scale. Head motion and eye separation
	/// are multiplied by this when composing the VR camera.
	Real getWorldUnitsPerMeter() const { return m_worldUnitsPerMeter; }

private:
	enum { MAX_EYES = 2 };

	Bool hasExtension(const char* name) const;
	void probeVulkanRequirements();
	Bool findDxvkInterop(IDirect3DDevice8* d3d8Device);
	Bool createSession();
	Bool createSwapchains();
	Bool createEyeTargets(IDirect3DDevice8* d3d8Device);
	Bool loadVulkanFunctions();
	Bool createVulkanCopyResources();
	/// VkImage backing a D3D8 texture/surface created by DXVK (via ID3D9VkInteropTexture).
	VkImage getVulkanImage(IUnknown* d3d8Resource, VkImageLayout* outLayout);
	/// Record and submit one image copy per eye on DXVK's queue.
	Bool copyEyesToSwapchains(const UnsignedInt* imageIndices);

	XrInstance m_instance;
	XrSystemId m_systemId;
	XrSession m_session;
	XrSpace m_appSpace;
	XrSessionState m_sessionState;
	XrEnvironmentBlendMode m_blendMode;
	Bool m_sessionRunning;
	Bool m_frameActive;
	XrTime m_predictedDisplayTime;

	XrSwapchain m_swapchains[MAX_EYES];
	std::vector<VkImage> m_swapchainImages[MAX_EYES];
	VREyeView m_eyeViews[MAX_EYES];
	XrPosef m_eyePoses[MAX_EYES];
	XrFovf m_eyeFovs[MAX_EYES];

	Int m_eyeCount;
	Int m_eyeWidth;
	Int m_eyeHeight;
	Real m_worldUnitsPerMeter;

	Bool m_supportsVulkan;   ///< XR_KHR_vulkan_enable2
	Bool m_supportsVulkan1;  ///< XR_KHR_vulkan_enable (accepts DXVK's existing VkDevice)
	Bool m_supportsD3D11;

	// DXVK / Vulkan interop
	struct ID3D9VkInteropDevice* m_dxvkInterop;
	VkInstance m_vkInstance;
	VkPhysicalDevice m_vkPhysicalDevice;
	VkDevice m_vkDevice;
	VkQueue m_vkQueue;
	UnsignedInt m_vkQueueIndex;
	UnsignedInt m_vkQueueFamilyIndex;
	VkCommandPool m_vkCommandPool;
	VkCommandBuffer m_vkCommandBuffer;
	VkFence m_vkFence;

	// Eye render targets (D3D8 side) and their Vulkan images
	IDirect3DTexture8* m_eyeTextures[MAX_EYES];
	IDirect3DSurface8* m_eyeSurfaces[MAX_EYES];
	IDirect3DSurface8* m_depthSurface;
	VkImage m_eyeImages[MAX_EYES];
	VkImageLayout m_eyeImageLayout;

	UnsignedInt m_framesSubmitted;
	Bool m_submitFailLogged;
};

extern OpenXRManager* TheOpenXR; ///< nullptr unless the game was launched with -vr
