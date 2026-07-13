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

class VRControls
{
public:
	VRControls();

	/// Once per frame, before the game client consumes input.
	void update();

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
	void updateLocomotion(W3DView *view);
	void updatePointer(W3DView *view);

	// Grab-drag state: where the world was gripped, and the camera position at that moment.
	Bool m_grabbing[2];
	Vector3 m_grabHandWorld[2];   ///< hand position in world units when the grip closed
	Coord3D m_grabCameraPos;      ///< tactical camera ground position when the grip closed
	Real m_grabAngle;             ///< camera yaw when the grip closed
	Real m_grabScale;             ///< world-units-per-metre when the two-handed grab began
	Real m_grabHandSpan;          ///< distance between hands when the two-handed grab began
	Bool m_twoHandGrab;

	Bool m_hasAimPoint;
	Coord3D m_aimPoint;
	Bool m_leftDown;              ///< synthetic mouse button state we have injected
	Bool m_rightDown;
};

extern VRControls *TheVRControls;  ///< nullptr unless the game runs with -vr
