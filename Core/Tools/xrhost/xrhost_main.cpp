// GeneralsVR-xrhost - the 64-bit VR host process.
//
// Meta's v205+ runtime crashes 32-bit session creation inside its own IPC client, so the
// 32-bit game cannot talk OpenXR directly any more. This process does it instead: it owns
// the instance/session/swapchains/actions on the (working) 64-bit runtime, receives the
// game's rendered eye images through cross-process GPU sharing, and ships poses + controller
// state back through shared memory. Protocol: VRHostProtocol.h. Spawned by the game with the
// game's PID as argv[1]; exits when the game dies, asks to stop, or the session is lost.

#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <cstdio>
#include <cstdint>
#include <vector>

#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "../../GameEngineDevice/Include/VRDevice/VRHostProtocol.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

static FILE* g_logFile;
static void L(const char* fmt, ...)
{
	char buf[1024]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
	printf("%s\n", buf); fflush(stdout);
	if (g_logFile) { fprintf(g_logFile, "%s\n", buf); fflush(g_logFile); }
}

struct Host
{
	VRHostSharedBlock* shm = nullptr;
	HANDLE gameProcess = nullptr;

	XrInstance instance = XR_NULL_HANDLE;
	XrSystemId systemId = XR_NULL_SYSTEM_ID;
	XrSession session = XR_NULL_HANDLE;
	XrSpace space = XR_NULL_HANDLE;
	XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
	bool sessionRunning = false;

	ID3D11Device* dev = nullptr;
	ID3D11DeviceContext* ctx = nullptr;
	ID3D11DeviceContext4* ctx4 = nullptr;

	XrSwapchain swapchain[VR_HOST_EYE_COUNT] = {};
	std::vector<ID3D11Texture2D*> swapTex[VR_HOST_EYE_COUNT];
	uint32_t eyeW = 0, eyeH = 0;

	ID3D11Texture2D* gameEye[VR_HOST_EYE_COUNT] = {};   // host-created, shared with the game
	ID3D11Fence* fence = nullptr;                       // host-created, shared with the game
	bool resourcesShared = false;
	bool gameConnected = false;

	// hosted UI panels (v4): one shared texture carrying the game's interface, shown as quads
	ID3D11Texture2D* uiTex = nullptr;
	XrSwapchain uiSwapchain = XR_NULL_HANDLE;
	std::vector<ID3D11Texture2D*> uiSwapTex;
	uint32_t uiW = 0, uiH = 0;
	bool uiCreated = false;
	int64_t swapchainFormat = 0;

	// input
	XrActionSet actionSet = XR_NULL_HANDLE;
	XrAction aimPose = XR_NULL_HANDLE, trigger = XR_NULL_HANDLE, grip = XR_NULL_HANDLE,
		stick = XR_NULL_HANDLE, primary = XR_NULL_HANDLE, secondary = XR_NULL_HANDLE,
		menu = XR_NULL_HANDLE, stickClick = XR_NULL_HANDLE;
	XrPath handPath[2] = {};
	XrSpace aimSpace[2] = {};

	uint32_t recenterSeen = 0;

	// XR_FB_display_refresh_rate (optional): lets the game ask the headset for 72/80/90/120 Hz
	PFN_xrEnumerateDisplayRefreshRatesFB pEnumRefreshRates = nullptr;
	PFN_xrGetDisplayRefreshRateFB pGetRefreshRate = nullptr;
	PFN_xrRequestDisplayRefreshRateFB pRequestRefreshRate = nullptr;
	uint32_t refreshSeen = 0;
};

static void writePose(VRHostPose& out, const XrPosef& p, uint32_t valid)
{
	out.posX = p.position.x; out.posY = p.position.y; out.posZ = p.position.z;
	out.quatX = p.orientation.x; out.quatY = p.orientation.y;
	out.quatZ = p.orientation.z; out.quatW = p.orientation.w;
	out.valid = valid;
}

