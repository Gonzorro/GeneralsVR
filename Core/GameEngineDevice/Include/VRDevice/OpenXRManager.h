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

/// A controller's state for the current frame. Pose is in the same VR reference space as
/// VREyeView (right-handed, -Z forward, +Y up, metres): the aim pose, which points out of the
/// controller's nose the way a laser pointer would.
struct VRControllerState
{
	Bool poseValid;
	Real quatX, quatY, quatZ, quatW;
	Real posX, posY, posZ;

	Bool trigger;         ///< held
	Bool triggerPressed;  ///< became held this frame
	Bool triggerReleased; ///< became free this frame
	Bool grip;            ///< held
	Bool gripPressed;
	Bool gripReleased;
	Real stickX, stickY;  ///< -1..1
	Bool primaryButton;   ///< A / X
	Bool primaryPressed;
};

enum VRHand { VR_HAND_LEFT = 0, VR_HAND_RIGHT = 1, VR_HAND_COUNT = 2 };

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

	/// True only when every VR resource exists (session, swapchains, eye targets with their
	/// Vulkan images, depth). If any part of the graphics setup failed, this stays false and
	/// the engine must not attempt an eye pass - the session still ticks so the game runs on.
	Bool isStereoReady() const { return m_stereoReady; }

	/// True between beginFrame() and submitEyes() while the runtime wants frames. The engine
	/// only renders eyes when this AND isStereoReady() are true.
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

	/// Controller state for this frame. Always safe to read; poseValid is false when the
	/// controller is off, lost, or the runtime has no bindings for it.
	const VRControllerState& getController(Int hand) const { return m_controllers[hand]; }

	/// The D3D8 surface the engine should render this eye into (eye-sized, colour only;
	/// pair it with getDepthSurface()).
	IDirect3DSurface8* getEyeSurface(Int eye) const { return m_eyeSurfaces[eye]; }
	IDirect3DSurface8* getDepthSurface() const { return m_depthSurface; }

	/// Ends the OpenXR frame. Copies the rendered eye targets into the runtime's swapchains
	/// (when \a worldRendered) and captures the finished flat frame from the backbuffer to
	/// show the game's own 2D UI in VR - as a cinema screen in the menus, or as wrist panels
	/// during a battle. Always ends the frame, so a frame with nothing rendered still keeps
	/// the session alive.
	void submitFrame(Bool worldRendered);

	/// Menus get one big screen in front of the player; a battle gets the HUD on the wrists.
	void setUiInGame(Bool inGame) { m_uiInGame = inGame; }

	/// Cast a hand's aim ray at the UI panels. Returns the pixel in the captured frame that
	/// the ray lands on, which the engine can feed to the mouse as if it were a real cursor.
	Bool pickUiPanel(Int hand, Int &outScreenX, Int &outScreenY) const;

	/// World units per real-world metre - the tabletop scale. Head motion and eye separation
	/// are multiplied by this when composing the VR camera.
	Real getWorldUnitsPerMeter() const { return m_worldUnitsPerMeter; }

private:
	enum { MAX_EYES = 2 };

	/// A slab of the game's own 2D frame, floating in VR. \a crop selects the region of the
	/// captured frame to show, so the minimap and the command bar can be pulled out of the
	/// finished HUD and hung on the wrists without re-rendering a thing.
	struct UiPanel
	{
		XrPosef pose;                       ///< in the app reference space
		Real widthMeters, heightMeters;
		Int cropX, cropY, cropW, cropH;     ///< pixels within the captured frame
		Bool active;
	};
	enum { UI_PANEL_SCREEN = 0, UI_PANEL_LEFT_WRIST = 1, UI_PANEL_RIGHT_WRIST = 2, UI_PANEL_COUNT = 3 };

	Bool createUiSwapchain();
	void layoutUiPanels();                  ///< place the panels for this frame
	Bool captureUiFrame(UnsignedInt uiImageIndex);  ///< backbuffer -> UI swapchain image

	Bool hasExtension(const char* name) const;
	void probeVulkanRequirements();
	Bool findDxvkInterop(IDirect3DDevice8* d3d8Device);
	Bool createSession();
	Bool createActions();      ///< action set + bindings; controllers are optional, never fatal
	void syncControllers();    ///< per frame, after the frame's display time is known
	Bool createSwapchains();
	Bool createEyeTargets(IDirect3DDevice8* d3d8Device);
	Bool loadVulkanFunctions();
	Bool createVulkanCopyResources();
	/// VkImage backing a D3D8 texture/surface created by DXVK (via ID3D9VkInteropTexture).
	VkImage getVulkanImage(IUnknown* d3d8Resource, VkImageLayout* outLayout);
	/// Record the eye copies (when \a imageIndices is given) and the UI capture into one command
	/// buffer and submit it on DXVK's queue.
	Bool recordAndSubmitCopies(const UnsignedInt* imageIndices, Bool captureUi, UnsignedInt uiImageIndex);

	XrInstance m_instance;
	XrSystemId m_systemId;
	XrSession m_session;
	XrSpace m_appSpace;
	XrSessionState m_sessionState;
	XrEnvironmentBlendMode m_blendMode;
	Bool m_sessionRunning;
	Bool m_frameActive;
	Bool m_stereoReady; ///< every graphics resource exists; false = session ticks, no eye pass
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

	// Controller input (all optional: the game stays playable with mouse and keyboard)
	XrActionSet m_actionSet;
	XrAction m_aimPoseAction;
	XrAction m_triggerAction;
	XrAction m_gripAction;
	XrAction m_stickAction;
	XrAction m_primaryAction;
	XrPath m_handPaths[VR_HAND_COUNT];
	XrSpace m_aimSpaces[VR_HAND_COUNT];
	VRControllerState m_controllers[VR_HAND_COUNT];
	Bool m_actionsReady;

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
	Bool m_copyInFlight; ///< our copy command buffer is still executing; must not be re-recorded

	// Eye render targets (D3D8 side) and their Vulkan images
	IDirect3DDevice8* m_d3d8Device;   ///< borrowed; used to fetch the backbuffer each frame
	IDirect3DTexture8* m_eyeTextures[MAX_EYES];
	IDirect3DSurface8* m_eyeSurfaces[MAX_EYES];
	IDirect3DSurface8* m_depthSurface;
	VkImage m_eyeImages[MAX_EYES];
	VkImageLayout m_eyeImageLayout;

	// The game's 2D frame, captured from the backbuffer and shown on panels in VR
	XrSwapchain m_uiSwapchain;
	std::vector<VkImage> m_uiImages;
	Int m_uiWidth, m_uiHeight;
	Bool m_uiInGame;
	Bool m_uiReady;
	UiPanel m_uiPanels[UI_PANEL_COUNT];

	UnsignedInt m_framesSubmitted;
	Bool m_submitFailLogged;
};

extern OpenXRManager* TheOpenXR; ///< nullptr unless the game was launched with -vr
