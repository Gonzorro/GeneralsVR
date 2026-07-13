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

// FILE: VRControls.cpp //////////////////////////////////////////////////////////////////////////
// GeneralsVR @feature Motion controller locomotion and pointing. See VRControls.h.
///////////////////////////////////////////////////////////////////////////////////////////////////

#include <windows.h>
#include <windowsx.h>

#include "VRDevice/VRControls.h"
#include "VRDevice/OpenXRManager.h"

#include "Common/Debug.h"
#include "Common/FramePacer.h"
#include "Common/GlobalData.h"
#include "GameClient/View.h"
#include "GameClient/Mouse.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/TerrainLogic.h"
#include "W3DDevice/GameClient/W3DView.h"
#include "Win32Device/GameClient/Win32Mouse.h"
#include "WWMath/quat.h"

VRControls *TheVRControls = nullptr;

namespace
{
	// Thumbstick feel. Pan is in metres of table per second, so it scales naturally with how
	// big the map currently is; rotation and zoom are per second too.
	const Real STICK_DEADZONE      = 0.20f;
	const Real STICK_PAN_SPEED     = 1.6f;    // metres of table per second at full deflection
	const Real STICK_TURN_SPEED    = 1.2f;    // radians per second
	const Real STICK_ZOOM_SPEED    = 0.9f;    // fraction of current scale per second

	// How far a controller ray will search for ground, in world units.
	const Real AIM_MAX_DISTANCE    = 20000.0f;
	const Real AIM_STEP            = 60.0f;

	// The map may be scaled between these (world units per metre). Small = the world is huge
	// around you; large = a small table you look down on.
	const Real MIN_SCALE           = 80.0f;
	const Real MAX_SCALE           = 4000.0f;

	Real applyDeadzone(Real v)
	{
		if (v > STICK_DEADZONE)  return (v - STICK_DEADZONE) / (1.0f - STICK_DEADZONE);
		if (v < -STICK_DEADZONE) return (v + STICK_DEADZONE) / (1.0f - STICK_DEADZONE);
		return 0.0f;
	}
}

//-------------------------------------------------------------------------------------------------
VRControls::VRControls()
	: m_grabAngle(0.0f)
	, m_grabScale(0.0f)
	, m_grabHandSpan(0.0f)
	, m_twoHandGrab(FALSE)
	, m_hasAimPoint(FALSE)
	, m_leftDown(FALSE)
	, m_rightDown(FALSE)
{
	m_grabbing[0] = m_grabbing[1] = FALSE;
	m_grabHandWorld[0] = m_grabHandWorld[1] = Vector3(0.0f, 0.0f, 0.0f);
	m_grabCameraPos.zero();
	m_aimPoint.zero();
}

//-------------------------------------------------------------------------------------------------
/** The headset's world-space frame. Position and heading come from the tactical camera; pitch
	* and roll are deliberately dropped, because inheriting the RTS camera's downward tilt would
	* tip the player's horizon and make them ill. Identical math to the stereo renderer - they
	* must agree, or the hands would not line up with what the eyes see. */
