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

// FILE: VRSettingsMenu.cpp //////////////////////////////////////////////////////////////////////
// GeneralsVR @feature The in-headset settings menu. See VRSettingsMenu.h for the design.
///////////////////////////////////////////////////////////////////////////////////////////////////

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>

#include "VRDevice/VRSettingsMenu.h"
#include "VRDevice/VRControls.h"
#include "VRDevice/OpenXRManager.h"

#include "Common/Debug.h"
#include "Common/GameAudio.h"
#include "Common/AudioAffect.h"
#include "Common/GlobalData.h"
#include "GameClient/Display.h"
#include "GameClient/DisplayString.h"
#include "GameClient/DisplayStringManager.h"
#include "GameClient/GameFont.h"
#include "GameLogic/GameLogic.h"
#include "W3DDevice/GameClient/W3DDisplay.h"

VRSettingsMenu *TheVRSettingsMenu = nullptr;

namespace
{
	// ---- what the menu offers -------------------------------------------------------------------

	enum VRSettingId
	{
		VRSET_RECENTER = 0,
		VRSET_WORLD_SCALE,
		VRSET_SMOOTH_MOTION,
		VRSET_SHOW_FPS,
		VRSET_SHOW_CLOCKS,
		VRSET_INPUT_MODE,
		VRSET_TRIGGER_MASH,
		VRSET_STICK_SPEED,
		VRSET_HANDEDNESS,
		VRSET_FACE_BUTTONS,
		VRSET_SHADOWS,
		VRSET_SKY_COLOR,
		VRSET_ENHANCED_ASSETS,
		VRSET_MUSIC_VOL,
		VRSET_SOUND_VOL,
		VRSET_VOICE_VOL,
	};

	enum WidgetKind { W_BUTTON, W_TOGGLE, W_SLIDER, W_CHOICE };

	struct WidgetDef
	{
		Int kind;
		Int id;
		const wchar_t *label;
		Real minV, maxV;
		Bool logScale;     ///< slider maps t through exp(): right feel for the world scale
		const wchar_t *tip; ///< shown at the bottom of the menu while the row is hovered
	};

	// One table per tab. The menu IS these tables: add a line here and the row appears,
	// hit-testing and tooltip included. Values live behind getValue/setValue.
	const WidgetDef TAB_VR_W[] =
	{
		{ W_BUTTON, VRSET_RECENTER,      L"Recenter view",              0.0f, 0.0f,   FALSE,
			L"Your current position and facing become the new center" },
		{ W_SLIDER, VRSET_WORLD_SCALE,   L"World scale",                80.0f, 900.0f, TRUE,
			L"How big the battlefield feels. Lower is bigger and closer" },
		{ W_TOGGLE, VRSET_SMOOTH_MOTION, L"Smooth motion",              0.0f, 1.0f,   FALSE,
			L"Smooths unit movement between game ticks. Off can look steppy" },
		{ W_TOGGLE, VRSET_SHOW_FPS,      L"Show FPS",                   0.0f, 1.0f,   FALSE,
			L"Shows the frame rate on the panels" },
		{ W_TOGGLE, VRSET_SHOW_CLOCKS,   L"Show clocks",                0.0f, 1.0f,   FALSE,
			L"Shows the system time and the game timer on the panels" },
	};
	const WidgetDef TAB_CONTROLS_W[] =
	{
		{ W_CHOICE, VRSET_INPUT_MODE,    L"Input",                      0.0f, 1.0f,   FALSE,
			L"Both lets the mouse take over when it moves. Controllers only never switches" },
		{ W_TOGGLE, VRSET_TRIGGER_MASH,  L"Triple squeeze takes control", 0.0f, 1.0f, FALSE,
			L"Squeeze the trigger three times fast to take control back from the mouse" },
		{ W_SLIDER, VRSET_STICK_SPEED,   L"Stick speed",                0.4f, 2.0f,   FALSE,
			L"How fast the thumbsticks pan and turn" },
		{ W_CHOICE, VRSET_HANDEDNESS,    L"Handedness",                 0.0f, 1.0f,   FALSE,
			L"Left handed mirrors everything: beam on the left hand, panel on the right" },
	};
	const WidgetDef TAB_GRAPHICS_W[] =
	{
		{ W_TOGGLE, VRSET_SHADOWS,       L"Unit shadows",               0.0f, 1.0f,   FALSE,
			L"Unit and building shadows in the headset" },
		{ W_CHOICE, VRSET_SKY_COLOR,     L"Sky color",                  0.0f, 5.0f,   FALSE,
			L"The color of the void around the battlefield" },
	};
	const WidgetDef TAB_AUDIO_W[] =
	{
		{ W_SLIDER, VRSET_MUSIC_VOL,     L"Music",                      0.0f, 1.0f,   FALSE,
			L"Music volume" },
		{ W_SLIDER, VRSET_SOUND_VOL,     L"Sound effects",              0.0f, 1.0f,   FALSE,
			L"Battle and interface sound volume" },
		{ W_SLIDER, VRSET_VOICE_VOL,     L"Voices",                     0.0f, 1.0f,   FALSE,
			L"Unit voice volume" },
	};

