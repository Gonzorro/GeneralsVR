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
	Bool primaryButton;   ///< A / X  - hold while clicking a group slot to ASSIGN it
	Bool primaryPressed;
	Bool secondaryButton; ///< B / Y  - left one forces attack; right one toggles the panel
	Bool secondaryPressed;
	Bool stickClick;      ///< pressing the thumbstick in
	Bool stickClickPressed;
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

	/// Is the player asking to skip the intro movie? Movies play inside a blocking loop that
	/// never reaches our per-frame update, so this polls the runtime directly - it is the VR
	/// equivalent of the engine reaching straight into the keyboard for Escape.
	Bool pollSkipRequest();

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

	/// In a battle each hand's panel is hidden until the player summons it with that hand's
	/// secondary button, so it never floats in the way while they are moving units.
	void toggleWristPanel(Int hand);
	Bool isWristPanelOpen(Int hand) const { return m_wristPanelOpen[hand]; }

	/// What a hand's ray is currently pointing at.
	enum VRPickKind { VR_PICK_NONE = 0, VR_PICK_SCREEN, VR_PICK_GROUP_SLOT };

	/// Cast a hand's aim ray at the UI panels.
	/// VR_PICK_SCREEN     -> outX/outY is the pixel of the game's own frame under the ray, which
	///                       the engine can feed to the mouse as if it were a real cursor.
	/// VR_PICK_GROUP_SLOT -> outX is the control group (0-9) under the ray.
	/// Also reports how far away the hit was, so the laser can be drawn stopping at the panel.
	VRPickKind pickUiPanel(Int hand, Int &outX, Int &outY, Real *outDistanceMeters = nullptr) const;

	/// The surface the engine draws the game's real 2D interface into, once per frame, with a
	/// transparent background. Showing THAT beats copying the finished frame: a crop of the
	/// backbuffer can only ever show a rectangle of whatever the flat game happened to draw,
	/// which is useless the moment a full-screen menu (the Generals promotion screen, say)
	/// appears. Here we get the real windows, sprites and all, on a clear background.
	IDirect3DSurface8* getUiSurface() const { return m_uiSurface; }
	Bool hasUiSurface() const { return m_uiSurface != nullptr; }

	/// Where the finished panel is composed: a black copy of the interface, with the interface
	/// itself standing on top of it. This is what the headset actually sees.
	IDirect3DSurface8* getUiCompositeSurface() const { return m_uiCompositeSurface; }
	IDirect3DTexture8* getUiTexture() const { return m_uiTexture; }

	/// The composed panel as a TEXTURE - what the renderer hangs on the quads in the eye pass.
	IDirect3DTexture8* getUiCompositeTexture() const { return m_uiCompositeTexture; }

	/// TRUE while the intro film is playing, when there is no interface to draw and the panel
	/// falls back to a compositor layer showing the finished flat frame.
	Bool isShowingFlatFrame() const { return m_showFlatFrame; }

	/// While a movie plays there IS no interface to draw - the film is painted straight to the
	/// backbuffer - so the VR screen shows the finished flat frame instead. Without this the
	/// headset just holds the last thing it saw while the intro plays on the monitor.
	void setShowFlatFrame(Bool showFlatFrame) { m_showFlatFrame = showFlatFrame; }

	/// Move the VR origin to where the player is now: forward becomes the way they are facing.
	void recenter();

	/// A panel, as the renderer needs to see it: the engine draws these as real 3D quads in the
	/// eye pass rather than letting the compositor slap them on top as flat layers. A layer has
	/// no depth, so it buried the laser pointing at it and turned the interface into a hard
	/// rectangle floating over the world.
	struct VRPanelInfo
	{
		Real quatX, quatY, quatZ, quatW;    ///< pose in the VR reference space, metres
		Real posX, posY, posZ;
		Real widthMeters, heightMeters;
		Real u0, v0, u1, v1;                ///< the region of the source texture to show
		Bool isGroupBar;                    ///< which texture: the group bar, or the interface
	};
	Int getPanelCount() const { return UI_PANEL_COUNT; }
	Bool getPanelInfo(Int index, VRPanelInfo &out) const;

	IDirect3DTexture8* getGroupBarTexture() const { return m_groupBarTexture; }

	/// The control-group bar the engine draws for us (10 slots), shown on the wrist panel.
	IDirect3DSurface8* getGroupBarSurface() const { return m_groupBarSurface; }
	Int getGroupBarWidth() const { return m_groupBarWidth; }
	Int getGroupBarHeight() const { return m_groupBarHeight; }
	Bool hasGroupBar() const { return m_groupBarSurface != nullptr; }

	/// World units per real-world metre - the tabletop scale. Head motion and eye separation
	/// are multiplied by this when composing the VR camera.
	Real getWorldUnitsPerMeter() const { return m_worldUnitsPerMeter; }

	/// Resize the player relative to the world. This MUST be how the scale changes: the value is
	/// cached here, so writing it into GlobalData - which is only read once, at startup - looked
	/// like it worked and did nothing at all.
	void setWorldUnitsPerMeter(Real scale) { if (scale > 0.0f) m_worldUnitsPerMeter = scale; }

