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

// FILE: VRControls.h ////////////////////////////////////////////////////////////////////////////
// GeneralsVR @feature Motion controllers: locomotion over the battlefield, and pointing at it.
//
// Design, in one paragraph. The battlefield is a table you are standing over. To move, you
// GRAB it and drag it - the world follows your hand, which is far more comfortable than
// flying a camera. Grab with BOTH hands and pull apart to make the map bigger (dive in), push
// together to shrink it (rise above the whole battle) - that is the "get further away" verb,
// and twisting both hands rotates the table. Thumbsticks do the same jobs for when you would
// rather not wave your arms. To command, you POINT: the controller casts a ray at the terrain,
// and the hit point is fed to the game as an ordinary mouse cursor, with the trigger as the
// left button and the other hand's trigger as the right button. That last part is the whole
// trick - selection, band-boxing, move and attack orders all run through the engine's real
// input pipeline, so every existing behaviour comes along for free.
//
// All of this drives the RTS tactical camera rather than a private VR camera, which keeps
// culling, fog of war, picking and the monitor mirror in agreement with what you see.
///////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Lib/BaseType.h"
#include "Common/GameType.h"
#include "WWMath/matrix3d.h"
#include "WWMath/vector3.h"

class W3DView;
class Line3DClass;
class SimpleSceneClass;
class Win32Mouse;

class VRControls
{
public:
	VRControls();
	~VRControls();

	/// Once per frame, before the game client consumes input.
	void update();

	/// The laser pointers, as a scene the stereo renderer draws with the world (and in the
	/// menus, on its own). Without a visible beam the player is pointing blind.
	SimpleSceneClass *getRayScene() const { return m_rayScene; }

	/// Draw the ten control-group slots into the render target OpenXRManager hands us. Called
	/// by the display, because only it may touch render targets.
	void drawGroupBar();

	/// The world-space transform the headset floats at: the tactical camera's position and
	/// heading, held upright (its downward pitch must not tilt the player's horizon). Shared
	/// with the stereo renderer so the eyes and the hands live in exactly the same space.
	static Bool getAnchor(W3DView *view, Matrix3D &outAnchor);

	/// Is a controller ray currently pointing at the ground? (Drives the in-world cursor.)
	Bool hasAimPoint() const { return m_hasAimPoint; }
	const Coord3D &getAimPoint() const { return m_aimPoint; }

	/// The right ray is resting on a UI panel THIS frame, so the cursor pixel it drives is real.
	/// The panel cursor is only drawn while this (or the physical mouse) is live: at any other
	/// time the cursor pixel is parked or stale, and drawing it paints a phantom X on the HUD
	/// panel for a beam that is pointing somewhere else entirely.
	Bool isRayOnScreenPanel() const { return m_rayOnScreenPanel; }

	/// An in-game menu or dialog is up (measured from the window system each frame).
	Bool isMenuOpen() const { return m_menuOpen; }
	/// Specifically a MODAL menu (the Escape menu and what it opens) - the promotion screen is
	/// not modal, and only modal menus get the solid black backing on the panel.
	Bool isModalMenuOpen() const { return m_modalMenuOpen; }
	/// The screen rect of the window that menu detection found (the biggest one), while a menu
	/// is up. The VR settings badge rides its bottom-right corner.
	Bool getMenuWindowRect(Int &x, Int &y, Int &w, Int &h) const;

	/// Paint the open radial menu (if any) into the VR interface capture. Called by the display
	/// from inside the capture pass, after the game's own UI.
	void drawRadialsIntoCapture();

	/// True while the on-screen cursor should be shown on the VR menu panel: the cursor has moved
	/// recently, whether by the physical mouse or a controller ray. Fades after a few idle seconds.
	Bool isCursorActive() const { return m_cursorActive; }
	/// True while the PHYSICAL mouse specifically has moved recently. Drives the monitor-view frame
	/// (a mouse+keyboard aid), so waving a controller does not raise it.
	Bool isMouseActive() const { return m_mouseActive; }