	const WidgetDef TAB_UI_W[] =
	{
		{ W_CHOICE, VRSET_ENHANCED_ASSETS, L"Assets",                   0.0f, 1.0f,   FALSE,
			L"Enhanced loads files from Data\\Enhanced over the originals. Restart to apply" },
	};

	struct TabDef { const wchar_t *name; const WidgetDef *widgets; Int count; };
	const TabDef TABS[] =
	{
		{ L"VR",       TAB_VR_W,       sizeof(TAB_VR_W) / sizeof(WidgetDef) },
		{ L"Controls", TAB_CONTROLS_W, sizeof(TAB_CONTROLS_W) / sizeof(WidgetDef) },
		{ L"Graphics", TAB_GRAPHICS_W, sizeof(TAB_GRAPHICS_W) / sizeof(WidgetDef) },
		{ L"UI",       TAB_UI_W,       sizeof(TAB_UI_W) / sizeof(WidgetDef) },
		{ L"Audio",    TAB_AUDIO_W,    sizeof(TAB_AUDIO_W) / sizeof(WidgetDef) },
	};
	const Int TAB_COUNT = sizeof(TABS) / sizeof(TabDef);

	// No mouse-only pin: a player who pinned it in the headset lost the rays AND the only menu
	// that could unpin them (the flat game has no VR menu). Both = auto-switch as before.
	const wchar_t *INPUT_MODE_NAMES[] = { L"Both", L"Controllers only" };
	const wchar_t *HANDEDNESS_NAMES[] = { L"Right handed", L"Left handed" };
	const wchar_t *FACE_BUTTON_NAMES[] = { L"Standard", L"Swapped" };
	// Order matches the palette in W3DDisplay's eye pass - keep the two lists in step.
	const wchar_t *SKY_COLOR_NAMES[] =
	{ L"Black", L"Midnight blue", L"Dawn purple", L"Storm grey", L"Forest", L"Ember" };
	const wchar_t *ENHANCED_ASSET_NAMES[] = { L"Original", L"Enhanced" };

	const wchar_t *choiceName(Int id, Int idx)
	{
		if (idx < 0)
			return L"?";
		switch (id)
		{
			case VRSET_INPUT_MODE:   return (idx < 2) ? INPUT_MODE_NAMES[idx] : L"?";
			case VRSET_HANDEDNESS:   return (idx < 2) ? HANDEDNESS_NAMES[idx] : L"?";
			case VRSET_FACE_BUTTONS: return (idx < 2) ? FACE_BUTTON_NAMES[idx] : L"?";
			case VRSET_SKY_COLOR:
			{
				if (idx < 6)
					return SKY_COLOR_NAMES[idx];
				// Past the colours come the sky pictures shipped in Skies\, by file name.
				static wchar_t s_skyName[64];
				const char *n = W3DDisplay::getVRSkyboxName(idx - 6);
				mbstowcs(s_skyName, n, 63);
				s_skyName[63] = 0;
				return s_skyName;
			}
			case VRSET_ENHANCED_ASSETS: return (idx < 2) ? ENHANCED_ASSET_NAMES[idx] : L"?";
			default:                 return L"?";
		}
	}

	// ---- hit codes ------------------------------------------------------------------------------
	enum { HIT_NONE = -1, HIT_CLOSE = 100, HIT_TAB0 = 110 /* ..+TAB_COUNT */, HIT_TOGGLE_BTN = 120,
	       HIT_ROW0 = 0 /* ..+rowCount */ };

	// ---- colours --------------------------------------------------------------------------------
	// The pointer's own cyan, so the menu reads as part of the same instrument as the beam.
	inline UnsignedInt colPanel()     { return GameMakeColor(10, 16, 28, 240); }
	inline UnsignedInt colPanelEdge() { return GameMakeColor(89, 217, 255, 255); }
	inline UnsignedInt colRow()       { return GameMakeColor(20, 30, 48, 255); }
	inline UnsignedInt colRowHover()  { return GameMakeColor(34, 52, 80, 255); }
	inline UnsignedInt colAccent()    { return GameMakeColor(89, 217, 255, 255); }
	inline UnsignedInt colAccentDim() { return GameMakeColor(45, 95, 118, 255); }
	inline UnsignedInt colText()      { return GameMakeColor(235, 240, 245, 255); }
	inline UnsignedInt colTextDim()   { return GameMakeColor(150, 160, 175, 255); }
	inline UnsignedInt colDrop()      { return GameMakeColor(0, 0, 0, 200); }

	// ---- persistence ----------------------------------------------------------------------------
	void settingsPath(char *out, size_t cap)
	{
		const char *base = getenv("LOCALAPPDATA");
		if (base == nullptr) base = ".";
		_snprintf(out, cap, "%s\\GeneralsVR", base);
		CreateDirectoryA(out, nullptr);	// harmless if it already exists
		_snprintf(out, cap, "%s\\GeneralsVR\\vr-settings.ini", base);
	}

	Int strH(DisplayString *s)
	{
		Int w = 0, h = 0;
		if (s != nullptr)
			s->getSize(&w, &h);
		return h;
	}