static bool createActions(Host& h)
{
	XrActionSetCreateInfo asci = { XR_TYPE_ACTION_SET_CREATE_INFO };
	strcpy(asci.actionSetName, "gameplay");
	strcpy(asci.localizedActionSetName, "Gameplay");
	if (XR_FAILED(xrCreateActionSet(h.instance, &asci, &h.actionSet))) return false;

	xrStringToPath(h.instance, "/user/hand/left", &h.handPath[0]);
	xrStringToPath(h.instance, "/user/hand/right", &h.handPath[1]);

	struct { XrAction* a; const char* n; const char* l; XrActionType t; } defs[] = {
		{ &h.aimPose, "aim_pose", "Aim Pose", XR_ACTION_TYPE_POSE_INPUT },
		{ &h.trigger, "trigger", "Trigger", XR_ACTION_TYPE_BOOLEAN_INPUT },
		{ &h.grip, "grip", "Grip", XR_ACTION_TYPE_BOOLEAN_INPUT },
		{ &h.stick, "stick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT },
		{ &h.primary, "primary", "Primary Button", XR_ACTION_TYPE_BOOLEAN_INPUT },
		{ &h.secondary, "secondary", "Secondary Button", XR_ACTION_TYPE_BOOLEAN_INPUT },
		{ &h.menu, "menu", "Menu Button", XR_ACTION_TYPE_BOOLEAN_INPUT },
		{ &h.stickClick, "stickclick", "Thumbstick Click", XR_ACTION_TYPE_BOOLEAN_INPUT },
	};
	for (auto& d : defs)
	{
		XrActionCreateInfo aci = { XR_TYPE_ACTION_CREATE_INFO };
		strcpy(aci.actionName, d.n); strcpy(aci.localizedActionName, d.l);
		aci.actionType = d.t;
		aci.countSubactionPaths = 2; aci.subactionPaths = h.handPath;
		if (XR_FAILED(xrCreateAction(h.actionSet, &aci, d.a))) return false;
	}

	// identical binding table to the in-game manager (oculus touch profile; runtime remaps others)
	const char* paths[] = {
		"/user/hand/left/input/aim/pose",         "/user/hand/right/input/aim/pose",
		"/user/hand/left/input/trigger",          "/user/hand/right/input/trigger",
		"/user/hand/left/input/squeeze/value",    "/user/hand/right/input/squeeze/value",
		"/user/hand/left/input/thumbstick",       "/user/hand/right/input/thumbstick",
		"/user/hand/left/input/x/click",          "/user/hand/right/input/a/click",
		"/user/hand/left/input/y/click",          "/user/hand/right/input/b/click",
		"/user/hand/left/input/menu/click",       nullptr,
		"/user/hand/left/input/thumbstick/click", "/user/hand/right/input/thumbstick/click",
	};
	XrAction acts[] = {
		h.aimPose, h.aimPose, h.trigger, h.trigger, h.grip, h.grip, h.stick, h.stick,
		h.primary, h.primary, h.secondary, h.secondary, h.menu, h.menu, h.stickClick, h.stickClick,
	};
	std::vector<XrActionSuggestedBinding> bindings;
	for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i)
	{
		if (!paths[i]) continue;
		XrPath p = XR_NULL_PATH;
		if (XR_FAILED(xrStringToPath(h.instance, paths[i], &p))) continue;
		bindings.push_back({ acts[i], p });
	}
	XrPath profile = XR_NULL_PATH;
	xrStringToPath(h.instance, "/interaction_profiles/oculus/touch_controller", &profile);
	XrInteractionProfileSuggestedBinding sug = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
	sug.interactionProfile = profile;
	sug.countSuggestedBindings = (uint32_t)bindings.size();
	sug.suggestedBindings = bindings.data();
	if (XR_FAILED(xrSuggestInteractionProfileBindings(h.instance, &sug))) return false;

	for (int hand = 0; hand < 2; ++hand)
	{
		XrActionSpaceCreateInfo si = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
		si.action = h.aimPose; si.subactionPath = h.handPath[hand];
		si.poseInActionSpace.orientation.w = 1.0f;
		if (XR_FAILED(xrCreateActionSpace(h.session, &si, &h.aimSpace[hand]))) return false;
	}
	XrSessionActionSetsAttachInfo at = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	at.countActionSets = 1; at.actionSets = &h.actionSet;
	return XR_SUCCEEDED(xrAttachSessionActionSets(h.session, &at));
}