	/// Input mode. In mouse+keyboard mode the motion controllers are put away entirely (no rays, no
	/// grab-locomotion) and the mouse + keyboard drive the game as on the monitor. A future VR
	/// settings menu flips this; for now it is forced on.
	void setMouseKbMode(Bool on) { m_mouseKbMode = on; }
	Bool isMouseKbMode() const { return m_mouseKbMode; }

private:
	Bool computeHandRay(Int hand, Vector3 &outOrigin, Vector3 &outDir) const;
	Bool traceTerrain(const Vector3 &origin, const Vector3 &dir, Coord3D &outHit) const;
	/// Cast the ray at the actual scene - units, buildings, everything the player can click.
	/// Terrain-only tracing is not enough: a ray aimed at a tank passes straight through it and
	/// lands on the dirt behind, and the game would then pick the dirt.
	Bool traceScene(const Vector3 &origin, const Vector3 &dir, Coord3D &outHit) const;
	/// The drawable the ray strikes, if any.
	class Drawable *pickDrawable(const Vector3 &origin, const Vector3 &dir) const;
	/// Select / command in WORLD space, with no screen pixel involved. The cursor we drive lives
	/// in the flat camera's view, which is far narrower than the headset's: most of what the
	/// player can see simply does not project onto it, so a click routed through a pixel is
	/// swallowed. These go straight into the message stream instead - the same messages the
	/// mouse translators would have produced.
	void selectUnderRay(const Vector3 &origin, const Vector3 &dir);
	void commandUnderRay(const Vector3 &origin, const Vector3 &dir);

	/// Move the injected cursor to the top-left corner, where no window lives. The cursor is a
	/// fiction while the beam is out on the battlefield, but the game still reads its pixel for
	/// hover - left over a menu or the control bar, that UI lights up for a ray pointing at the
	/// dirt. Releases any held synthetic buttons first (never park mid-"drag").
	void parkCursor(Win32Mouse *mouse);
	/// Let go of any mouse buttons we injected, at the pixel they were last driven to.
	void releasePointerButtons(Win32Mouse *mouse);

	/// Sweep the laser across the ground with the trigger held to take everything inside the
	/// box, the way a mouse drag does. A rectangle is drawn on the ground while you sweep, so
	/// the gesture reads the same as the one it replaces.
	void updateBoxSelect(const Vector3 &origin, const Vector3 &dir);
	void selectInBox(const Coord3D &corner0, const Coord3D &corner1);
	void updateBoxVisual(Bool visible);
	/// A marker floating over everything currently selected - without one, a selection made from
	/// across the map is invisible.
	void updateSelectionMarkers();
	void updatePlacement(const Vector3 &origin, const Vector3 &dir);
	void jumpToSelection();
	void updateUiCrop();
	Bool m_placing;
	Bool m_placeArmed;             ///< the trigger has been LET GO of since placement began
	UnsignedInt m_placePressTime;  ///< when the trigger went down; 0 when not placing
	Bool m_placeTurning;           ///< held long enough that the building now turns with the beam
	Coord3D m_placeAnchor;         ///< where it pinned itself when the trigger went down
	Real m_placeAngle;
	/// What the game would do if the trigger went now - the beam wears this as its colour, since
	/// a mouse cursor cannot follow a laser out into the world.
	void getReticleColor(const Vector3 &origin, const Vector3 &dir,
		Real &outR, Real &outG, Real &outB) const;

