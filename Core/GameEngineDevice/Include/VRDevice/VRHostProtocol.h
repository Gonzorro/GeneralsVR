// VRHostProtocol.h - shared contract between the 32-bit game and the 64-bit VR host process.
//
// WHY: Meta runtime v205/v206 crashes inside its 32-bit IPC client during xrCreateSession,
// for every graphics API (proven with standalone probes). The 64-bit runtime is unaffected
// (also proven). So OpenXR is driven from a small 64-bit host process; the 32-bit game keeps
// rendering exactly as before and hands finished eye images across the process boundary.
//
// Ownership and life cycle:
//   1. GAME creates the named shared-memory block (GeneralsVR-xrhost-shm-<gamePid>), then
//      spawns GeneralsVR-xrhost.exe with the game's pid as the only argument.
//   2. HOST opens the block, brings up OpenXR on the 64-bit runtime, publishes the adapter
//      LUID + recommended eye size, and sets state WAIT_TEXTURES.
//   3. GAME creates two exportable eye images and one shareable timeline fence on its own
//      graphics device, duplicates the three NT handles INTO the host process, writes the
//      handle values (as seen by the host) into the block, sets texturesPublished = 1.
//   4. HOST imports them and runs the session frame loop: every frame it writes poses,
//      controller state and timing into the block; whenever the game has signalled a new
//      submit it waits on the fence and copies the eye images into the OpenXR swapchains.
//   5. GAME per frame: reads poses/controllers, renders both eyes, copies into the export
//      images, signals the fence with ++gameSubmitIndex, writes the index.
//
// Resource direction (v3): the HOST creates the shared eye textures and the shared fence on
// its D3D11 device and publishes its OWN handle values; the game duplicates those handles out
// of the host process (it holds a full-access process handle from CreateProcess) and IMPORTS
// them into DXVK's Vulkan device. D3D11-created/Vulkan-imported is the direction every driver
// actually supports (it is how DXVK itself shares textures); the reverse - Vulkan-exported/
// D3D11-opened - failed with E_INVALIDARG on real hardware.
//
// This header compiles into BOTH sides (32-bit game, 64-bit host): fixed-width types only,
// no pointers, explicit packing.

#pragma once
#include <stdint.h>

#define VR_HOST_EXE_NAME        "GeneralsVR-xrhost.exe"
#define VR_HOST_SHM_NAME_FMT    "Local\\GeneralsVR-xrhost-shm-%lu"   // %lu = game process id
#define VR_HOST_PROTOCOL_VERSION 5u
#define VR_HOST_MAGIC           0x56524835u                          // 'VRH5'
#define VR_HOST_EYE_COUNT       2
#define VR_HOST_MAX_PANELS      8

// controller button bits (held state; edges are computed on the game side per game frame)
#define VR_HOST_BTN_TRIGGER    0x01u
#define VR_HOST_BTN_GRIP       0x02u
#define VR_HOST_BTN_PRIMARY    0x04u   // A / X
#define VR_HOST_BTN_SECONDARY  0x08u   // B / Y
#define VR_HOST_BTN_MENU       0x10u   // left controller three-bar button
#define VR_HOST_BTN_STICKCLICK 0x20u

#pragma pack(push, 4)

struct VRHostPose
{
	float posX, posY, posZ;             // metres, host's LOCAL reference space
	float quatX, quatY, quatZ, quatW;
	float fovLeft, fovRight, fovUp, fovDown;  // radians; per-eye asymmetric fov (eyes only)
	uint32_t valid;
};

struct VRHostController
{
	VRHostPose aimPose;                 // fov fields unused
	uint32_t buttons;                   // VR_HOST_BTN_* held bits
	float stickX, stickY;               // -1..1
	uint32_t active;                    // controller tracked this frame
};

enum VRHostState : uint32_t
{
	VRHOST_STATE_STARTING      = 0,  // host launched, not yet through OpenXR bring-up
	VRHOST_STATE_NO_RUNTIME    = 1,  // no working 64-bit runtime -> game stays flat
	VRHOST_STATE_WAIT_TEXTURES = 2,  // session up; waiting for the game's export handles
	VRHOST_STATE_RUNNING       = 3,  // frames flowing
	VRHOST_STATE_SESSION_LOST  = 4,  // headset went away; host exits, game goes flat
	VRHOST_STATE_FATAL         = 5,  // unrecoverable host error; lastError has detail
};