	Real sliderToValue(const WidgetDef &w, Real t)
	{
		if (t < 0.0f) t = 0.0f;
		if (t > 1.0f) t = 1.0f;
		if (w.logScale)
			return (Real)exp(log(w.minV) + ((Real)log(w.maxV) - (Real)log(w.minV)) * t);
		return w.minV + (w.maxV - w.minV) * t;
	}

	Real valueToSlider(const WidgetDef &w, Real v)
	{
		if (v < w.minV) v = w.minV;
		if (v > w.maxV) v = w.maxV;
		if (w.logScale)
			return (Real)((log(v) - log(w.minV)) / (log(w.maxV) - log(w.minV)));
		return (v - w.minV) / (w.maxV - w.minV);
	}
}

//-------------------------------------------------------------------------------------------------
VRSettingsMenu::VRSettingsMenu()
	: m_open(FALSE)
	, m_tab(0)
	, m_hover(HIT_NONE)
	, m_dragRow(-1)
	, m_pointerConsumed(FALSE)
	, m_frameConsumed(FALSE)
	, m_pointerX(0)
	, m_pointerY(0)
{
	for (Int i = 0; i < MAX_STRINGS; ++i)
		m_strings[i] = nullptr;

	loadSettings();
}

//-------------------------------------------------------------------------------------------------
VRSettingsMenu::~VRSettingsMenu()
{
	for (Int i = 0; i < MAX_STRINGS; ++i)
	{
		if (m_strings[i] != nullptr && TheDisplayStringManager != nullptr)
			TheDisplayStringManager->freeDisplayString(m_strings[i]);
		m_strings[i] = nullptr;
	}
}

//-------------------------------------------------------------------------------------------------
/** Current live value of a setting. The menu never caches these: whatever changed them (the
	* sticks resizing the world, the game's own audio options) shows up here immediately. */
//-------------------------------------------------------------------------------------------------
Real VRSettingsMenu::getValue(Int id) const
{
	switch (id)
	{
		case VRSET_WORLD_SCALE:
			return (TheOpenXR != nullptr) ? TheOpenXR->getWorldUnitsPerMeter() : 500.0f;
		case VRSET_SMOOTH_MOTION:
			return (TheGlobalData != nullptr && TheGlobalData->m_smoothMotion) ? 1.0f : 0.0f;
		case VRSET_SHOW_FPS:
			return (TheGlobalData != nullptr && !TheGlobalData->m_vrHideFps) ? 1.0f : 0.0f;
		case VRSET_SHOW_CLOCKS:
			return (TheGlobalData != nullptr && !TheGlobalData->m_vrHideClocks) ? 1.0f : 0.0f;
		case VRSET_INPUT_MODE:
			return (TheGlobalData != nullptr) ? (Real)TheGlobalData->m_vrInputMode : 0.0f;
		case VRSET_TRIGGER_MASH:
			return (TheGlobalData != nullptr && TheGlobalData->m_vrTriggerMashEnabled) ? 1.0f : 0.0f;
		case VRSET_STICK_SPEED:
			return (TheGlobalData != nullptr) ? TheGlobalData->m_vrStickSpeed : 1.0f;
		case VRSET_HANDEDNESS:
			return (TheGlobalData != nullptr && TheGlobalData->m_vrLeftHanded) ? 1.0f : 0.0f;
		case VRSET_FACE_BUTTONS:
			return (TheGlobalData != nullptr && TheGlobalData->m_vrSwapFaceButtons) ? 1.0f : 0.0f;
		case VRSET_SHADOWS:
			return (TheGlobalData != nullptr && TheGlobalData->m_vrShadowsEnabled) ? 1.0f : 0.0f;
		case VRSET_SKY_COLOR:
			return (TheGlobalData != nullptr) ? (Real)TheGlobalData->m_vrSkyColorIndex : 0.0f;
		case VRSET_ENHANCED_ASSETS:
			return (TheGlobalData != nullptr && TheGlobalData->m_vrEnhancedAssets) ? 1.0f : 0.0f;
		case VRSET_MUSIC_VOL:
			return (TheAudio != nullptr) ? TheAudio->getVolume(AudioAffect_Music) : 0.5f;
		case VRSET_SOUND_VOL:
			return (TheAudio != nullptr) ? TheAudio->getVolume(AudioAffect_Sound) : 0.5f;
		case VRSET_VOICE_VOL:
			return (TheAudio != nullptr) ? TheAudio->getVolume(AudioAffect_Speech) : 0.5f;
		default:
			return 0.0f;
	}
}