static void pumpControllers(Host& h, XrTime t)
{
	XrActiveActionSet active = { h.actionSet, XR_NULL_PATH };
	XrActionsSyncInfo sync = { XR_TYPE_ACTIONS_SYNC_INFO };
	sync.countActiveActionSets = 1; sync.activeActionSets = &active;
	if (XR_FAILED(xrSyncActions(h.session, &sync))) return;

	for (int hand = 0; hand < 2; ++hand)
	{
		VRHostController& c = h.shm->controller[hand];
		c.buttons = 0;

		XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
		gi.subactionPath = h.handPath[hand];
		XrActionStateBoolean b = { XR_TYPE_ACTION_STATE_BOOLEAN };
		struct { XrAction a; uint32_t bit; } bools[] = {
			{ h.trigger, VR_HOST_BTN_TRIGGER }, { h.grip, VR_HOST_BTN_GRIP },
			{ h.primary, VR_HOST_BTN_PRIMARY }, { h.secondary, VR_HOST_BTN_SECONDARY },
			{ h.menu, VR_HOST_BTN_MENU }, { h.stickClick, VR_HOST_BTN_STICKCLICK },
		};
		bool anyActive = false;
		for (auto& bd : bools)
		{
			gi.action = bd.a;
			if (XR_SUCCEEDED(xrGetActionStateBoolean(h.session, &gi, &b)))
			{
				if (b.isActive) anyActive = true;
				if (b.isActive && b.currentState) c.buttons |= bd.bit;
			}
		}
		gi.action = h.stick;
		XrActionStateVector2f v = { XR_TYPE_ACTION_STATE_VECTOR2F };
		if (XR_SUCCEEDED(xrGetActionStateVector2f(h.session, &gi, &v)) && v.isActive)
		{ c.stickX = v.currentState.x; c.stickY = v.currentState.y; anyActive = true; }
		else { c.stickX = 0; c.stickY = 0; }

		XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
		if (XR_SUCCEEDED(xrLocateSpace(h.aimSpace[hand], h.space, t, &loc))
			&& (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)
			&& (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
			writePose(c.aimPose, loc.pose, 1);
		else
			c.aimPose.valid = 0;
		c.active = anyActive ? 1u : 0u;
	}
}

// v3: the host CREATES the shared eye textures + fence (the direction every driver supports)
// and publishes its local handle values; the game duplicates them out and imports into Vulkan.
static bool createSharedResources(Host& h)
{
	ID3D11Device5* dev5 = nullptr;
	h.dev->QueryInterface(__uuidof(ID3D11Device5), (void**)&dev5);
	if (!dev5) { L("no ID3D11Device5 - cannot create a shared fence"); h.shm->lastError = 1; return false; }

	for (int eye = 0; eye < VR_HOST_EYE_COUNT; ++eye)
	{
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = h.eyeW; td.Height = h.eyeH;
		td.MipLevels = 1; td.ArraySize = 1;
		td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
		HRESULT hr = h.dev->CreateTexture2D(&td, nullptr, &h.gameEye[eye]);
		if (FAILED(hr))
		{
			// Some runtimes insist NTHANDLE comes with a keyed mutex. We sync through the
			// fence, so the mutex is acquired once here and simply held for the host's life.
			td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
			hr = h.dev->CreateTexture2D(&td, nullptr, &h.gameEye[eye]);
			if (FAILED(hr)) { L("shared eye %d failed 0x%08lx", eye, hr); h.shm->lastError = (uint32_t)hr; return false; }
			IDXGIKeyedMutex* km = nullptr;
			h.gameEye[eye]->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&km);
			if (km) km->AcquireSync(0, 0);
			L("eye %d created WITH keyed mutex (held permanently; sync is the fence)", eye);
		}
		IDXGIResource1* res = nullptr;
		h.gameEye[eye]->QueryInterface(__uuidof(IDXGIResource1), (void**)&res);
		HANDLE hTex = nullptr;
		hr = res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &hTex);
		if (FAILED(hr)) { L("CreateSharedHandle eye %d failed 0x%08lx", eye, hr); h.shm->lastError = (uint32_t)hr; return false; }
		h.shm->eyeTextureHandle[eye] = (uint32_t)(uintptr_t)hTex;
	}

	HRESULT hr = dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), (void**)&h.fence);
	if (FAILED(hr)) { L("shared fence failed 0x%08lx", hr); h.shm->lastError = (uint32_t)hr; return false; }
	HANDLE hFence = nullptr;
	hr = h.fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &hFence);
	if (FAILED(hr)) { L("fence CreateSharedHandle failed 0x%08lx", hr); h.shm->lastError = (uint32_t)hr; return false; }
	h.shm->fenceHandle = (uint32_t)(uintptr_t)hFence;

	h.resourcesShared = true;
	h.shm->hostResourcesReady = 1;
	L("shared eye textures + fence created and published");
	return true;
}

// v4: shared texture + swapchain for the game's interface panels, created on the game's request
// (the game knows the interface size). Same NTHANDLE / keyed-mutex-fallback pattern as the eyes.
static bool createUiShared(Host& h)
{
	h.uiW = h.shm->uiWidth; h.uiH = h.shm->uiHeight;
	if (h.uiW == 0 || h.uiH == 0) return false;

	D3D11_TEXTURE2D_DESC td = {};
	td.Width = h.uiW; td.Height = h.uiH;
	td.MipLevels = 1; td.ArraySize = 1;
	td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
	HRESULT hr = h.dev->CreateTexture2D(&td, nullptr, &h.uiTex);
	if (FAILED(hr))
	{
		td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
		hr = h.dev->CreateTexture2D(&td, nullptr, &h.uiTex);
		if (FAILED(hr)) { L("shared UI texture failed 0x%08lx", hr); return false; }
		IDXGIKeyedMutex* km = nullptr;
		h.uiTex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&km);
		if (km) km->AcquireSync(0, 0);
	}
	IDXGIResource1* res = nullptr;
	h.uiTex->QueryInterface(__uuidof(IDXGIResource1), (void**)&res);
	HANDLE hTex = nullptr;
	hr = res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &hTex);
	if (FAILED(hr)) { L("UI CreateSharedHandle failed 0x%08lx", hr); return false; }

	XrSwapchainCreateInfo sw = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	sw.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	sw.format = h.swapchainFormat;
	sw.sampleCount = 1; sw.faceCount = 1; sw.arraySize = 1; sw.mipCount = 1;
	sw.width = h.uiW; sw.height = h.uiH;
	if (XR_FAILED(xrCreateSwapchain(h.session, &sw, &h.uiSwapchain))) { L("UI swapchain failed"); return false; }
	uint32_t n = 0; xrEnumerateSwapchainImages(h.uiSwapchain, 0, &n, nullptr);
	std::vector<XrSwapchainImageD3D11KHR> imgs(n, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
	xrEnumerateSwapchainImages(h.uiSwapchain, n, &n, (XrSwapchainImageBaseHeader*)imgs.data());
	for (uint32_t i = 0; i < n; ++i) h.uiSwapTex.push_back(imgs[i].texture);

	h.shm->uiTextureHandle = (uint32_t)(uintptr_t)hTex;
	h.shm->uiResourcesReady = 1;
	h.uiCreated = true;
	L("shared UI texture %ux%u + swapchain published", h.uiW, h.uiH);
	return true;
}