struct VRHostSharedBlock
{
	// --- identity (host writes magic/version once the block is mapped on its side) ---
	uint32_t magic;                  // VR_HOST_MAGIC
	uint32_t protocolVersion;        // VR_HOST_PROTOCOL_VERSION; mismatched sides must refuse
	uint32_t state;                  // VRHostState (host-owned)
	uint32_t lastError;              // XrResult / HRESULT / Win32 code for NO_RUNTIME or FATAL

	// --- host -> game, published once after bring-up ---
	uint32_t adapterLuidLow;         // the GPU the runtime demands; game must render on it
	int32_t  adapterLuidHigh;
	uint32_t eyeWidth;               // export images MUST be exactly this size, BGRA8
	uint32_t eyeHeight;
	uint32_t runtimeMajor, runtimeMinor, runtimePatch;

	// --- shared GPU resources, HOST-created (v3 direction) ---
	uint32_t gamePid;
	uint32_t hostResourcesReady;     // host sets 1 after the handles below are valid
	uint32_t texturesPublished;      // game sets 1 after it imported everything (game is ready)
	uint32_t eyeTextureHandle[VR_HOST_EYE_COUNT];  // NT handles, valid IN THE HOST process; game duplicates them out
	uint32_t fenceHandle;            // shared D3D11 fence handle, valid in the host; game imports as a timeline semaphore

	// --- per-frame, host -> game ---
	// frameSeq is a seqlock: the host bumps it to ODD before writing this block and to EVEN
	// after. The game copies the block and retries until it saw a stable even value - a torn
	// read here is a one-frame pose spike (wobble, phantom input).
	uint32_t frameSeq;
	uint32_t pad0;
	uint64_t hostFrameIndex;         // increments every xrWaitFrame; the game PACES on this
	int64_t  predictedDisplayTimeNs; // XrTime of the frame being prepared
	uint32_t sessionFocused;         // 1 = headset on head, session visible+focused
	uint32_t pad1;
	VRHostPose headPose;
	VRHostPose eyePose[VR_HOST_EYE_COUNT];
	VRHostController controller[VR_HOST_EYE_COUNT];  // [0]=left, [1]=right

	// --- per-frame, game -> host ---
	uint64_t gameSubmitIndex;        // ++ after both eye copies are queued; also the fence value signalled
	uint64_t hostConsumedIndex;      // host sets = the submit it last displayed
	VRHostPose renderPose[VR_HOST_EYE_COUNT]; // the eye poses+fov THIS submit was rendered with;
	                                 // the host must stamp the layer with these, not its current
	                                 // poses, or the compositor mis-reprojects (camera wobble)
	uint32_t recenterRequests;       // game ++ to ask for a view recenter
	uint32_t gameRequestsStop;       // game sets 1; host exits cleanly

	// --- hosted UI panels (v4): the game's interface as floating quads in the headset ---
	uint32_t uiRequest;              // game sets 1 after writing uiWidth/uiHeight
	uint32_t uiWidth, uiHeight;      // the game's interface capture size
	uint32_t uiResourcesReady;       // host created the shared UI texture below
	uint32_t uiTextureHandle;        // host-local NT handle of the shared UI texture
	uint32_t uiConnected;            // game imported it; the panel list below is live
	uint32_t panelCount;             // entries used in panels[]
	struct VRHostPanel
	{
		VRHostPose pose;             // panel centre in the host's LOCAL space (fov unused)
		float widthMeters, heightMeters;
		float u0, v0, u1, v1;        // crop of the UI texture, as fractions
		float alpha;
		uint32_t visible;
		uint32_t isGroupBar;         // group-bar panels are not shown by hosted v1
		uint32_t onTop;
	} panels[VR_HOST_MAX_PANELS];
};

#pragma pack(pop)

// Eye images are BGRA8; the host's swapchains use the same format so the per-frame move is a
// plain GPU copy with no conversion.
#define VR_HOST_EYE_FORMAT_DXGI 87u  // DXGI_FORMAT_B8G8R8A8_UNORM