private:
	enum { MAX_EYES = 2 };

	/// A slab of the game's own 2D frame, floating in VR. \a crop selects the region of the
	/// captured frame to show, so the minimap and the command bar can be pulled out of the
	/// finished HUD and hung on the wrists without re-rendering a thing.
	struct UiPanel
	{
		XrPosef pose;                       ///< in the app reference space
		Real widthMeters, heightMeters;
		Int cropX, cropY, cropW, cropH;     ///< pixels within the source image
		Bool active;
		Bool isGroupBar;                    ///< draws from the group-bar swapchain, not the frame
		Int ownerHand;                      ///< the hand it hangs off, or -1 for the fixed screen
	};
	enum { UI_PANEL_SCREEN = 0, UI_PANEL_LEFT_WRIST = 1, UI_PANEL_RIGHT_WRIST = 2,
	       UI_PANEL_LEFT_GROUPS = 3, UI_PANEL_RIGHT_GROUPS = 4, UI_PANEL_COUNT = 5 };
	enum { VR_GROUP_COUNT = 10 };

	Bool createUiSwapchain();
	Bool createGroupBar();
	void layoutUiPanels();                  ///< place the panels for this frame
	Bool captureUiFrame(UnsignedInt uiImageIndex);  ///< backbuffer -> UI swapchain image
	Bool copyGroupBar(UnsignedInt imageIndex);      ///< our drawn bar -> its swapchain image

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
	Bool recordAndSubmitCopies(const UnsignedInt* imageIndices, Bool captureUi, UnsignedInt uiImageIndex,
		Bool captureGroupBar, UnsignedInt groupBarImageIndex);

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
	XrAction m_secondaryAction;
	XrAction m_menuAction;      ///< the three-bar button: recenter
	XrAction m_stickClickAction;
	Bool m_menuButtonDown;
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
	IDirect3DTexture8* m_uiTexture;   ///< the engine draws the real interface here
	IDirect3DSurface8* m_uiSurface;
	IDirect3DTexture8* m_uiCompositeTexture;  ///< black copy + interface, composed
	IDirect3DSurface8* m_uiCompositeSurface;
	Int m_uiWidth, m_uiHeight;
	Bool m_uiInGame;
	Bool m_uiReady;
	Bool m_showFlatFrame;   ///< capture the backbuffer, not the UI layer (movies)
	UiPanel m_uiPanels[UI_PANEL_COUNT];
	Bool m_wristPanelOpen[VR_HAND_COUNT];

	// The control-group bar: a render target the engine draws ten numbered slots into, shown
	// under the wrist panel so squads can be saved and recalled without a keyboard.
	IDirect3DTexture8* m_groupBarTexture;
	IDirect3DSurface8* m_groupBarSurface;
	XrSwapchain m_groupBarSwapchain;
	std::vector<VkImage> m_groupBarImages;
	Int m_groupBarWidth, m_groupBarHeight;
	Bool m_groupBarReady;

	UnsignedInt m_framesSubmitted;
	Bool m_submitFailLogged;
};

extern OpenXRManager* TheOpenXR; ///< nullptr unless the game was launched with -vr