	enum { MAX_SELECTION_MARKERS = 40 };
	class Line3DClass *m_boxLines[4];
	/// A bead over each selected unit, and its health beneath: red for what it has lost, green
	/// for what it has left. The flat game paints these in screen space, which does not exist
	/// out here, so they are built from world geometry instead.
	class SphereRenderObjClass *m_selectionBeads[MAX_SELECTION_MARKERS];
	class Line3DClass *m_healthBack[MAX_SELECTION_MARKERS];
	class Line3DClass *m_healthFill[MAX_SELECTION_MARKERS];
	Bool m_boxing;                ///< the trigger is down and the sweep has grown past a nudge
	Bool m_boxArmed;              ///< the trigger is down; we are watching to see if it becomes a sweep
	Coord3D m_boxStart;
	Coord3D m_boxEnd;
	/// The box is drawn in the PLAYER's frame, not the world's: one pair of edges runs left-right
	/// across your view. A box locked to the world's axes sits at a crooked angle to you the
	/// moment you turn, and you find yourself dragging a diamond.
	Vector3 m_boxRight;
	Vector3 m_boxForward;
	UnsignedInt m_boxPressTime;   ///< when the trigger went down; a box must be HELD, not just swept
	/// Where the ray lands: an object if it hits one, otherwise the ground.
	Bool traceAim(const Vector3 &origin, const Vector3 &dir, Coord3D &outHit) const;
	void updateLocomotion(W3DView *view);
	void updatePointer(W3DView *view);
	void updateRays(W3DView *view);
	/// Track physical-mouse movement so the mouse+keyboard aids appear on use and fade when idle.
	/// Cursor motion we injected from the controller ray is discounted for the physical signal.
	void updateMouseActivity();
	/// Soft auto-switch of the input mode: rays are primary; moving the physical mouse takes over at
	/// once; a few idle seconds with the controllers in use hands it back to the rays.
	void updateInputMode();
	/// The frame on the ground marking what the flat monitor is rendering (the tactical camera's
	/// footprint), so a mouse+keyboard player can see where edge-scrolling the mouse will pan.
	void updateMonitorFrame(Bool visible);
	/// A crosshair on the ground where the mouse points, so a mouse+keyboard player can see their
	/// cursor out on the battlefield (there is no flat screen in the headset to carry it).
	void updateMouseMarker(Bool visible);
	/// If the mouse pixel falls on an active HUD/menu panel, return the world point ON that panel so
	/// the one crosshair can slide onto it (instead of dropping to the terrain behind). False = the
	/// mouse is over the open battlefield.
	Bool mousePanelWorldPos(const Matrix3D &anchor, Int mx, Int my, Vector3 &out) const;
	/// Edge-scroll the tactical camera when the mouse is shoved to the screen edge, as on the monitor.
	void updateMouseScroll(W3DView *view);
	/// Draw the mouse's drag-select band box on the ground (the flat game draws it in screen space,
	/// which does not exist in the headset).
	void updateMouseBoxSelect(W3DView *view);
	/// Measure where the in-game HUD panel should sit (matched to the monitor frame) and how opaque
	/// it should be (faded unless the mouse is over it), and hand it to OpenXR.
	void updateFixedHud(W3DView *view, Bool inGame);
	/// Paint a single 0-9 control-group numeral as a small seven-segment figure under a unit's
	/// health bar, or hide its segments when there is no group. marker indexes m_groupDigit.
	void setGroupDigit(Int marker, Int digit, const Vector3 &centre, const Vector3 &right,
		const Vector3 &down, Real halfW, Real halfH, Real width);
	void updatePanelToggles();
	void applyControlGroup(Int slot, Bool assign);

	/// The radial (pie) menus: tap the LEFT stick in for commands (stop, attack-move, guard...),
	/// LONG-press the right B for control groups 1-10. Flick the stick to a sector, let it
	/// return to centre and the sector fires - both radials are one-thumb gestures, which is
	/// why they open on a TAP and stay (a thumb cannot hold B and flick the stick it sits on).
	void updateRadials();
	void openRadial(Int kind);
	void closeRadial();
	void fireRadialSector(Int kind, Int sector);
	/// Radial geometry shared by input and drawing: centre/radius in screen pixels.
	void getRadialGeometry(Int &cx, Int &cy, Int &radius) const;
	/// The 2D laser drawn ON the panel image (hosted quads have no depth to respect the 3D one).
	void drawPanelBeam();
	/// Short menu-button press recenters; a long press locks the camera to the selection.
	void updateMenuButton();
	void toggleFollowSelected();

	// Grab-drag state: where the world was gripped, and the camera position at that moment.
	Bool m_grabbing[2];
	Vector3 m_grabHandWorld[2];   ///< hand position in world units when the grip closed
	Coord3D m_grabCameraPos;      ///< tactical camera ground position when the grip closed
	Real m_grabAngle;             ///< camera yaw when the grip closed
	Real m_grabScale;             ///< world-units-per-metre when the two-handed grab began
	Real m_grabHandSpan;          ///< distance between hands when the two-handed grab began
	Bool m_twoHandGrab;

	// Throwing the map: a flick leaves the world sliding, and it coasts to a stop. Without this
	// every centimetre of travel costs a full arm movement.
	Coord2D m_slideVelocity;      ///< world units per second
	Coord3D m_lastCameraPos;

	Bool m_hasAimPoint;
	Coord3D m_aimPoint;
	Bool m_leftDown;              ///< synthetic mouse button state we have injected
	Bool m_rightDown;
	Bool m_rayOnScreenPanel;      ///< the right ray rests on a UI panel this frame
	Bool m_cursorParked;          ///< the injected cursor sits in the windowless corner

	// The laser pointers.
	SimpleSceneClass *m_rayScene;
	Line3DClass *m_rayLines[2];
	Bool m_rayVisible[2];

