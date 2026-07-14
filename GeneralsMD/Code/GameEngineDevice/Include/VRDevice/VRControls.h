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
	Bool m_placing;
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
	/// Where the ray lands: an object if it hits one, otherwise the ground.
	Bool traceAim(const Vector3 &origin, const Vector3 &dir, Coord3D &outHit) const;
	void updateLocomotion(W3DView *view);
	void updatePointer(W3DView *view);
	void updateRays(W3DView *view);
	void updatePanelToggles();
	void applyControlGroup(Int slot, Bool assign);

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

	// The laser pointers.
	SimpleSceneClass *m_rayScene;
	Line3DClass *m_rayLines[2];
	Bool m_rayVisible[2];
};

extern VRControls *TheVRControls;  ///< nullptr unless the game runs with -vr