//-------------------------------------------------------------------------------------------------
void VRSettingsMenu::setValue(Int id, Real v)
{
	switch (id)
	{
		case VRSET_WORLD_SCALE:
			if (TheOpenXR != nullptr)
				TheOpenXR->setWorldUnitsPerMeter(v);
			break;
		case VRSET_SMOOTH_MOTION:
			if (TheWritableGlobalData != nullptr)
				TheWritableGlobalData->m_smoothMotion = (v != 0.0f);
			break;
		case VRSET_SHOW_FPS:
			if (TheWritableGlobalData != nullptr)
				TheWritableGlobalData->m_vrHideFps = (v == 0.0f);
			break;
		case VRSET_SHOW_CLOCKS:
			if (TheWritableGlobalData != nullptr)
				TheWritableGlobalData->m_vrHideClocks = (v == 0.0f);
			break;
		case VRSET_INPUT_MODE:
			if (TheWritableGlobalData != nullptr)
			{
				Int mode = (Int)(v + 0.5f);
				if (mode < 0 || mode > 1)
					mode = 0;	// older saves knew a mouse-only pin; it maps back to auto
				TheWritableGlobalData->m_vrInputMode = mode;
			}
			break;
		case VRSET_TRIGGER_MASH:
			if (TheWritableGlobalData != nullptr)
				TheWritableGlobalData->m_vrTriggerMashEnabled = (v != 0.0f);
			break;
		case VRSET_STICK_SPEED:
			if (TheWritableGlobalData != nullptr)
				TheWritableGlobalData->m_vrStickSpeed = v;
			break;
		case VRSET_HANDEDNESS:
			if (TheWritableGlobalData != nullptr)
				TheWritableGlobalData->m_vrLeftHanded = (v != 0.0f);
			break;
		case VRSET_FACE_BUTTONS:
			if (TheWritableGlobalData != nullptr)
				TheWritableGlobalData->m_vrSwapFaceButtons = (v != 0.0f);
			break;
		case VRSET_SHADOWS:
			if (TheWritableGlobalData != nullptr)
				TheWritableGlobalData->m_vrShadowsEnabled = (v != 0.0f);
			break;
		case VRSET_SKY_COLOR:
			if (TheWritableGlobalData != nullptr)
			{
				Int idx = (Int)(v + 0.5f);
				const Int total = 6 + W3DDisplay::getVRSkyboxCount();
				if (idx < 0 || idx >= total)
					idx = 0;
				TheWritableGlobalData->m_vrSkyColorIndex = idx;
			}
			break;
		case VRSET_ENHANCED_ASSETS:
			if (TheWritableGlobalData != nullptr)
				TheWritableGlobalData->m_vrEnhancedAssets = (v != 0.0f);
			break;
		case VRSET_MUSIC_VOL:
			if (TheAudio != nullptr)
				TheAudio->setVolume(v, AudioAffect_Music);
			break;
		case VRSET_SOUND_VOL:
			if (TheAudio != nullptr)
				TheAudio->setVolume(v, (AudioAffect)(AudioAffect_Sound | AudioAffect_Sound3D));
			break;
		case VRSET_VOICE_VOL:
			if (TheAudio != nullptr)
				TheAudio->setVolume(v, AudioAffect_Speech);
			break;
		default:
			break;
	}

	// The VR-side settings persist; audio rides the game's own options persistence.
	if (id != VRSET_MUSIC_VOL && id != VRSET_SOUND_VOL && id != VRSET_VOICE_VOL)
		saveSettings();
}

//-------------------------------------------------------------------------------------------------
void VRSettingsMenu::act(Int id)
{
	if (id == VRSET_RECENTER && TheOpenXR != nullptr)
		TheOpenXR->recenter();
}

//-------------------------------------------------------------------------------------------------
/** vr-settings.ini: plain "key = value" lines. Written on every change, read once at startup.
	* Loaded AFTER the command line, so a saved world scale wins over the launcher's -vrscale
	* default (the flag stays the fallback for a fresh install). */
//-------------------------------------------------------------------------------------------------
void VRSettingsMenu::loadSettings()
{
	char path[MAX_PATH];
	settingsPath(path, sizeof(path));

	FILE *f = fopen(path, "r");
	if (f == nullptr)
		return;

	char key[64];
	float v = 0.0f;
	while (fscanf(f, " %63[^= \t] = %f ", key, &v) == 2)
	{
		if (strcmp(key, "worldScale") == 0)        setValue(VRSET_WORLD_SCALE, v);
		else if (strcmp(key, "smoothMotion") == 0) setValue(VRSET_SMOOTH_MOTION, v);
		else if (strcmp(key, "showFps") == 0)      setValue(VRSET_SHOW_FPS, v);
		else if (strcmp(key, "showClocks") == 0)   setValue(VRSET_SHOW_CLOCKS, v);
		else if (strcmp(key, "showReadouts") == 0)	// older saves had the pair as one switch
		{
			setValue(VRSET_SHOW_FPS, v);
			setValue(VRSET_SHOW_CLOCKS, v);
		}
		else if (strcmp(key, "inputMode") == 0)    setValue(VRSET_INPUT_MODE, v);
		else if (strcmp(key, "triggerMash") == 0)  setValue(VRSET_TRIGGER_MASH, v);
		else if (strcmp(key, "stickSpeed") == 0)   setValue(VRSET_STICK_SPEED, v);
		else if (strcmp(key, "leftHanded") == 0)   setValue(VRSET_HANDEDNESS, v);
		else if (strcmp(key, "shadows") == 0)      setValue(VRSET_SHADOWS, v);
		else if (strcmp(key, "skyColor") == 0)     setValue(VRSET_SKY_COLOR, v);
		else if (strcmp(key, "enhancedAssets") == 0) setValue(VRSET_ENHANCED_ASSETS, v);
	}
	fclose(f);
	DEBUG_LOG(("VRSettings: loaded %s", path));
}