int main(int argc, char** argv)
{
	g_logFile = fopen("xrhost_log.txt", "w");
	L("=== GeneralsVR-xrhost (64-bit) starting ===");
	if (argc < 2) { L("usage: %s <game pid>", VR_HOST_EXE_NAME); return 1; }
	DWORD gamePid = (DWORD)strtoul(argv[1], nullptr, 10);

	char shmName[128];
	sprintf(shmName, VR_HOST_SHM_NAME_FMT, (unsigned long)gamePid);
	HANDLE mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, shmName);
	if (!mapping) { L("shared memory '%s' not found (start me from the game)", shmName); return 2; }

	Host h;
	h.shm = (VRHostSharedBlock*)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(VRHostSharedBlock));
	if (!h.shm) { L("MapViewOfFile failed"); return 3; }
	h.shm->magic = VR_HOST_MAGIC;
	h.shm->protocolVersion = VR_HOST_PROTOCOL_VERSION;
	h.shm->state = VRHOST_STATE_STARTING;
	h.gameProcess = OpenProcess(SYNCHRONIZE, FALSE, gamePid);

	// --- OpenXR bring-up (the recipe both 64-bit probes proved) ---
	// Refresh-rate control is an optional extension: probe for it, enable it when the runtime
	// has it, and say so in the log either way - whether Link exposes it decides whether the
	// game's Headset Hz setting can work at all.
	const char* exts[2] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };
	uint32_t extCount = 1;
	bool haveRefreshExt = false;
	{
		uint32_t n = 0;
		xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
		std::vector<XrExtensionProperties> props(n, { XR_TYPE_EXTENSION_PROPERTIES });
		if (n > 0 && XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(nullptr, n, &n, props.data())))
			for (uint32_t i = 0; i < n; ++i)
				if (strcmp(props[i].extensionName, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME) == 0)
					haveRefreshExt = true;
		L("%s: %s", XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME, haveRefreshExt ? "available" : "NOT offered by this runtime");
		if (haveRefreshExt) exts[extCount++] = XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME;
	}
	XrInstanceCreateInfo ici = { XR_TYPE_INSTANCE_CREATE_INFO };
	strcpy(ici.applicationInfo.applicationName, "GeneralsVR");
	ici.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
	ici.enabledExtensionCount = extCount; ici.enabledExtensionNames = exts;
	if (XR_FAILED(xrCreateInstance(&ici, &h.instance)))
	{ h.shm->state = VRHOST_STATE_NO_RUNTIME; L("xrCreateInstance failed"); return 4; }

	XrInstanceProperties ip = { XR_TYPE_INSTANCE_PROPERTIES };
	xrGetInstanceProperties(h.instance, &ip);
	h.shm->runtimeMajor = XR_VERSION_MAJOR(ip.runtimeVersion);
	h.shm->runtimeMinor = XR_VERSION_MINOR(ip.runtimeVersion);
	h.shm->runtimePatch = XR_VERSION_PATCH(ip.runtimeVersion);
	L("runtime '%s' %u.%u.%u", ip.runtimeName, h.shm->runtimeMajor, h.shm->runtimeMinor, h.shm->runtimePatch);

	XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO };
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	if (XR_FAILED(xrGetSystem(h.instance, &sgi, &h.systemId)))
	{ h.shm->state = VRHOST_STATE_NO_RUNTIME; L("xrGetSystem failed (no headset)"); return 5; }

	uint32_t vc = 0;
	xrEnumerateViewConfigurationViews(h.instance, h.systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &vc, nullptr);
	std::vector<XrViewConfigurationView> vcv(vc, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
	xrEnumerateViewConfigurationViews(h.instance, h.systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, vc, &vc, vcv.data());
	h.eyeW = vcv[0].recommendedImageRectWidth;
	h.eyeH = vcv[0].recommendedImageRectHeight;

	PFN_xrGetD3D11GraphicsRequirementsKHR pReq = nullptr;
	xrGetInstanceProcAddr(h.instance, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction*)&pReq);
	XrGraphicsRequirementsD3D11KHR req = { XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
	pReq(h.instance, h.systemId, &req);

	IDXGIFactory1* fac = nullptr; CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac);
	IDXGIAdapter1* ad = nullptr; IDXGIAdapter1* use = nullptr;
	for (UINT i = 0; fac->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; ++i)
	{
		DXGI_ADAPTER_DESC1 d; ad->GetDesc1(&d);
		if (d.AdapterLuid.LowPart == req.adapterLuid.LowPart && d.AdapterLuid.HighPart == req.adapterLuid.HighPart) { use = ad; break; }
		ad->Release();
	}
	if (FAILED(D3D11CreateDevice(use, use ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
		nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &h.dev, nullptr, &h.ctx)))
	{ h.shm->state = VRHOST_STATE_FATAL; L("D3D11 device failed"); return 6; }
	h.ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void**)&h.ctx4);
	if (!h.ctx4) { h.shm->state = VRHOST_STATE_FATAL; L("no ID3D11DeviceContext4 (needs Win10+)"); return 7; }
	h.shm->adapterLuidLow = req.adapterLuid.LowPart;
	h.shm->adapterLuidHigh = req.adapterLuid.HighPart;

	XrGraphicsBindingD3D11KHR gb = { XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
	gb.device = h.dev;
	XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO };
	sci.next = &gb; sci.systemId = h.systemId;
	if (XR_FAILED(xrCreateSession(h.instance, &sci, &h.session)))
	{ h.shm->state = VRHOST_STATE_NO_RUNTIME; L("xrCreateSession failed"); return 8; }
	L("session created (64-bit path)");

	// Publish the refresh-rate menu before the game connects: by the time it sees our resources
	// it can already read what the headset offers. Count 0 = no extension = no choice to offer.
	if (haveRefreshExt)
	{
		xrGetInstanceProcAddr(h.instance, "xrEnumerateDisplayRefreshRatesFB", (PFN_xrVoidFunction*)&h.pEnumRefreshRates);
		xrGetInstanceProcAddr(h.instance, "xrGetDisplayRefreshRateFB", (PFN_xrVoidFunction*)&h.pGetRefreshRate);
		xrGetInstanceProcAddr(h.instance, "xrRequestDisplayRefreshRateFB", (PFN_xrVoidFunction*)&h.pRequestRefreshRate);
		if (h.pEnumRefreshRates)
		{
			uint32_t n = 0;
			h.pEnumRefreshRates(h.session, 0, &n, nullptr);
			std::vector<float> rates(n);
			if (n > 0 && XR_SUCCEEDED(h.pEnumRefreshRates(h.session, n, &n, rates.data())))
			{
				if (n > VR_HOST_MAX_REFRESH_RATES) n = VR_HOST_MAX_REFRESH_RATES;
				char list[160] = ""; size_t at = 0;
				for (uint32_t i = 0; i < n; ++i)
				{
					h.shm->refreshRates[i] = rates[i];
					at += snprintf(list + at, sizeof(list) - at, "%s%.0f", i ? ", " : "", rates[i]);
				}
				h.shm->refreshRateCount = n;
				L("display refresh rates offered: %s Hz", list);
			}
			else
				L("xrEnumerateDisplayRefreshRatesFB returned no rates");
		}
		float cur = 0.0f;
		if (h.pGetRefreshRate && XR_SUCCEEDED(h.pGetRefreshRate(h.session, &cur)))
		{ h.shm->currentRefreshRate = cur; L("current display refresh rate: %.0f Hz", cur); }
	}

	XrReferenceSpaceCreateInfo rsci = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	rsci.poseInReferenceSpace.orientation.w = 1.0f;
	xrCreateReferenceSpace(h.session, &rsci, &h.space);

	// The game's eye images carry sRGB-encoded bytes (same as its monitor output). An _SRGB
	// swapchain makes the compositor read them as such; UNORM would wash the picture out.
	// CopyResource between UNORM and UNORM_SRGB is legal (same DXGI format family).
	int64_t chosenFormat = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
	{
		uint32_t nf = 0;
		xrEnumerateSwapchainFormats(h.session, 0, &nf, nullptr);
		std::vector<int64_t> formats(nf);
		xrEnumerateSwapchainFormats(h.session, nf, &nf, formats.data());
		bool haveSrgb = false, haveUnorm = false;
		for (int64_t f : formats)
		{
			if (f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) haveSrgb = true;
			if (f == DXGI_FORMAT_B8G8R8A8_UNORM) haveUnorm = true;
		}
		if (!haveSrgb && haveUnorm) chosenFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
		L("swapchain format: %lld (srgb available: %d)", (long long)chosenFormat, haveSrgb ? 1 : 0);
	}
	h.swapchainFormat = chosenFormat;
	for (int eye = 0; eye < VR_HOST_EYE_COUNT; ++eye)
	{
		XrSwapchainCreateInfo sw = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
		sw.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
		sw.format = chosenFormat;
		sw.sampleCount = 1; sw.faceCount = 1; sw.arraySize = 1; sw.mipCount = 1;
		sw.width = h.eyeW; sw.height = h.eyeH;
		if (XR_FAILED(xrCreateSwapchain(h.session, &sw, &h.swapchain[eye])))
		{ h.shm->state = VRHOST_STATE_FATAL; L("swapchain failed"); return 9; }
		uint32_t n = 0; xrEnumerateSwapchainImages(h.swapchain[eye], 0, &n, nullptr);
		std::vector<XrSwapchainImageD3D11KHR> imgs(n, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
		xrEnumerateSwapchainImages(h.swapchain[eye], n, &n, (XrSwapchainImageBaseHeader*)imgs.data());
		for (uint32_t i = 0; i < n; ++i) h.swapTex[eye].push_back(imgs[i].texture);
	}

	if (!createActions(h)) L("controller actions failed - continuing without input");

	h.shm->eyeWidth = h.eyeW;
	h.shm->eyeHeight = h.eyeH;
	if (!createSharedResources(h))
	{
		h.shm->state = VRHOST_STATE_FATAL;
		return 10;
	}
	h.shm->state = VRHOST_STATE_WAIT_TEXTURES;
	L("publishing eye size %ux%u, waiting for the game to import", h.eyeW, h.eyeH);

	// --- main loop: ride the session lifecycle, never quit on a mere STOPPING ---
	uint64_t frame = 0;
	bool quit = false;
	while (!quit)
	{
		if (h.shm->gameRequestsStop) { L("game asked to stop"); break; }
		if (h.gameProcess && WaitForSingleObject(h.gameProcess, 0) == WAIT_OBJECT_0)
		{ L("game process ended"); break; }

		XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
		while (xrPollEvent(h.instance, &ev) == XR_SUCCESS)
		{
			if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
			{
				h.sessionState = ((XrEventDataSessionStateChanged*)&ev)->state;
				if (h.sessionState == XR_SESSION_STATE_READY && !h.sessionRunning)
				{
					XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO };
					bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
					if (XR_SUCCEEDED(xrBeginSession(h.session, &bi))) { h.sessionRunning = true; L("session running"); }
				}
				else if (h.sessionState == XR_SESSION_STATE_STOPPING && h.sessionRunning)
				{
					// headset idle / dash focus: end politely and WAIT - the runtime hands the
					// session back with a fresh READY when the player returns.
					xrEndSession(h.session);
					h.sessionRunning = false;
					L("session paused by the runtime (waiting for it to come back)");
				}
				else if (h.sessionState == XR_SESSION_STATE_EXITING || h.sessionState == XR_SESSION_STATE_LOSS_PENDING)
				{ L("session lost"); h.shm->state = VRHOST_STATE_SESSION_LOST; quit = true; }
			}
			else if (ev.type == XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB)
			{
				const XrEventDataDisplayRefreshRateChangedFB* rr = (const XrEventDataDisplayRefreshRateChangedFB*)&ev;
				h.shm->currentRefreshRate = rr->toDisplayRefreshRate;
				L("display refresh rate changed: %.0f -> %.0f Hz", rr->fromDisplayRefreshRate, rr->toDisplayRefreshRate);
			}
			ev = { XR_TYPE_EVENT_DATA_BUFFER };
		}
		if (quit) break;
		if (!h.sessionRunning) { Sleep(15); continue; }

		if (h.shm->uiRequest && !h.uiCreated)
		{
			if (!createUiShared(h))
				h.shm->uiRequest = 2;   // permanent failure marker; game stops waiting
		}

		XrFrameWaitInfo fwi = { XR_TYPE_FRAME_WAIT_INFO };
		XrFrameState fs = { XR_TYPE_FRAME_STATE };
		if (XR_FAILED(xrWaitFrame(h.session, &fwi, &fs))) { Sleep(5); continue; }
		XrFrameBeginInfo fbi = { XR_TYPE_FRAME_BEGIN_INFO };
		xrBeginFrame(h.session, &fbi);

		XrViewLocateInfo vli = { XR_TYPE_VIEW_LOCATE_INFO };
		vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		vli.displayTime = fs.predictedDisplayTime; vli.space = h.space;
		XrViewState vst = { XR_TYPE_VIEW_STATE };
		XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
		uint32_t nviews = 0;
		xrLocateViews(h.session, &vli, &vst, 2, &nviews, views);

		// publish frame data for the game (seqlock: odd while writing, even when stable)
		h.shm->frameSeq++;
		h.shm->predictedDisplayTimeNs = fs.predictedDisplayTime;
		h.shm->sessionFocused = (h.sessionState == XR_SESSION_STATE_FOCUSED) ? 1u : 0u;
		for (int eye = 0; eye < 2; ++eye)
		{
			writePose(h.shm->eyePose[eye], views[eye].pose,
				(vst.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) ? 1u : 0u);
			h.shm->eyePose[eye].fovLeft = views[eye].fov.angleLeft;
			h.shm->eyePose[eye].fovRight = views[eye].fov.angleRight;
			h.shm->eyePose[eye].fovUp = views[eye].fov.angleUp;
			h.shm->eyePose[eye].fovDown = views[eye].fov.angleDown;
		}
		// head = midpoint of the eyes, orientation of eye 0 (good enough for the game's uses)
		h.shm->headPose = h.shm->eyePose[0];
		h.shm->headPose.posX = 0.5f * (h.shm->eyePose[0].posX + h.shm->eyePose[1].posX);
		h.shm->headPose.posY = 0.5f * (h.shm->eyePose[0].posY + h.shm->eyePose[1].posY);
		h.shm->headPose.posZ = 0.5f * (h.shm->eyePose[0].posZ + h.shm->eyePose[1].posZ);
		pumpControllers(h, fs.predictedDisplayTime);
		h.shm->hostFrameIndex = ++frame;
		h.shm->frameSeq++;

		// The game's Headset Hz wish. It is only ever a REQUEST: the runtime may refuse (result
		// logged), and the actual rate comes back through the CHANGED event above. Polled once
		// per request too, for runtimes that switch without sending the event.
		if (h.shm->refreshRateRequestSeq != h.refreshSeen)
		{
			h.refreshSeen = h.shm->refreshRateRequestSeq;
			if (h.pRequestRefreshRate)
			{
				const float want = h.shm->requestedRefreshRate;
				XrResult r = h.pRequestRefreshRate(h.session, want);
				h.shm->refreshRateResult = (int32_t)r;
				L("refresh rate request %.0f Hz -> %s (%d)", want, XR_SUCCEEDED(r) ? "accepted" : "REFUSED", (int)r);
				float cur = 0.0f;
				if (h.pGetRefreshRate && XR_SUCCEEDED(h.pGetRefreshRate(h.session, &cur)))
					h.shm->currentRefreshRate = cur;
			}
			h.shm->refreshRateAppliedSeq = h.refreshSeen;
		}

		if (h.shm->recenterRequests != h.recenterSeen)
		{
			// Same maths as the game's own recenter: the new space sits at the head's position,
			// rotated to its current yaw (pitch and roll stay world-aligned). Recreating a plain
			// identity LOCAL space would be a no-op - Meta pins LOCAL to the session start.
			h.recenterSeen = h.shm->recenterRequests;
			const XrPosef& head = views[0].pose;
			float yaw = atan2f(
				2.0f * (head.orientation.w * head.orientation.y
					+ head.orientation.x * head.orientation.z),
				1.0f - 2.0f * (head.orientation.y * head.orientation.y
					+ head.orientation.x * head.orientation.x));
			XrReferenceSpaceCreateInfo rc = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
			rc.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
			rc.poseInReferenceSpace.orientation.y = sinf(yaw * 0.5f);
			rc.poseInReferenceSpace.orientation.w = cosf(yaw * 0.5f);
			rc.poseInReferenceSpace.position = head.position;
			XrSpace fresh = XR_NULL_HANDLE;
			if (XR_SUCCEEDED(xrCreateReferenceSpace(h.session, &rc, &fresh)))
			{ xrDestroySpace(h.space); h.space = fresh; L("recentered (yaw %.0f deg)", yaw * 57.2958f); }
		}

		bool haveFrame = false;
		if (h.resourcesShared && h.shm->texturesPublished)
		{
			if (!h.gameConnected) { h.gameConnected = true; h.shm->state = VRHOST_STATE_RUNNING; L("running - game connected, frames flowing"); }
			// Never stall the compositor waiting for the game: show the newest submit whose
			// copies have finished on the GPU; if none is new, the previous content is still in
			// the shared textures and gets shown again.
			uint64_t want = h.shm->gameSubmitIndex;
			uint64_t completed = h.fence->GetCompletedValue();
			if (want > completed) want = completed;
			if (want > 0)
			{
				haveFrame = true;
				h.shm->hostConsumedIndex = want;
			}
		}

		XrCompositionLayerProjectionView pv[2];
		for (int eye = 0; eye < 2; ++eye)
		{
			uint32_t idx = 0;
			XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
			xrAcquireSwapchainImage(h.swapchain[eye], &ai, &idx);
			XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
			wi.timeout = XR_INFINITE_DURATION;
			xrWaitSwapchainImage(h.swapchain[eye], &wi);
			if (haveFrame)
				h.ctx->CopyResource(h.swapTex[eye][idx], h.gameEye[eye]);
			else
			{
				ID3D11RenderTargetView* rtv = nullptr;
				if (SUCCEEDED(h.dev->CreateRenderTargetView(h.swapTex[eye][idx], nullptr, &rtv)))
				{ float black[4] = { 0, 0, 0, 1 }; h.ctx->ClearRenderTargetView(rtv, black); rtv->Release(); }
			}
			XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
			xrReleaseSwapchainImage(h.swapchain[eye], &ri);

			pv[eye] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
			// Stamp the layer with the pose the game RENDERED with (echoed per submit), so the
			// compositor's reprojection corrects head motion instead of fighting it. Without a
			// game frame yet, fall back to the current views.
			const VRHostPose& rp = h.shm->renderPose[eye];
			if (haveFrame && rp.valid)
			{
				pv[eye].pose.orientation.x = rp.quatX; pv[eye].pose.orientation.y = rp.quatY;
				pv[eye].pose.orientation.z = rp.quatZ; pv[eye].pose.orientation.w = rp.quatW;
				pv[eye].pose.position.x = rp.posX; pv[eye].pose.position.y = rp.posY;
				pv[eye].pose.position.z = rp.posZ;
				pv[eye].fov.angleLeft = rp.fovLeft; pv[eye].fov.angleRight = rp.fovRight;
				pv[eye].fov.angleUp = rp.fovUp; pv[eye].fov.angleDown = rp.fovDown;
			}
			else
			{
				pv[eye].pose = views[eye].pose;
				pv[eye].fov = views[eye].fov;
			}
			pv[eye].subImage.swapchain = h.swapchain[eye];
			pv[eye].subImage.imageRect.extent.width = (int32_t)h.eyeW;
			pv[eye].subImage.imageRect.extent.height = (int32_t)h.eyeH;
		}
		XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
		layer.space = h.space; layer.viewCount = 2; layer.views = pv;
		const XrCompositionLayerBaseHeader* layers[1 + VR_HOST_MAX_PANELS];
		uint32_t layerCount = 0;
		layers[layerCount++] = (XrCompositionLayerBaseHeader*)&layer;

		// the game's interface as floating quads (v4)
		XrCompositionLayerQuad quads[VR_HOST_MAX_PANELS];
		if (h.uiCreated && h.shm->uiConnected && haveFrame)
		{
			uint32_t idx = 0;
			XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
			xrAcquireSwapchainImage(h.uiSwapchain, &ai, &idx);
			XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
			wi.timeout = XR_INFINITE_DURATION;
			xrWaitSwapchainImage(h.uiSwapchain, &wi);
			h.ctx->CopyResource(h.uiSwapTex[idx], h.uiTex);
			XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
			xrReleaseSwapchainImage(h.uiSwapchain, &ri);

			uint32_t pc = h.shm->panelCount;
			if (pc > VR_HOST_MAX_PANELS) pc = VR_HOST_MAX_PANELS;
			for (uint32_t i = 0; i < pc; ++i)
			{
				const VRHostSharedBlock::VRHostPanel& p = h.shm->panels[i];
				if (!p.visible || p.isGroupBar) continue;
				XrCompositionLayerQuad& q = quads[i];
				q = { XR_TYPE_COMPOSITION_LAYER_QUAD };
				q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
				q.space = h.space;
				q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
				q.subImage.swapchain = h.uiSwapchain;
				q.subImage.imageRect.offset.x = (int32_t)(p.u0 * h.uiW);
				q.subImage.imageRect.offset.y = (int32_t)(p.v0 * h.uiH);
				q.subImage.imageRect.extent.width = (int32_t)((p.u1 - p.u0) * h.uiW);
				q.subImage.imageRect.extent.height = (int32_t)((p.v1 - p.v0) * h.uiH);
				q.pose.orientation.x = p.pose.quatX; q.pose.orientation.y = p.pose.quatY;
				q.pose.orientation.z = p.pose.quatZ; q.pose.orientation.w = p.pose.quatW;
				q.pose.position.x = p.pose.posX; q.pose.position.y = p.pose.posY;
				q.pose.position.z = p.pose.posZ;
				q.size.width = p.widthMeters;
				q.size.height = p.heightMeters;
				layers[layerCount++] = (XrCompositionLayerBaseHeader*)&q;
			}
		}

		XrFrameEndInfo fei = { XR_TYPE_FRAME_END_INFO };
		fei.displayTime = fs.predictedDisplayTime;
		fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		fei.layerCount = fs.shouldRender ? layerCount : 0u;
		fei.layers = layers;
		xrEndFrame(h.session, &fei);
	}

	L("=== xrhost exiting (frames: %llu) ===", (unsigned long long)frame);
	if (h.sessionRunning) xrEndSession(h.session);
	xrDestroySession(h.session);
	xrDestroyInstance(h.instance);
	return 0;
}
