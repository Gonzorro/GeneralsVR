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

// FILE: VRSettingsMenu.h ////////////////////////////////////////////////////////////////////////
// GeneralsVR @feature The in-headset settings menu.
//
// How it works, in one paragraph. This is NOT a game window: it is painted straight into the VR
// interface capture (the texture the panels show), after the game's own UI, and it does its own
// hit-testing on the ray's panel pixel BEFORE the game sees it. That buys three things at once:
// it needs no .wnd asset (the mod ships only an exe - the game's data files are untouchable);
// it renders on every panel automatically (wrist panel, mouse-mode HUD); and while it is open
// the game's UI underneath is unreachable BY CONSTRUCTION, because the pixel and the clicks are
// simply never forwarded. A small [ VR ] button is painted onto the bottom-right corner of the
// in-game menu; clicking it opens the settings over that menu, clicking [X] (or the left
// trigger) closes them again.
//
// Settings apply LIVE through the engine's own globals, and the VR-specific ones persist to
// %LOCALAPPDATA%\GeneralsVR\vr-settings.ini (audio rides the game's own options persistence).
///////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Lib/BaseType.h"

class DisplayString;

class VRSettingsMenu
{
public:
	VRSettingsMenu();
	~VRSettingsMenu();

	Bool isOpen() const { return m_open; }

	/// The ray's panel pixel, offered to the settings layer BEFORE the game sees it. TRUE means
	/// consumed: the pixel and any trigger activity belong to this layer for the frame - the
	/// [ VR ] toggle button when the menu is closed, the whole panel while it is open.
	Bool handlePointer(Int px, Int py);

	/// Paint into the VR interface capture, over the game's own UI. Called by the display from
	/// inside the capture pass. Also self-closes when the battle ends or the mouse takes over.
	void drawIntoCapture();

	/// TRUE while the settings layer owned the pointer this frame. The shared 2D crosshair sits
	/// the frame out (this layer draws its own pointer). Latched at draw time, so a frame whose
	/// input path never ran cannot inherit last frame's claim.
	Bool pointerConsumed() const { return m_frameConsumed; }

private:
	// Everything the layout produces for one frame: the boxes hit-testing and drawing share.
	// Built identically in both, so what you see is exactly what the ray touches.
	struct Rect
	{
		Int x, y, w, h;
		Bool contains(Int px, Int py) const
		{ return px >= x && px < x + w && py >= y && py < y + h; }
	};

	enum { MAX_ROWS = 6, MAX_STRINGS = 40 };

	struct Layout
	{
		Rect box;                 ///< the whole menu panel
		Rect close;               ///< [X]
		Rect tabs[5];
		Rect rows[MAX_ROWS];      ///< one per widget of the current tab
		Rect tracks[MAX_ROWS];    ///< slider track inside its row (unused for other kinds)
		Int rowCount;
		Rect toggleButton;        ///< the [ VR ] badge on the in-game menu (menu-closed state)
		Bool toggleButtonValid;
	};
	void buildLayout(Layout &out) const;

	Real getValue(Int id) const;         ///< current live value of a setting
	void setValue(Int id, Real v);       ///< apply live + persist
	void act(Int id);                    ///< push-button actions (recenter)
	void loadSettings();                 ///< read vr-settings.ini and apply
	void saveSettings() const;           ///< write vr-settings.ini

	/// Pooled DisplayStrings so the text objects live across frames instead of churning.
	DisplayString *text(Int slot, const wchar_t *fmt, ...);
	/// The one smaller string: the tooltip/hint line (its own slot, its own font).
	DisplayString *smallText(const wchar_t *fmt, ...);
	/// Draw the layer's own pointer dot and retire this frame's pointer claim.
	void finishFrame();

	Bool m_open;
	Int m_tab;
	Int m_hover;              ///< hit code under the pointer this frame (HIT_* in the .cpp)
	Int m_dragRow;            ///< row index of the slider being dragged, -1 when none
	Bool m_pointerConsumed;   ///< set by handlePointer, spent by the next drawIntoCapture
	Bool m_frameConsumed;     ///< the latched claim the rest of this frame reads
	Int m_pointerX, m_pointerY;
	DisplayString *m_strings[MAX_STRINGS];
};

extern VRSettingsMenu *TheVRSettingsMenu;  ///< nullptr unless the game runs with -vr