//-------------------------------------------------------------------------------------------------
void VRSettingsMenu::saveSettings() const
{
	char path[MAX_PATH];
	settingsPath(path, sizeof(path));

	FILE *f = fopen(path, "w");
	if (f == nullptr)
		return;
	fprintf(f, "worldScale = %.1f\n",  getValue(VRSET_WORLD_SCALE));
	fprintf(f, "smoothMotion = %.0f\n", getValue(VRSET_SMOOTH_MOTION));
	fprintf(f, "showFps = %.0f\n",     getValue(VRSET_SHOW_FPS));
	fprintf(f, "showClocks = %.0f\n",  getValue(VRSET_SHOW_CLOCKS));
	fprintf(f, "inputMode = %.0f\n",   getValue(VRSET_INPUT_MODE));
	fprintf(f, "triggerMash = %.0f\n", getValue(VRSET_TRIGGER_MASH));
	fprintf(f, "stickSpeed = %.2f\n",  getValue(VRSET_STICK_SPEED));
	fprintf(f, "leftHanded = %.0f\n",  getValue(VRSET_HANDEDNESS));
	fprintf(f, "shadows = %.0f\n",     getValue(VRSET_SHADOWS));
	fprintf(f, "skyColor = %.0f\n",    getValue(VRSET_SKY_COLOR));
	fprintf(f, "enhancedAssets = %.0f\n", getValue(VRSET_ENHANCED_ASSETS));
	fclose(f);
}

//-------------------------------------------------------------------------------------------------
/** The one layout, shared by hit-testing and drawing, so what you see is what the ray touches. */
//-------------------------------------------------------------------------------------------------
void VRSettingsMenu::buildLayout(Layout &out) const
{
	const Int sw = (TheDisplay != nullptr) ? (Int)TheDisplay->getWidth() : 800;
	const Int sh = (TheDisplay != nullptr) ? (Int)TheDisplay->getHeight() : 600;

	// The whole in-game menu frame: edge to edge, down to the control-bar line. The panel is
	// the screen while it is open, not a floating box in front of one.
	out.box.x = (Int)(sw * 0.005f);
	out.box.y = (Int)(sh * 0.005f);
	out.box.w = (Int)(sw * 0.99f);
	out.box.h = (Int)(sh * 0.645f);

	const Int pad = out.box.w / 40;
	const Int titleH = (Int)(sh * 0.05f);
	const Int tabH = (Int)(sh * 0.05f);

	out.close.w = titleH;
	out.close.h = titleH;
	out.close.x = out.box.x + out.box.w - titleH - pad / 2;
	out.close.y = out.box.y + pad / 2;

	const Int tabW = (out.box.w - pad * 2) / TAB_COUNT;
	for (Int t = 0; t < TAB_COUNT; ++t)
	{
		out.tabs[t].x = out.box.x + pad + t * tabW;
		out.tabs[t].y = out.box.y + titleH + pad / 2;
		out.tabs[t].w = tabW - 4;
		out.tabs[t].h = tabH;
	}

	const Int rowTop = out.tabs[0].y + tabH + pad / 2;
	const Int rowH = (Int)(sh * 0.065f);
	out.rowCount = TABS[m_tab].count;
	if (out.rowCount > MAX_ROWS)
		out.rowCount = MAX_ROWS;

	for (Int r = 0; r < out.rowCount; ++r)
	{
		Rect &row = out.rows[r];
		row.x = out.box.x + pad;
		row.y = rowTop + r * (rowH + pad / 2);
		row.w = out.box.w - pad * 2;
		row.h = rowH;

		// Slider track: the right half of the row, vertically centred.
		Rect &track = out.tracks[r];
		track.w = row.w * 42 / 100;
		track.h = row.h / 3;
		track.x = row.x + row.w - track.w - pad;
		track.y = row.y + (row.h - track.h) / 2;
	}

	// The [ VR ] badge rides the top-left corner of whatever in-game menu window is open
	// (the Escape menu, the generals experience screen, all of them).
	out.toggleButtonValid = FALSE;
	Int mx = 0, my = 0, mw = 0, mh = 0;
	if (TheVRControls != nullptr && TheVRControls->getMenuWindowRect(mx, my, mw, mh))
	{
		out.toggleButton.w = (Int)(sw * 0.055f);
		out.toggleButton.h = (Int)(sh * 0.038f);
		out.toggleButton.x = mx + 6;
		out.toggleButton.y = my + 6;
		out.toggleButtonValid = TRUE;
	}
}

//-------------------------------------------------------------------------------------------------
/** The ray's panel pixel, before the game sees it. All the interaction lives here: hover, the
	* toggle badge, tabs, toggles, choice cycling, slider drags. Uses the trigger edges straight
	* from OpenXR - by the time the game's synthetic mouse would have seen them, we have already
	* decided they belong to this layer. */