//-------------------------------------------------------------------------------------------------
Bool VRControls::getAnchor(W3DView *view, Matrix3D &outAnchor)
{
	if (view == nullptr)
		return FALSE;

	CameraClass *camera = view->get3DCamera();
	if (camera == nullptr)
		return FALSE;

	const Matrix3D &cameraTransform = camera->Get_Transform();
	const Vector3 worldUp(0.0f, 0.0f, 1.0f);	// the game world is Z-up

	Vector3 forward = -cameraTransform.Get_Z_Vector();
	forward.Z = 0.0f;
	if (forward.Length2() < 0.0001f)
		forward = Vector3(1.0f, 0.0f, 0.0f);
	forward.Normalize();

	Vector3 right;
	Vector3::Cross_Product(forward, worldUp, &right);
	right.Normalize();

	// W3D camera axes: X right, Y up, Z backward.
	outAnchor = Matrix3D(right, worldUp, -forward, camera->Get_Position());
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
/** A controller's aim ray, in world space: the same anchor the eyes use, composed with the
	* hand's pose. The controller points down its own -Z, exactly like a camera. */
//-------------------------------------------------------------------------------------------------
Bool VRControls::computeHandRay(Int hand, Vector3 &outOrigin, Vector3 &outDir) const
{
	if (TheOpenXR == nullptr)
		return FALSE;

	const VRControllerState &c = TheOpenXR->getController(hand);
	if (!c.poseValid)
		return FALSE;

	Matrix3D anchor;
	if (!getAnchor((W3DView *)TheTacticalView, anchor))
		return FALSE;

	const Real scale = TheOpenXR->getWorldUnitsPerMeter();

	Quaternion q(c.quatX, c.quatY, c.quatZ, c.quatW);
	Matrix3D handPose;
	Build_Matrix3D(q, handPose);
	handPose.Set_Translation(Vector3(c.posX * scale, c.posY * scale, c.posZ * scale));

	Matrix3D handWorld;
	Matrix3D::Multiply(anchor, handPose, &handWorld);

	outOrigin = handWorld.Get_Translation();
	outDir = -handWorld.Get_Z_Vector();	// the controller points along its -Z
	outDir.Normalize();
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
/** March the ray until it goes under the terrain, then bisect. The heightfield is not analytic,
	* so a march is both the simplest and the most robust way to hit it. */
//-------------------------------------------------------------------------------------------------
Bool VRControls::traceTerrain(const Vector3 &origin, const Vector3 &dir, Coord3D &outHit) const
{
	if (TheTerrainLogic == nullptr)
		return FALSE;

	// A ray pointing up (or dead level) will never meet the ground.
	if (dir.Z > -0.001f)
		return FALSE;

	Vector3 prev = origin;
	Real prevGap = origin.Z - TheTerrainLogic->getGroundHeight(origin.X, origin.Y);

	for (Real t = AIM_STEP; t <= AIM_MAX_DISTANCE; t += AIM_STEP)
	{
		Vector3 p = origin + dir * t;
		const Real gap = p.Z - TheTerrainLogic->getGroundHeight(p.X, p.Y);

		if (gap <= 0.0f && prevGap > 0.0f)
		{
			// Crossed the surface between prev and p: bisect to a tight hit.
			Vector3 lo = prev, hi = p;
			for (Int i = 0; i < 12; ++i)
			{
				Vector3 mid = (lo + hi) * 0.5f;
				if (mid.Z - TheTerrainLogic->getGroundHeight(mid.X, mid.Y) > 0.0f)
					lo = mid;
				else
					hi = mid;
			}
			outHit.x = hi.X;
			outHit.y = hi.Y;
			outHit.z = TheTerrainLogic->getGroundHeight(hi.X, hi.Y);
			return TRUE;
		}

		prev = p;
		prevGap = gap;
	}

	return FALSE;
}

//-------------------------------------------------------------------------------------------------
/** Grab the world and drag it; grab with both hands to scale and turn it. */
//-------------------------------------------------------------------------------------------------
void VRControls::updateLocomotion(W3DView *view)
{
	const VRControllerState &left = TheOpenXR->getController(VR_HAND_LEFT);
	const VRControllerState &right = TheOpenXR->getController(VR_HAND_RIGHT);
	const VRControllerState *hands[2] = { &left, &right };

	Matrix3D anchor;
	if (!getAnchor(view, anchor))
		return;

	const Real scale = TheOpenXR->getWorldUnitsPerMeter();

	// Hand positions in world units (no rotation needed - we only care where they are).
	Vector3 handWorld[2];
	Bool handValid[2];
	for (Int hand = 0; hand < 2; ++hand)
	{
		const VRControllerState &c = *hands[hand];
		handValid[hand] = c.poseValid;
		if (!c.poseValid)
			continue;
		Matrix3D pose(TRUE);
		pose.Set_Translation(Vector3(c.posX * scale, c.posY * scale, c.posZ * scale));
		Matrix3D world;
		Matrix3D::Multiply(anchor, pose, &world);
		handWorld[hand] = world.Get_Translation();
	}

	const Bool leftGrab = left.grip && handValid[VR_HAND_LEFT];
	const Bool rightGrab = right.grip && handValid[VR_HAND_RIGHT];

	// Starting or restarting a grab: snapshot where the hands and the camera are, so the drag
	// is always measured from the moment of the grip rather than accumulating drift.
	const Bool bothNow = leftGrab && rightGrab;
	const Bool anyNow = leftGrab || rightGrab;
	const Bool wasGrabbing = m_grabbing[0] || m_grabbing[1];

	if (anyNow && (!wasGrabbing || bothNow != m_twoHandGrab))
	{
		m_grabCameraPos = view->getPosition();
		m_grabAngle = view->getAngle();
		m_grabScale = scale;
		m_twoHandGrab = bothNow;
		for (Int hand = 0; hand < 2; ++hand)
			m_grabHandWorld[hand] = handWorld[hand];

		if (bothNow)
		{
			const Vector3 span = handWorld[1] - handWorld[0];
			m_grabHandSpan = span.Length();
		}
	}
	m_grabbing[VR_HAND_LEFT] = leftGrab;
	m_grabbing[VR_HAND_RIGHT] = rightGrab;

	if (anyNow)
	{
		if (bothNow && m_grabHandSpan > 1.0f)
		{
			// Two hands: pulling them apart magnifies the table (you dive in), pushing them
			// together shrinks it (you rise above the whole battle).
			const Vector3 span = handWorld[1] - handWorld[0];
			const Real spanNow = span.Length();
			if (spanNow > 1.0f)
			{
				Real newScale = m_grabScale * (m_grabHandSpan / spanNow);
				if (newScale < MIN_SCALE) newScale = MIN_SCALE;
				if (newScale > MAX_SCALE) newScale = MAX_SCALE;
				TheWritableGlobalData->m_vrWorldUnitsPerMeter = newScale;
			}
		}

		// One hand (or the midpoint of two): the world follows the hand. Moving your hand
		// right pushes the world right, which means the camera goes left.
		Vector3 dragNow(0.0f, 0.0f, 0.0f);
		Vector3 dragStart(0.0f, 0.0f, 0.0f);
		Int dragCount = 0;
		for (Int hand = 0; hand < 2; ++hand)
		{
			if (!m_grabbing[hand] || !handValid[hand])
				continue;
			dragNow += handWorld[hand];
			dragStart += m_grabHandWorld[hand];
			++dragCount;
		}
		if (dragCount > 0)
		{
			dragNow /= (Real)dragCount;
			dragStart /= (Real)dragCount;

			Coord3D pos;
			pos.x = m_grabCameraPos.x - (dragNow.X - dragStart.X);
			pos.y = m_grabCameraPos.y - (dragNow.Y - dragStart.Y);
			pos.z = m_grabCameraPos.z;
			view->lookAt(&pos);
		}
	}
	else
	{
		m_twoHandGrab = FALSE;
	}

	// Thumbsticks: the same verbs without the arm movement. Left pans, right turns and zooms.
	const Real dt = TheFramePacer != nullptr ? TheFramePacer->getUpdateTime() : (1.0f / 90.0f);

	const Real panX = applyDeadzone(left.stickX);
	const Real panY = applyDeadzone(left.stickY);
	if (panX != 0.0f || panY != 0.0f)
	{
		// Pan along the direction you are facing, in table-metres so it feels the same at any
		// zoom level.
		const Vector3 forward = -anchor.Get_Z_Vector();
		const Vector3 right2 = anchor.Get_X_Vector();
		const Real step = STICK_PAN_SPEED * scale * dt;

		Coord3D pos = view->getPosition();
		pos.x += (forward.X * panY + right2.X * panX) * step;
		pos.y += (forward.Y * panY + right2.Y * panX) * step;
		view->lookAt(&pos);
	}

	const Real turn = applyDeadzone(right.stickX);
	if (turn != 0.0f)
		view->setAngle(view->getAngle() + turn * STICK_TURN_SPEED * dt);

	const Real zoom = applyDeadzone(right.stickY);
	if (zoom != 0.0f)
	{
		// Push forward to grow the table (dive in), pull back to shrink it (rise above).
		Real newScale = TheOpenXR->getWorldUnitsPerMeter() * (1.0f - zoom * STICK_ZOOM_SPEED * dt);
		if (newScale < MIN_SCALE) newScale = MIN_SCALE;
		if (newScale > MAX_SCALE) newScale = MAX_SCALE;
		TheWritableGlobalData->m_vrWorldUnitsPerMeter = newScale;
	}
}

//-------------------------------------------------------------------------------------------------
/** Point at the battlefield and click. The ray's ground hit is projected into the tactical
	* camera's screen space and injected as a real Win32 mouse event, so selection, band boxes
	* and orders all run through the engine's own input pipeline untouched. */
//-------------------------------------------------------------------------------------------------
void VRControls::updatePointer(W3DView *view)
{
	m_hasAimPoint = FALSE;

	Win32Mouse *mouse = (Win32Mouse *)TheMouse;
	if (mouse == nullptr)
		return;

	// The right hand points; the left hand's trigger is the right mouse button (move / attack),
	// which keeps both of the RTS verbs on triggers where they belong.
	Vector3 origin, dir;
	if (!computeHandRay(VR_HAND_RIGHT, origin, dir))
		return;

	Coord3D hit;
	if (!traceTerrain(origin, dir, hit))
		return;

	ICoord2D screen;
	if (!view->worldToScreen(&hit, &screen))
		return;	// pointing somewhere the flat camera cannot see; leave the cursor alone

	m_hasAimPoint = TRUE;
	m_aimPoint = hit;

	const DWORD now = GetTickCount();
	const LPARAM packed = MAKELPARAM(screen.x, screen.y);

	mouse->addWin32Event(WM_MOUSEMOVE, 0, packed, now);

	const VRControllerState &right = TheOpenXR->getController(VR_HAND_RIGHT);
	const VRControllerState &left = TheOpenXR->getController(VR_HAND_LEFT);

	// Right trigger = left click (select, band box, confirm).
	if (right.trigger && !m_leftDown)
	{
		m_leftDown = TRUE;
		mouse->addWin32Event(WM_LBUTTONDOWN, MK_LBUTTON, packed, now);
	}
	else if (!right.trigger && m_leftDown)
	{
		m_leftDown = FALSE;
		mouse->addWin32Event(WM_LBUTTONUP, 0, packed, now);
	}

	// Left trigger = right click (move, attack, cancel).
	if (left.trigger && !m_rightDown)
	{
		m_rightDown = TRUE;
		mouse->addWin32Event(WM_RBUTTONDOWN, MK_RBUTTON, packed, now);
	}
	else if (!left.trigger && m_rightDown)
	{
		m_rightDown = FALSE;
		mouse->addWin32Event(WM_RBUTTONUP, 0, packed, now);
	}
}

//-------------------------------------------------------------------------------------------------
void VRControls::update()
{
	if (TheOpenXR == nullptr || !TheOpenXR->isFrameActive())
		return;

	W3DView *view = (W3DView *)TheTacticalView;
	if (view == nullptr)
		return;

	// Only drive the battlefield while there is a battlefield: in menus the tactical view is
	// not meaningful and the controllers must not fling the camera around.
	if (TheGameLogic == nullptr || !TheGameLogic->isInGame())
		return;

	updateLocomotion(view);
	updatePointer(view);
}