	// Physical-mouse activity, for the mouse+keyboard aids (on-screen cursor + monitor frame).
	Bool m_cursorActive;              ///< cursor moved recently (physical mouse OR controller ray)
	Bool m_mouseActive;               ///< the PHYSICAL mouse moved recently (ray-driven motion excluded)
	Bool m_mouseSeeded;               ///< first frame only records the position, does not count as a move
	Int m_mousePrevX, m_mousePrevY;   ///< cursor pixel last frame
	UnsignedInt m_cursorLastMoveTime; ///< GetTickCount() of the last cursor move from either source
	UnsignedInt m_mouseLastMoveTime;  ///< GetTickCount() of the last PHYSICAL move
	Int m_injectedCursorX, m_injectedCursorY; ///< pixel the controller ray last drove the cursor to
	Line3DClass *m_monitorLines[4];   ///< the four edges of the monitor-view frame on the ground
	Line3DClass *m_mouseMarkerLines[2]; ///< a crosshair where the mouse points on the battlefield
	Bool m_mouseKbMode;               ///< current mode: TRUE = mouse+keyboard, FALSE = rays (auto-switched)
	Vector3 m_prevCtrlPos[2];         ///< last controller positions, to detect controller movement
	Real m_prevCtrlQuat[2][4];        ///< last controller orientations
	Bool m_prevInGame;                ///< menu/battle transition detector (cursor warps there must not flip modes)
	UnsignedInt m_modeGraceUntil;     ///< until this tick, mouse movement cannot steal control (loading warps)
	UnsignedInt m_ctrlLastMoveTime;   ///< GetTickCount() of the last real controller movement
	UnsignedInt m_rayPressFirstTime;  ///< start of the current trigger-mash burst (mouse mode only)
	Int m_rayPressCount;              ///< trigger presses inside the burst window; 3 = "give me the rays"
	Bool m_ctrlSeeded;                ///< first frame just records controller poses
	Bool m_ctrlSpaceWasDown;          ///< edge-detect Ctrl+Space (recenter)
	Real m_fixedHudAlpha;             ///< eased HUD opacity, full over the HUD and faded off it
	Bool m_mouseOverHud;              ///< the cursor is on the control-bar strip (hide the world marker there)
	Bool m_menuOpen;                 ///< an in-game menu/dialog is up (Escape menu, options, quit-confirm, promotion)
	Bool m_menuOpenExternal;         ///< same, but NOT counting the radial dial (the dial gates on this)
	Bool m_modalMenuOpen;            ///< a MODAL window is up (Escape menu family): black-backed on the panel
	Int m_menuWinX, m_menuWinY;      ///< screen rect of the biggest window menu detection found
	Int m_menuWinW, m_menuWinH;

	// The radial menus.
	enum { VR_RADIAL_NONE = 0, VR_RADIAL_COMMANDS, VR_RADIAL_GROUPS };
	Int m_radialKind;
	Int m_radialSector;              ///< sector the stick last pointed at; -1 = none yet
	UnsignedInt m_radialOpenTime;
	Bool m_radialStickWasOut;        ///< deflected past the pick threshold (fire on the way back)
	UnsignedInt m_bDownTime;         ///< right B press time: short = panel toggle, long = groups radial
	Bool m_bLongFired;
	Int m_lastGroupRecalled;         ///< recall the same group twice quickly = jump the camera to it
	UnsignedInt m_lastGroupTime;
	Int m_armedOrder;                ///< 0 none, 1 attack-move, 2 guard: next trigger places it
	UnsignedInt m_lastOwnSelectTime; ///< double-tap a unit = select all matching on screen
	UnsignedInt m_lastStickJumpTime; ///< double-click the right stick = last radar event
	UnsignedInt m_menuBtnDownTime;   ///< three-bar button: short recenters, long follows selection
	Bool m_menuBtnLongFired;
	Bool m_followActive;
	class DisplayString *m_radialStrings[16]; ///< pooled labels for the radial drawing
	UnsignedInt m_clickGuardUntil;   ///< after a dial fires, the trigger is dead this long (its own release must not click)
	Bool m_panelBeamValid;           ///< the 2D beam ON the panel this frame (hosted quads cover the 3D one)
	Int m_panelBeamX0, m_panelBeamY0; ///< the hand dropped onto the panel, frame pixels (may be off-frame)
	Int m_panelBeamX1, m_panelBeamY1; ///< where the ray strikes the panel, frame pixels
	Line3DClass *m_groupDigit[MAX_SELECTION_MARKERS][7]; ///< seven-segment control-group numeral per unit
};

extern VRControls *TheVRControls;  ///< nullptr unless the game runs with -vr