//-------------------------------------------------------------------------------------------------
Bool VRSettingsMenu::handlePointer(Int px, Int py)
{
	m_hover = HIT_NONE;
	m_pointerConsumed = FALSE;

	if (TheOpenXR == nullptr)
		return FALSE;
	const Bool inGame = (TheGameLogic != nullptr && TheGameLogic->isInGame());
	if (!inGame)
	{
		m_open = FALSE;	// the battle ended under the menu
		return FALSE;
	}

	const VRControllerState &right = TheOpenXR->getController(VR_HAND_RIGHT);
	const VRControllerState &left = TheOpenXR->getController(VR_HAND_LEFT);

	Layout lay;
	buildLayout(lay);

	if (!m_open)
	{
		// Closed: the only thing this layer owns is its [ VR ] badge on the in-game menu.
		if (!lay.toggleButtonValid || !lay.toggleButton.contains(px, py))
			return FALSE;

		m_hover = HIT_TOGGLE_BTN;
		if (right.triggerPressed)
		{
			m_open = TRUE;
			m_dragRow = -1;
			DEBUG_LOG(("VRSettings: opened"));
		}
		m_pointerConsumed = TRUE;
		m_pointerX = px; m_pointerY = py;
		return TRUE;
	}

	// Open: the whole panel is ours - nothing reaches the game's UI, which is the point.
	m_pointerConsumed = TRUE;
	m_pointerX = px; m_pointerY = py;

	// The left trigger backs out, same as it cancels a pending power.
	if (left.triggerPressed)
	{
		m_open = FALSE;
		m_dragRow = -1;
		return TRUE;
	}

	// A slider drag in progress follows the pointer while the trigger is held, even off the track.
	if (m_dragRow >= 0)
	{
		if (!right.trigger)
		{
			m_dragRow = -1;
		}
		else if (m_dragRow < lay.rowCount)
		{
			const WidgetDef &w = TABS[m_tab].widgets[m_dragRow];
			const Rect &track = lay.tracks[m_dragRow];
			const Real t = (track.w > 0) ? (Real)(px - track.x) / (Real)track.w : 0.0f;
			setValue(w.id, sliderToValue(w, t));
			m_hover = m_dragRow;
			return TRUE;
		}
	}

	if (lay.close.contains(px, py))
	{
		m_hover = HIT_CLOSE;
		if (right.triggerPressed)
		{
			m_open = FALSE;
			m_dragRow = -1;
		}
		return TRUE;
	}

	for (Int t = 0; t < TAB_COUNT; ++t)
	{
		if (lay.tabs[t].contains(px, py))
		{
			m_hover = HIT_TAB0 + t;
			if (right.triggerPressed && m_tab != t)
			{
				m_tab = t;
				m_dragRow = -1;
			}
			return TRUE;
		}
	}

	for (Int r = 0; r < lay.rowCount; ++r)
	{
		if (!lay.rows[r].contains(px, py))
			continue;

		m_hover = r;
		const WidgetDef &w = TABS[m_tab].widgets[r];

		if (right.triggerPressed)
		{
			switch (w.kind)
			{
				case W_BUTTON:
					act(w.id);
					break;
				case W_TOGGLE:
					setValue(w.id, (getValue(w.id) != 0.0f) ? 0.0f : 1.0f);
					break;
				case W_CHOICE:
				{
					// The sky selector grows past its table entry: the colours, then every
					// sky picture found in Skies\.
					Int total = (Int)(w.maxV + 0.5f) + 1;
					if (w.id == VRSET_SKY_COLOR)
						total = 6 + W3DDisplay::getVRSkyboxCount();
					Int next = (Int)(getValue(w.id) + 0.5f) + 1;
					if (next >= total)
						next = (Int)(w.minV + 0.5f);
					setValue(w.id, (Real)next);
					break;
				}
				case W_SLIDER:
				{
					m_dragRow = r;
					const Rect &track = lay.tracks[r];
					const Real t = (track.w > 0) ? (Real)(px - track.x) / (Real)track.w : 0.0f;
					setValue(w.id, sliderToValue(w, t));
					break;
				}
				default:
					break;
			}
		}
		return TRUE;
	}

	return TRUE;	// inside nothing in particular, but the panel is open: still ours
}

//-------------------------------------------------------------------------------------------------
DisplayString *VRSettingsMenu::text(Int slot, const wchar_t *fmt, ...)
{
	if (slot < 0 || slot >= MAX_STRINGS || TheDisplayStringManager == nullptr)
		return nullptr;

	if (m_strings[slot] == nullptr)
	{
		m_strings[slot] = TheDisplayStringManager->newDisplayString();
		if (m_strings[slot] == nullptr)
			return nullptr;
		// Big. The panel hangs at arm's length in a headset; desk-sized type was unreadable.
		GameFont *font = (TheFontLibrary != nullptr)
			? TheFontLibrary->getFont("Arial", 36, TRUE) : nullptr;
		if (font != nullptr)
			m_strings[slot]->setFont(font);
	}

	wchar_t buffer[256];
	va_list args;
	va_start(args, fmt);
	_vsnwprintf(buffer, 255, fmt, args);
	buffer[255] = 0;
	va_end(args);

	UnicodeString u;
	u.set(buffer);
	m_strings[slot]->setText(u);
	return m_strings[slot];
}

//-------------------------------------------------------------------------------------------------
/** The one smaller string: the tooltip/hint line at the bottom of the menu. Its own slot with
	* its own font, because a pooled slot keeps the font it was born with. */
//-------------------------------------------------------------------------------------------------
DisplayString *VRSettingsMenu::smallText(const wchar_t *fmt, ...)
{
	const Int slot = MAX_STRINGS - 1;
	if (TheDisplayStringManager == nullptr)
		return nullptr;

	if (m_strings[slot] == nullptr)
	{
		m_strings[slot] = TheDisplayStringManager->newDisplayString();
		if (m_strings[slot] == nullptr)
			return nullptr;
		GameFont *font = (TheFontLibrary != nullptr)
			? TheFontLibrary->getFont("Arial", 20, FALSE) : nullptr;
		if (font != nullptr)
			m_strings[slot]->setFont(font);
	}

	wchar_t buffer[256];
	va_list args;
	va_start(args, fmt);
	_vsnwprintf(buffer, 255, fmt, args);
	buffer[255] = 0;
	va_end(args);

	UnicodeString u;
	u.set(buffer);
	m_strings[slot]->setText(u);
	return m_strings[slot];
}

//-------------------------------------------------------------------------------------------------
/** Paint the menu (or its badge) into the VR interface capture, over the game's own UI. */
//-------------------------------------------------------------------------------------------------
void VRSettingsMenu::drawIntoCapture()
{
	// Latch this frame's pointer claim (set by handlePointer before the capture) and spend it,
	// so a frame whose input path never ran cannot inherit last frame's claim.
	m_frameConsumed = m_pointerConsumed;
	m_pointerConsumed = FALSE;

	if (TheDisplay == nullptr)
	{
		m_frameConsumed = FALSE;
		return;
	}

	const Bool inGame = (TheGameLogic != nullptr && TheGameLogic->isInGame());
	const Bool mouseMode = (TheVRControls != nullptr && TheVRControls->isMouseKbMode());

	// The menu is a ray instrument: the battle ending or the mouse taking over puts it away.
	if (!inGame || mouseMode)
	{
		m_open = FALSE;
		m_frameConsumed = FALSE;
		return;
	}

	Layout lay;
	buildLayout(lay);

	Int slot = 0;	// pooled-string cursor; stable order per frame keeps churn to zero

	if (!m_open)
	{
		// Closed: just the [ VR ] badge on the in-game menu's corner.
		if (lay.toggleButtonValid
			&& TheVRControls != nullptr && TheVRControls->isMenuOpen())
		{
			const Rect &b = lay.toggleButton;
			const Bool hot = (m_hover == HIT_TOGGLE_BTN);
			TheDisplay->drawFillRect(b.x, b.y, b.w, b.h, hot ? colRowHover() : colRow());
			// A thin fluorescent orange edge, always on: the badge got lost against the menu art.
			TheDisplay->drawOpenRect(b.x - 1, b.y - 1, b.w + 2, b.h + 2, 2.0f,
				GameMakeColor(255, 120, 10, 255));
			DisplayString *s = text(slot++, L"VR");
			if (s != nullptr)
				s->draw(b.x + (b.w - s->getWidth()) / 2, b.y + (b.h - strH(s)) / 2,
					hot ? colAccent() : colText(), colDrop());
		}
		finishFrame();
		return;
	}

	// ---- the open panel -------------------------------------------------------------------------
	const Rect &box = lay.box;
	TheDisplay->drawFillRect(box.x, box.y, box.w, box.h, colPanel());
	TheDisplay->drawOpenRect(box.x, box.y, box.w, box.h, 2.0f, colPanelEdge());

	DisplayString *title = text(slot++, L"VR SETTINGS");
	if (title != nullptr)
		title->draw(box.x + box.w / 40, box.y + box.w / 60, colAccent(), colDrop());

	// [X]
	{
		const Bool hot = (m_hover == HIT_CLOSE);
		const Rect &c = lay.close;
		TheDisplay->drawFillRect(c.x, c.y, c.w, c.h, hot ? colRowHover() : colRow());
		TheDisplay->drawOpenRect(c.x, c.y, c.w, c.h, 1.0f, hot ? colAccent() : colAccentDim());
		DisplayString *s = text(slot++, L"X");
		if (s != nullptr)
			s->draw(c.x + (c.w - s->getWidth()) / 2, c.y + (c.h - strH(s)) / 2,
				hot ? colAccent() : colText(), colDrop());
	}

	// Tabs.
	for (Int t = 0; t < TAB_COUNT; ++t)
	{
		const Rect &tr = lay.tabs[t];
		const Bool active = (t == m_tab);
		const Bool hot = (m_hover == HIT_TAB0 + t);
		TheDisplay->drawFillRect(tr.x, tr.y, tr.w, tr.h,
			active ? colRowHover() : (hot ? colRow() : colPanel()));
		if (active)
			TheDisplay->drawFillRect(tr.x, tr.y + tr.h - 3, tr.w, 3, colAccent());
		DisplayString *s = text(slot++, L"%ls", TABS[t].name);
		if (s != nullptr)
			s->draw(tr.x + (tr.w - s->getWidth()) / 2, tr.y + (tr.h - strH(s)) / 2,
				active ? colAccent() : (hot ? colText() : colTextDim()), colDrop());
	}

	// Rows.
	for (Int r = 0; r < lay.rowCount; ++r)
	{
		const WidgetDef &w = TABS[m_tab].widgets[r];
		const Rect &row = lay.rows[r];
		const Bool hot = (m_hover == r);

		TheDisplay->drawFillRect(row.x, row.y, row.w, row.h, hot ? colRowHover() : colRow());

		DisplayString *label = text(slot++, L"%ls", w.label);
		if (label != nullptr)
			label->draw(row.x + row.h / 3, row.y + (row.h - strH(label)) / 2,
				colText(), colDrop());

		switch (w.kind)
		{
			case W_BUTTON:
			{
				DisplayString *s = text(slot++, L"[ press ]");
				if (s != nullptr)
					s->draw(row.x + row.w - s->getWidth() - row.h / 3,
						row.y + (row.h - strH(s)) / 2,
						hot ? colAccent() : colTextDim(), colDrop());
				break;
			}
			case W_TOGGLE:
			{
				const Bool on = (getValue(w.id) != 0.0f);
				const Int boxSz = row.h / 2;
				const Int bx = row.x + row.w - boxSz - row.h / 3;
				const Int by = row.y + (row.h - boxSz) / 2;
				TheDisplay->drawOpenRect(bx, by, boxSz, boxSz, 1.0f,
					on ? colAccent() : colTextDim());
				if (on)
					TheDisplay->drawFillRect(bx + 3, by + 3, boxSz - 6, boxSz - 6, colAccent());
				DisplayString *s = text(slot++, on ? L"On" : L"Off");
				if (s != nullptr)
					s->draw(bx - s->getWidth() - 8, row.y + (row.h - strH(s)) / 2,
						on ? colAccent() : colTextDim(), colDrop());
				break;
			}
			case W_CHOICE:
			{
				const Int idx = (Int)(getValue(w.id) + 0.5f);
				const wchar_t *name = choiceName(w.id, idx);
				DisplayString *s = text(slot++, L"%ls  (click to change)", name);
				if (s != nullptr)
					s->draw(row.x + row.w - s->getWidth() - row.h / 3,
						row.y + (row.h - strH(s)) / 2,
						hot ? colAccent() : colText(), colDrop());
				break;
			}
			case W_SLIDER:
			{
				const Rect &track = lay.tracks[r];
				const Real t = valueToSlider(w, getValue(w.id));
				const Int fillW = (Int)(track.w * t);

				TheDisplay->drawFillRect(track.x, track.y, track.w, track.h, colPanel());
				TheDisplay->drawOpenRect(track.x, track.y, track.w, track.h, 1.0f, colAccentDim());
				if (fillW > 0)
					TheDisplay->drawFillRect(track.x, track.y, fillW, track.h, colAccentDim());
				// the knob
				const Int kx = track.x + fillW - 3;
				TheDisplay->drawFillRect(kx, track.y - 3, 6, track.h + 6,
					hot ? colAccent() : colText());

				// value text left of the track
				DisplayString *s = nullptr;
				if (w.id == VRSET_WORLD_SCALE)
					s = text(slot++, L"%d", (Int)(getValue(w.id) + 0.5f));
				else if (w.id == VRSET_STICK_SPEED)
					s = text(slot++, L"x%.1f", getValue(w.id));
				else
					s = text(slot++, L"%d%%", (Int)(getValue(w.id) * 100.0f + 0.5f));
				if (s != nullptr)
					s->draw(track.x - s->getWidth() - 10,
						row.y + (row.h - strH(s)) / 2, colText(), colDrop());
				break;
			}
			default:
				break;
		}
	}

	// The bottom line: the hovered option's tooltip, or the plain how-to when nothing is.
	{
		DisplayString *s = nullptr;
		if (m_hover >= 0 && m_hover < lay.rowCount)
			s = smallText(L"%ls", TABS[m_tab].widgets[m_hover].tip);
		else
			s = smallText(L"Right trigger selects    Left trigger closes");
		if (s != nullptr)
			s->draw(box.x + box.w / 40, box.y + box.h - strH(s) - box.w / 80,
				(m_hover >= 0 && m_hover < lay.rowCount) ? colText() : colTextDim(), colDrop());
	}

	finishFrame();
}

//-------------------------------------------------------------------------------------------------
/** The layer's own pointer, where the beam meets the panel, on the frames it owned the pixel. */
//-------------------------------------------------------------------------------------------------
void VRSettingsMenu::finishFrame()
{
	if (m_frameConsumed)
	{
		TheDisplay->drawFillRect(m_pointerX - 5, m_pointerY - 5, 10, 10, colDrop());
		TheDisplay->drawFillRect(m_pointerX - 3, m_pointerY - 3, 6, 6, colAccent());
	}
}
