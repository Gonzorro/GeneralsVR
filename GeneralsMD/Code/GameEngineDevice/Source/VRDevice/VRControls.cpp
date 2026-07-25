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
#include "Common/MessageStream.h"
#include "GameClient/Display.h"
#include "GameClient/Drawable.h"
#include "GameClient/GameClient.h"
#include "GameClient/DrawableInfo.h"
#include "GameClient/InGameUI.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/CommandXlat.h"
#include "GameLogic/Object.h"
#include "Common/ThingTemplate.h"
#include "GameClient/View.h"
#include "GameClient/Mouse.h"
#include "GameClient/Keyboard.h"
#include "GameClient/KeyDefs.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/TerrainLogic.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "W3DDevice/GameClient/W3DDisplay.h"
#include "W3DDevice/GameClient/W3DScene.h"
#include "W3DDevice/GameClient/W3DView.h"
#include "Win32Device/GameClient/Win32Mouse.h"
#include "WW3D2/coltest.h"
#include "WW3D2/dx8wrapper.h"
#include "WW3D2/line3d.h"
#include "WW3D2/rendobj.h"
#include "WW3D2/sphereobj.h"
#include "WW3D2/scene.h"
#include "WW3D2/ww3d.h"
#include "WWMath/lineseg.h"
#include "WWMath/plane.h"
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

	// Dragging the world. The grab is 1:1 - the ground stays stuck to your hand, which is the
	// whole point of grabbing it - and releasing mid-sweep lets it coast a little further.
	// Everything here is deliberately tame: a throw that overshoots the map is useless.
	const Real GRAB_GAIN           = 1.0f;   ///< 1:1. The world stays under your hand.
	const Real SLIDE_FRICTION      = 7.0f;   ///< per second; a throw dies away in a few tenths
	const Real SLIDE_MIN_SPEED     = 0.05f;  ///< table-metres per second below which we stop
	const Real SLIDE_MAX_SPEED     = 2.5f;   ///< table-metres per second; hard ceiling on a throw
	const Real SLIDE_SMOOTHING     = 0.25f;  ///< how much of each frame's velocity we believe

	// Flying up and down over the battlefield.
	const Real STICK_HEIGHT_SPEED  = 1.6f;    ///< fraction of the current height, per second
	const Real MIN_HEIGHT          = 30.0f;   ///< low enough to stand among the tanks
	const Real MAX_HEIGHT          = 6000.0f; ///< high enough to hold the whole map

	const Real RAY_WIDTH_METERS    = 0.004f;
	const Real RAY_MAX_METERS      = 6.0f;   ///< how far a laser reaches when it hits nothing

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
	, m_rayScene(nullptr)
{
	m_grabbing[0] = m_grabbing[1] = FALSE;
	m_grabHandWorld[0] = m_grabHandWorld[1] = Vector3(0.0f, 0.0f, 0.0f);
	m_grabCameraPos.zero();
	m_lastCameraPos.zero();
	m_aimPoint.zero();
	m_slideVelocity.x = m_slideVelocity.y = 0.0f;

	// The laser pointers live in their own scene so the stereo renderer can draw them over the
	// battlefield - and, in the menus, on their own with nothing else in the world.
	m_rayScene = NEW_REF(SimpleSceneClass, ());
	for (Int hand = 0; hand < 2; ++hand)
	{
		// Cyan for the pointing hand, amber for the other, so they are told apart at a glance.
		const Real r = (hand == VR_HAND_RIGHT) ? 0.35f : 1.0f;
		const Real g = (hand == VR_HAND_RIGHT) ? 0.85f : 0.72f;
		const Real b = (hand == VR_HAND_RIGHT) ? 1.00f : 0.25f;
		m_rayLines[hand] = NEW_REF(Line3DClass, (Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 0.0f, 1.0f),
			1.0f, r, g, b, 0.85f));
		m_rayScene->Add_Render_Object(m_rayLines[hand]);
		m_rayLines[hand]->Set_Hidden(true);
		m_rayVisible[hand] = FALSE;
	}

	// The four sides of the selection box drawn on the ground while sweeping.
	for (Int i = 0; i < 4; ++i)
	{
		m_boxLines[i] = NEW_REF(Line3DClass, (Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 0.0f, 1.0f),
			1.0f, 0.30f, 1.0f, 0.45f, 0.9f));
		m_rayScene->Add_Render_Object(m_boxLines[i]);
		m_boxLines[i]->Set_Hidden(true);
	}

	// The four sides of the monitor-view frame: what the flat screen is rendering, laid on the
	// ground. Soft white so it reads as a screen boundary, apart from the green select box and the
	// coloured hand rays.
	for (Int i = 0; i < 4; ++i)
	{
		m_monitorLines[i] = NEW_REF(Line3DClass, (Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 0.0f, 1.0f),
			1.0f, 0.92f, 0.92f, 0.98f, 0.75f));
		m_rayScene->Add_Render_Object(m_monitorLines[i]);
		m_monitorLines[i]->Set_Hidden(true);
	}

	// A two-stroke crosshair marking where the mouse points on the battlefield. Warm yellow so it
	// reads as the cursor, apart from the white frame, green box and coloured rays.
	for (Int i = 0; i < 2; ++i)
	{
		m_mouseMarkerLines[i] = NEW_REF(Line3DClass, (Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 0.0f, 1.0f),
			1.0f, 1.0f, 0.85f, 0.15f, 0.95f));
		m_rayScene->Add_Render_Object(m_mouseMarkerLines[i]);
		m_mouseMarkerLines[i]->Set_Hidden(true);
	}

	// Seven-segment numerals for the control-group number under each unit's health bar. Bright so
	// they read against the battlefield.
	for (Int m = 0; m < MAX_SELECTION_MARKERS; ++m)
		for (Int s = 0; s < 7; ++s)
		{
			m_groupDigit[m][s] = NEW_REF(Line3DClass, (Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 0.0f, 1.0f),
				1.0f, 1.0f, 1.0f, 0.35f, 1.0f));
			m_rayScene->Add_Render_Object(m_groupDigit[m][s]);
			m_groupDigit[m][s]->Set_Hidden(true);
		}

	// A green bead over each selected unit, with its health slung underneath.
	for (Int i = 0; i < MAX_SELECTION_MARKERS; ++i)
	{
		m_selectionBeads[i] = NEW_REF(SphereRenderObjClass, ());
		m_selectionBeads[i]->Set_Color(Vector3(0.25f, 1.0f, 0.35f));
		m_selectionBeads[i]->Set_Alpha(0.85f);
		m_rayScene->Add_Render_Object(m_selectionBeads[i]);
		m_selectionBeads[i]->Set_Hidden(true);

		m_healthBack[i] = NEW_REF(Line3DClass, (Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 0.0f, 1.0f),
			1.0f, 0.75f, 0.10f, 0.10f, 0.9f));
		m_rayScene->Add_Render_Object(m_healthBack[i]);
		m_healthBack[i]->Set_Hidden(true);

		m_healthFill[i] = NEW_REF(Line3DClass, (Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 0.0f, 1.0f),
			1.0f, 0.25f, 1.0f, 0.30f, 1.0f));
		m_rayScene->Add_Render_Object(m_healthFill[i]);
		m_healthFill[i]->Set_Hidden(true);
	}

	m_boxing = FALSE;
	m_boxArmed = FALSE;
	m_placing = FALSE;
	m_placeArmed = FALSE;
	m_placePressTime = 0;
	m_placeTurning = FALSE;
	m_placeAnchor.zero();
	m_placeAngle = 0.0f;
	m_boxStart.zero();
	m_boxEnd.zero();
	m_boxRight = Vector3(1.0f, 0.0f, 0.0f);
	m_boxForward = Vector3(0.0f, 1.0f, 0.0f);
	m_boxPressTime = 0;

	m_cursorActive = FALSE;
	m_mouseActive = FALSE;
	m_mouseSeeded = FALSE;
	m_prevInGame = FALSE;
	m_modeGraceUntil = 0;
	m_mousePrevX = m_mousePrevY = 0;
	m_cursorLastMoveTime = 0;
	m_mouseLastMoveTime = 0;
	m_injectedCursorX = m_injectedCursorY = -100000;	// a pixel the real cursor can never sit at

	// Rays are the PRIMARY input; moving the physical mouse auto-switches to mouse+keyboard, and a
	// few idle seconds hands it back. (The VR settings menu can pin a mode later.)
	m_mouseKbMode = FALSE;
	for (Int i = 0; i < 2; ++i)
	{
		m_prevCtrlPos[i] = Vector3(0.0f, 0.0f, 0.0f);
		m_prevCtrlQuat[i][0] = m_prevCtrlQuat[i][1] = m_prevCtrlQuat[i][2] = 0.0f;
		m_prevCtrlQuat[i][3] = 1.0f;
	}
	m_ctrlLastMoveTime = 0;
	m_rayPressFirstTime = 0;
	m_rayPressCount = 0;
	m_ctrlSeeded = FALSE;
	m_ctrlSpaceWasDown = FALSE;
	m_fixedHudAlpha = 1.0f;
	m_mouseOverHud = FALSE;
	m_menuOpen = FALSE;
}

VRControls::~VRControls()
{
	for (Int hand = 0; hand < 2; ++hand)
	{
		if (m_rayLines[hand] != nullptr)
		{
			if (m_rayScene != nullptr)
				m_rayScene->Remove_Render_Object(m_rayLines[hand]);
			m_rayLines[hand]->Release_Ref();
			m_rayLines[hand] = nullptr;
		}
	}
	if (m_rayScene != nullptr)
	{
		m_rayScene->Release_Ref();
		m_rayScene = nullptr;
	}
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
/** Cast the ray at the things the player actually wants to click: units, buildings, everything
	* in the scene. This is the same cast the engine performs for a mouse click - it just starts
	* from the controller instead of from a pixel. */
//-------------------------------------------------------------------------------------------------
Bool VRControls::traceScene(const Vector3 &origin, const Vector3 &dir, Coord3D &outHit) const
{
	if (W3DDisplay::m_3DScene == nullptr)
		return FALSE;

	LineSegClass lineSeg;
	lineSeg.Set(origin, origin + dir * AIM_MAX_DISTANCE);

	CastResultStruct result;
	result.ComputeContactPoint = true;	// we need WHERE it hit, not just that it did

	RayCollisionTestClass rayTest(lineSeg, &result, COLL_TYPE_ALL, false, false);

	if (!W3DDisplay::m_3DScene->castRay(rayTest, true, (Int)PICK_TYPE_ALL_DRAWABLES))
		return FALSE;
	if (rayTest.CollidedRenderObj == nullptr)
		return FALSE;

	// Only report things that belong to a drawable - the terrain has no drawable behind it and
	// is handled by the ground trace instead.
	DrawableInfo *info = (DrawableInfo *)rayTest.CollidedRenderObj->Get_User_Data();
	if (info == nullptr || info->m_drawable == nullptr)
		return FALSE;
	if (info->m_drawable->getFullyObscuredByShroud())
		return FALSE;	// the laser does not reach into the fog

	// RTS3DScene::castRay does NOT fill in the CastResultStruct we hand it - it tests each object
	// with a result of its own. What it gives back instead is a CLIPPED RAY: rayTest.Ray now ends
	// exactly at the intersection. Reading Fraction (still 1.0, untouched) or ContactPoint (still
	// the origin) puts the hit twenty thousand units away, underground, which then projects to
	// nowhere and silently swallows every click on a unit.
	const Vector3 point = rayTest.Ray.Get_P1();

	outHit.x = point.X;
	outHit.y = point.Y;
	outHit.z = point.Z;
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
/** The drawable under the ray, if any. */
//-------------------------------------------------------------------------------------------------
Drawable *VRControls::pickDrawable(const Vector3 &origin, const Vector3 &dir) const
{
	if (W3DDisplay::m_3DScene == nullptr)
		return nullptr;

	LineSegClass lineSeg;
	lineSeg.Set(origin, origin + dir * AIM_MAX_DISTANCE);

	CastResultStruct result;
	RayCollisionTestClass rayTest(lineSeg, &result, COLL_TYPE_ALL, false, false);

	if (!W3DDisplay::m_3DScene->castRay(rayTest, true, (Int)PICK_TYPE_ALL_DRAWABLES))
		return nullptr;
	if (rayTest.CollidedRenderObj == nullptr)
		return nullptr;

	DrawableInfo *info = (DrawableInfo *)rayTest.CollidedRenderObj->Get_User_Data();
	Drawable *draw = (info != nullptr) ? info->m_drawable : nullptr;

	// What the shroud hides, the laser cannot touch. The ray reaches into the dark parts of the
	// map where a mouse could never click, so without this the player could pick enemies out of
	// unexplored fog - and read their health off the bar.
	if (draw != nullptr && draw->getFullyObscuredByShroud())
		return nullptr;

	return draw;
}

//-------------------------------------------------------------------------------------------------
/** Select whatever the laser is on. This does NOT go through a screen pixel: the cursor lives in
	* the flat camera's view, which is far narrower than the headset's, so most of what the player
	* can plainly see does not project onto it and the click vanishes. We build the very message
	* the mouse translator would have built, and hand it to the engine directly. */
//-------------------------------------------------------------------------------------------------
void VRControls::selectUnderRay(const Vector3 &origin, const Vector3 &dir)
{
	if (TheInGameUI == nullptr || TheMessageStream == nullptr)
		return;

	Drawable *draw = pickDrawable(origin, dir);

	if (draw == nullptr || !draw->isSelectable() || draw->getObject() == nullptr)
	{
		// Empty ground: clear the selection, exactly as clicking bare terrain does.
		TheInGameUI->deselectAllDrawables();
		TheMessageStream->appendMessage(GameMessage::MSG_DESTROY_SELECTED_GROUP);
		return;
	}

	TheInGameUI->deselectAllDrawables();
	TheInGameUI->selectDrawable(draw);

	GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_CREATE_SELECTED_GROUP);
	msg->appendBooleanArgument(TRUE);	// a fresh group, not an addition
	msg->appendObjectIDArgument(draw->getObject()->getID());
}

//-------------------------------------------------------------------------------------------------
/** Putting a building down.
	*
	* This was the worst-behaved thing in the game and the reason is worth stating: the engine's
	* placement ghost follows the MOUSE CURSOR, which it turns into a world position with
	* screenToTerrain. Our cursor is a fiction - it only exists where the flat camera can see, and
	* the flat camera sees a fraction of what the player does - so the ghost lurched, stuck, and
	* refused to land where the laser pointed.
	*
	* So placement leaves the cursor out of it entirely. The laser gives a world position; a
	* footprint is drawn there from the building's own geometry; and the trigger sends the same
	* MSG_DOZER_CONSTRUCT the mouse would have sent, with that position. The angle comes from the
	* way the player is facing, which is what you would want anyway.
	*/
//-------------------------------------------------------------------------------------------------
void VRControls::updatePlacement(const Vector3 &origin, const Vector3 &dir)
{
	const ThingTemplate *build = (TheInGameUI != nullptr) ? TheInGameUI->getPendingPlaceType() : nullptr;
	if (build == nullptr)
	{
		if (m_placing)
		{
			// Leave nothing behind. A press time left lying around here is a press time that is
			// already hundreds of milliseconds old the next time a building is picked up - which
			// reads as 'held long enough to turn' the instant placement begins.
			m_placing = FALSE;
			m_placeTurning = FALSE;
			m_placePressTime = 0;
			m_placeArmed = FALSE;
			if (TheWritableGlobalData != nullptr)
				TheWritableGlobalData->m_vrPlaceAngleValid = FALSE;
			updateBoxVisual(FALSE);
		}
		return;
	}

	// THE TRIGGER THAT PICKED THE BUILDING IS NOT THE TRIGGER THAT PLACES IT.
	//
	// The building is chosen by pulling the trigger on the menu, and that trigger is still down when
	// placement begins a frame later. Taken as the placement press it starts the hold timer at once,
	// pinned to wherever the beam happened to cross the ground on its way to the menu - so half a
	// second later the building is spinning, anchored to a spot the player never chose, and there is
	// no way to commit it. Which is exactly what it did.
	//
	// So placement will not listen to a trigger it never saw go down: the player must LET GO first.
	if (!m_placing)
		m_placeArmed = FALSE;

	m_placing = TRUE;

	Coord3D spot;
	if (!traceTerrain(origin, dir, spot))
		return;

	// The building itself is the preview: the engine's own ghost, with its own legality tint,
	// follows the laser because the aim point is published where the placement code reads it
	// (see GlobalData::m_vrAimPoint). No stand-in footprint required - you see the building.

	const VRControllerState &right = TheOpenXR->getController(VR_HAND_RIGHT);
	const UnsignedInt now = GetTickCount();

	// TAP to drop it as it stands. HOLD, and the building pins itself where you first pointed and
	// turns to follow the laser - sweep the beam around it like a compass needle, release to
	// commit. The threshold is what separates the two, and it costs a tap nothing.
	const UnsignedInt HOLD_TO_TURN_MS = 500;

	// Let go of the menu's trigger, and from here on the trigger is the placement's own.
	if (!right.trigger)
		m_placeArmed = TRUE;

	if (!m_placeArmed)
		return;

	if (right.triggerPressed)
	{
		m_placePressTime = now;
		m_placeAnchor = spot;
		m_placeTurning = FALSE;
		return;
	}

	if (right.trigger && m_placePressTime != 0)
	{
		if (!m_placeTurning && (now - m_placePressTime) >= HOLD_TO_TURN_MS)
			m_placeTurning = TRUE;

		if (m_placeTurning)
		{
			// The angle from where the building sits to where the beam now points.
			const Real dx = spot.x - m_placeAnchor.x;
			const Real dy = spot.y - m_placeAnchor.y;
			if ((dx * dx + dy * dy) > 1.0f)
				m_placeAngle = atan2f(dy, dx);

			TheWritableGlobalData->m_vrPlaceAngleValid = TRUE;
			TheWritableGlobalData->m_vrPlaceAngle = m_placeAngle;
			TheWritableGlobalData->m_vrAimPoint = m_placeAnchor;	// it stays put while it turns
		}
		return;
	}

	const Bool releasedAfterHold = (!right.trigger && m_placePressTime != 0);
	if (!releasedAfterHold && !right.primaryPressed)
		return;

	// Where and how it lands: where the beam was when it was pinned, turned however the player
	// turned it. A tap never entered the turn, so it takes the heading the player is facing.
	const Coord3D where = (m_placePressTime != 0) ? m_placeAnchor : spot;

	// A tap builds it exactly as the ghost was standing. Only the ghost knows what heading that
	// is - inventing one (the camera's angle, or a bare zero) means the building that lands is
	// not the building the player was looking at.
	const Real angle = m_placeTurning ? m_placeAngle : TheInGameUI->getPlaceIconAngle();

	GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_DOZER_CONSTRUCT);
	msg->appendIntegerArgument(build->getTemplateID());
	msg->appendLocationArgument(where);
	msg->appendRealArgument(angle);

	// Leave placement mode, exactly as the mouse path does once it has placed.
	TheInGameUI->placeBuildAvailable(nullptr, nullptr);
	m_placing = FALSE;
	m_placeTurning = FALSE;
	m_placePressTime = 0;
	TheWritableGlobalData->m_vrPlaceAngleValid = FALSE;

	DEBUG_LOG(("OpenXR: placed building at (%.0f %.0f) angle %.0f",
		where.x, where.y, angle * 57.2958f));
}

//-------------------------------------------------------------------------------------------------
/** Everything inside the swept box, if it is yours. Mirrors what a mouse drag does. */
//-------------------------------------------------------------------------------------------------
namespace
{
	struct BoxSelectContext
	{
		GameMessage *msg;
		Int count;
		Vector3 origin;             ///< the corner the sweep began at
		Vector3 right, forward;     ///< the player's frame, which the box is drawn in
		Real acrossLo, acrossHi;
		Real awayLo, awayHi;
	};

	void addDrawableToSelection(Drawable *draw, void *userData)
	{
		BoxSelectContext *ctx = (BoxSelectContext *)userData;
		if (draw == nullptr || ctx == nullptr || ctx->count >= 256)
			return;
		if (!draw->isSelectable())
			return;

		Object *obj = draw->getObject();
		if (obj == nullptr || !obj->isLocallyControlled())
			return;	// a box drag takes YOUR units, never the enemy's

		// And never your BUILDINGS. Sweeping up a barracks along with the tanks standing next to
		// it means the group can no longer be told to do anything a tank does - the order gets
		// refused because half the selection cannot move. A box is for units.
		if (obj->isKindOf(KINDOF_STRUCTURE))
			return;

		// Inside the TURNED box, not merely inside the world-aligned one we asked the engine for.
		const Coord3D *pos = draw->getPosition();
		if (pos == nullptr)
			return;

		const Vector3 offset(pos->x - ctx->origin.X, pos->y - ctx->origin.Y, 0.0f);
		const Real across = Vector3::Dot_Product(offset, ctx->right);
		const Real away = Vector3::Dot_Product(offset, ctx->forward);
		if (across < ctx->acrossLo || across > ctx->acrossHi
			|| away < ctx->awayLo || away > ctx->awayHi)
			return;

		TheInGameUI->selectDrawable(draw);
		ctx->msg->appendObjectIDArgument(obj->getID());
		++ctx->count;
	}
}

void VRControls::selectInBox(const Coord3D &corner0, const Coord3D &corner1)
{
	if (TheGameClient == nullptr || TheInGameUI == nullptr || TheMessageStream == nullptr)
		return;

	// The box the player drew is turned to face them, so the units it takes have to be found in
	// that same turned frame - otherwise the rectangle on the ground and the rectangle doing the
	// selecting are two different rectangles, and the wrong tanks come along.
	const Vector3 start(corner0.x, corner0.y, 0.0f);
	const Vector3 span(corner1.x - corner0.x, corner1.y - corner0.y, 0.0f);
	const Real acrossExtent = Vector3::Dot_Product(span, m_boxRight);
	const Real awayExtent = Vector3::Dot_Product(span, m_boxForward);

	const Vector3 c0 = start;
	const Vector3 c1 = start + m_boxRight * acrossExtent;
	const Vector3 c2 = c1 + m_boxForward * awayExtent;
	const Vector3 c3 = start + m_boxForward * awayExtent;

	// A world-aligned box around the turned one, to ask the engine for candidates cheaply.
	Region3D region;
	region.lo.x = min(min(c0.X, c1.X), min(c2.X, c3.X));
	region.hi.x = max(max(c0.X, c1.X), max(c2.X, c3.X));
	region.lo.y = min(min(c0.Y, c1.Y), min(c2.Y, c3.Y));
	region.hi.y = max(max(c0.Y, c1.Y), max(c2.Y, c3.Y));
	// Tall on purpose: aircraft and the tops of buildings are inside a box drawn on the ground.
	region.lo.z = min(corner0.z, corner1.z) - 500.0f;
	region.hi.z = max(corner0.z, corner1.z) + 2000.0f;

	TheInGameUI->deselectAllDrawables();

	BoxSelectContext ctx;
	ctx.msg = TheMessageStream->appendMessage(GameMessage::MSG_CREATE_SELECTED_GROUP);
	ctx.msg->appendBooleanArgument(TRUE);	// a fresh group
	ctx.count = 0;
	ctx.origin = start;
	ctx.right = m_boxRight;
	ctx.forward = m_boxForward;
	ctx.acrossLo = min(0.0f, acrossExtent);
	ctx.acrossHi = max(0.0f, acrossExtent);
	ctx.awayLo = min(0.0f, awayExtent);
	ctx.awayHi = max(0.0f, awayExtent);

	TheGameClient->iterateDrawablesInRegion(&region, addDrawableToSelection, &ctx);

	DEBUG_LOG(("OpenXR: box select: %d units", ctx.count));
}

//-------------------------------------------------------------------------------------------------
/** Draw the box on the ground as it is swept. */
//-------------------------------------------------------------------------------------------------
void VRControls::updateBoxVisual(Bool visible)
{
	if (!visible)
	{
		for (Int i = 0; i < 4; ++i)
		{
			if (m_boxLines[i] != nullptr)
				m_boxLines[i]->Set_Hidden(true);
		}
		return;
	}

	if (TheTerrainLogic == nullptr)
		return;

	const Real width = (3.0f + 0.004f * TheOpenXR->getWorldUnitsPerMeter()) * 0.2f;
	const Real lift = 4.0f;	// float it clear of the ground so it is not swallowed by the terrain

	// The box lives in the player's frame: one pair of edges runs left-right across their view,
	// the other away from them. Dragged in the world's frame it would sit at a crooked angle the
	// moment they turned, and they would find themselves sweeping a diamond.
	const Vector3 start(m_boxStart.x, m_boxStart.y, 0.0f);
	const Vector3 end(m_boxEnd.x, m_boxEnd.y, 0.0f);
	const Vector3 span = end - start;

	const Real acrossExtent = Vector3::Dot_Product(span, m_boxRight);
	const Real awayExtent = Vector3::Dot_Product(span, m_boxForward);

	const Vector3 across = m_boxRight * acrossExtent;
	const Vector3 away = m_boxForward * awayExtent;

	Vector3 flat[4] =
	{
		start,
		start + across,
		start + across + away,
		start + away,
	};

	// Follow the ground at each corner rather than cutting through the hills.
	Vector3 corners[4];
	for (Int c = 0; c < 4; ++c)
	{
		corners[c] = Vector3(flat[c].X, flat[c].Y,
			TheTerrainLogic->getGroundHeight(flat[c].X, flat[c].Y) + lift);
	}

	for (Int i = 0; i < 4; ++i)
	{
		if (m_boxLines[i] == nullptr)
			continue;
		m_boxLines[i]->Reset(corners[i], corners[(i + 1) % 4], width);
		m_boxLines[i]->Set_Hidden(false);
	}
}

//-------------------------------------------------------------------------------------------------
/** Watch the mouse so the mouse+keyboard aids appear on use and fade when the mouse goes idle.
	*
	* Two signals fall out of this. The on-screen cursor on the VR menu panel follows ANY cursor
	* movement, mouse or controller ray, because pointing at the menu with a controller should show
	* the cursor too. The monitor-view frame follows the PHYSICAL mouse only: the controller ray
	* injects a WM_MOUSEMOVE at the pixel it is aiming at (see updatePointer), so we discount a move
	* that lands exactly there - a real mouse move goes somewhere else. Both fade after a few idle
	* seconds. GetTickCount is a wall clock, so the timer runs even when the game logic is paused. */
//-------------------------------------------------------------------------------------------------
void VRControls::updateMouseActivity()
{
	const UnsignedInt MOUSE_IDLE_HIDE_MS = 5000;	// hide the aids this long after the last move

	m_cursorActive = FALSE;
	m_mouseActive = FALSE;
	if (TheMouse == nullptr)
		return;

	const MouseIO *mio = TheMouse->getMouseStatus();
	const UnsignedInt now = GetTickCount();
	const Int px = mio->pos.x;
	const Int py = mio->pos.y;

	if (!m_mouseSeeded)
	{
		// First frame: adopt the position without treating it as a move, or the aids flash on at
		// startup before the player has touched anything.
		m_mousePrevX = px;
		m_mousePrevY = py;
		m_mouseSeeded = TRUE;
	}
	else if (px != m_mousePrevX || py != m_mousePrevY)
	{
		m_cursorLastMoveTime = now;	// any movement wakes the on-screen cursor
		const Bool fromController = (px == m_injectedCursorX && py == m_injectedCursorY);
		if (!fromController)
			m_mouseLastMoveTime = now;	// only a real mouse move wakes the monitor frame
		m_mousePrevX = px;
		m_mousePrevY = py;
	}

	m_cursorActive = (m_cursorLastMoveTime != 0) && ((now - m_cursorLastMoveTime) < MOUSE_IDLE_HIDE_MS);
	m_mouseActive  = (m_mouseLastMoveTime  != 0) && ((now - m_mouseLastMoveTime)  < MOUSE_IDLE_HIDE_MS);
}

//-------------------------------------------------------------------------------------------------
/** Soft auto-switch between the two input styles. Rays are primary; the moment the PHYSICAL mouse
	* moves, mouse+keyboard takes over (rays hidden, cursor/frame/HUD shown). If the mouse then sits
	* idle for a few seconds WHILE the controllers are being used, control eases back to the rays.
	* The mouse-vs-rays branch in update() reads m_mouseKbMode, so flipping it swaps the whole style. */
//-------------------------------------------------------------------------------------------------
void VRControls::updateInputMode()
{
	if (TheOpenXR == nullptr)
		return;
	const UnsignedInt now = GetTickCount();

	// Did either controller move beyond the natural hand jitter this frame?
	Bool ctrlMoved = FALSE;
	for (Int hand = 0; hand < 2; ++hand)
	{
		const VRControllerState &c = TheOpenXR->getController(hand);
		if (!c.poseValid)
			continue;
		const Vector3 pos(c.posX, c.posY, c.posZ);
		const Real qdot = (Real)fabs(c.quatX * m_prevCtrlQuat[hand][0] + c.quatY * m_prevCtrlQuat[hand][1]
			+ c.quatZ * m_prevCtrlQuat[hand][2] + c.quatW * m_prevCtrlQuat[hand][3]);
		if (m_ctrlSeeded && ((pos - m_prevCtrlPos[hand]).Length() > 0.012f || qdot < 0.9995f))
			ctrlMoved = TRUE;	// ~1.2cm of travel or ~3.6 deg of turn
		m_prevCtrlPos[hand] = pos;
		m_prevCtrlQuat[hand][0] = c.quatX; m_prevCtrlQuat[hand][1] = c.quatY;
		m_prevCtrlQuat[hand][2] = c.quatZ; m_prevCtrlQuat[hand][3] = c.quatW;
	}
	m_ctrlSeeded = TRUE;
	if (ctrlMoved)
		m_ctrlLastMoveTime = now;

	// Moving the mouse takes over immediately. Otherwise, once the mouse has been idle for 3s and a
	// controller is currently in use, hand it back to the rays.
	const Bool mouseJustMoved = (m_mouseLastMoveTime != 0) && ((now - m_mouseLastMoveTime) < 250);
	if (mouseJustMoved && now >= m_modeGraceUntil)
		m_mouseKbMode = TRUE;
	else if (m_mouseKbMode
		&& (now - m_mouseLastMoveTime) >= 3000
		&& m_ctrlLastMoveTime != 0 && (now - m_ctrlLastMoveTime) < 500)
		m_mouseKbMode = FALSE;

	// GeneralsVR @feature Squeezing the trigger a few times is a clear "give me the rays back":
	// the player should not have to wait out the mouse-idle timer or wave the controller around.
	// Three squeezes inside 1.5 seconds flip back to the rays; a single stray squeeze while
	// mousing does not. The flip completes on the RELEASE of the last squeeze, so the squeeze
	// that causes it can never land a ray click on whatever the laser happens to cross.
	if (m_mouseKbMode)
	{
		Bool pressEdge = FALSE, releaseEdge = FALSE, anyDown = FALSE;
		for (Int hand = 0; hand < 2; ++hand)
		{
			const VRControllerState &c = TheOpenXR->getController(hand);
			if (c.triggerPressed)  pressEdge = TRUE;
			if (c.triggerReleased) releaseEdge = TRUE;
			if (c.trigger)         anyDown = TRUE;
		}
		if (pressEdge)
		{
			if (m_rayPressCount == 0 || (now - m_rayPressFirstTime) > 1500)
			{
				m_rayPressFirstTime = now;
				m_rayPressCount = 1;
			}
			else
			{
				m_rayPressCount++;
			}
			m_ctrlLastMoveTime = now;	// mashing counts as "controller in use" for the idle handback too
		}
		if (releaseEdge && !anyDown && m_rayPressCount >= 3 && (now - m_rayPressFirstTime) <= 1500)
		{
			m_mouseKbMode = FALSE;
			m_rayPressCount = 0;
		}
	}
	else
	{
		m_rayPressCount = 0;
	}
}

//-------------------------------------------------------------------------------------------------
/** Draw a frame on the ground showing what the flat monitor renders - the tactical camera's four
	* screen corners dropped onto the terrain. A mouse+keyboard player scrolls by shoving the cursor
	* to the screen edge, but in the headset that edge is invisible; this is where it is. */
//-------------------------------------------------------------------------------------------------
void VRControls::updateMonitorFrame(Bool visible)
{
	if (!visible)
	{
		for (Int i = 0; i < 4; ++i)
			if (m_monitorLines[i] != nullptr)
				m_monitorLines[i]->Set_Hidden(true);
		return;
	}

	if (TheTacticalView == nullptr)
		return;

	// A ground height to lay the frame on: whatever the terrain is under the middle of the view.
	// The centre pixel always lands on the ground, even when the top corners are near the horizon.
	Real groundZ = 0.0f;
	Int ox, oy;
	TheTacticalView->getOrigin(&ox, &oy);
	ICoord2D center = { ox + TheTacticalView->getWidth() / 2, oy + TheTacticalView->getHeight() / 2 };
	Coord3D centerWorld;
	if (TheTacticalView->screenToTerrain(&center, &centerWorld))
		groundZ = centerWorld.z;

	// Drop the four screen corners onto a HORIZONTAL PLANE at that height. Unlike screenToTerrain
	// (a raycast against the finite heightmap, which misses once the view tilts low and the top
	// corners run off the map), an infinite plane always catches them - so the frame stops
	// vanishing when the camera comes down close. This is the same call the radar uses to draw the
	// camera box on the minimap.
	Coord3D tl, tr, br, bl;
	if (TheTacticalView->getScreenCornerWorldPointsAtZ(&tl, &tr, &br, &bl, groundZ) == PlaneClass::NO_INTERSECTION)
	{
		for (Int i = 0; i < 4; ++i)
			if (m_monitorLines[i] != nullptr)
				m_monitorLines[i]->Set_Hidden(true);
		return;
	}

	const Real lift = 6.0f;	// float it above the ground so the terrain does not swallow it
	const Coord3D *src[4] = { &tl, &tr, &br, &bl };
	Vector3 corners[4];
	for (Int c = 0; c < 4; ++c)
	{
		// Sit each corner ON the terrain, not on the flat plane. When the camera drops in low the
		// plane can pass under a rise and the frame sank into the hillside and vanished; following
		// the ground height at each corner keeps it on the surface.
		Real gz = src[c]->z;
		if (TheTerrainLogic != nullptr)
			gz = TheTerrainLogic->getGroundHeight(src[c]->x, src[c]->y);
		corners[c] = Vector3(src[c]->x, src[c]->y, gz + lift);
	}

	const Real width = (3.0f + 0.004f * TheOpenXR->getWorldUnitsPerMeter()) * 0.2f;
	for (Int i = 0; i < 4; ++i)
	{
		if (m_monitorLines[i] == nullptr)
			continue;
		m_monitorLines[i]->Reset(corners[i], corners[(i + 1) % 4], width);
		m_monitorLines[i]->Set_Hidden(false);
	}
}

//-------------------------------------------------------------------------------------------------
/** A crosshair on the ground under the mouse. The cursor pixel is cast through the flat camera to
	* a terrain point (the same mapping the game already uses for clicks), so the mark lands exactly
	* where a mouse order would - inside the monitor-view frame, since that is the region the flat
	* camera can see. */
//-------------------------------------------------------------------------------------------------
Bool VRControls::mousePanelWorldPos(const Matrix3D &anchor, Int mx, Int my, Vector3 &out) const
{
	if (TheOpenXR == nullptr)
		return FALSE;
	const Real uiW = (Real)TheOpenXR->getUiWidth();
	const Real uiH = (Real)TheOpenXR->getUiHeight();
	if (uiW <= 0.0f || uiH <= 0.0f)
		return FALSE;
	const Real scale = TheOpenXR->getWorldUnitsPerMeter();

	for (Int i = 0; i < TheOpenXR->getPanelCount(); ++i)
	{
		OpenXRManager::VRPanelInfo info;
		if (!TheOpenXR->getPanelInfo(i, info) || info.isGroupBar)
			continue;

		// The panel shows the crop [u0..u1]x[v0..v1] of the UI surface; in pixels that is where the
		// mouse must be for the pointer to be ON this panel.
		const Real x0 = info.u0 * uiW, x1 = info.u1 * uiW;
		const Real y0 = info.v0 * uiH, y1 = info.v1 * uiH;
		if (x1 <= x0 || y1 <= y0)
			continue;
		if ((Real)mx < x0 || (Real)mx > x1 || (Real)my < y0 || (Real)my > y1)
			continue;

		const Real u = ((Real)mx - x0) / (x1 - x0);	// 0 left .. 1 right
		const Real v = ((Real)my - y0) / (y1 - y0);	// 0 top  .. 1 bottom

		// The panel's world quad, exactly as drawVRPanels builds it: pose (metres) lifted through the
		// anchor, then half-extents along its own axes.
		Quaternion q(info.quatX, info.quatY, info.quatZ, info.quatW);
		Matrix3D panelPose;
		Build_Matrix3D(q, panelPose);
		panelPose.Set_Translation(Vector3(info.posX * scale, info.posY * scale, info.posZ * scale));
		Matrix3D panelWorld;
		Matrix3D::Multiply(anchor, panelPose, &panelWorld);

		const Vector3 origin = panelWorld.Get_Translation();
		const Vector3 right = panelWorld.Get_X_Vector() * (info.widthMeters * 0.5f * scale);
		const Vector3 up = panelWorld.Get_Y_Vector() * (info.heightMeters * 0.5f * scale);
		// u:0->left(-right),1->right(+right)   v:0->top(+up),1->bottom(-up)
		out = origin + right * (2.0f * u - 1.0f) + up * (1.0f - 2.0f * v);
		return TRUE;
	}
	return FALSE;
}

//-------------------------------------------------------------------------------------------------
void VRControls::updateMouseMarker(Bool visible)
{
	if (!visible || TheMouse == nullptr || TheTacticalView == nullptr)
	{
		for (Int i = 0; i < 2; ++i)
			if (m_mouseMarkerLines[i] != nullptr)
				m_mouseMarkerLines[i]->Set_Hidden(true);
		return;
	}

	Matrix3D anchor;
	const Bool haveAnchor = getAnchor((W3DView *)TheTacticalView, anchor);

	// ONE crosshair for the whole session: it sits on a HUD/menu panel when the mouse is over one,
	// and on the terrain otherwise. The player never learns there are two cursors underneath.
	const MouseIO *mio = TheMouse->getMouseStatus();
	Vector3 c;
	Bool onPanel = FALSE;
	if (haveAnchor && mousePanelWorldPos(anchor, mio->pos.x, mio->pos.y, c))
	{
		onPanel = TRUE;
	}
	else
	{
		ICoord2D mousePixel = { mio->pos.x, mio->pos.y };
		Coord3D world;
		if (!TheTacticalView->screenToTerrain(&mousePixel, &world))
		{
			for (Int i = 0; i < 2; ++i)
				if (m_mouseMarkerLines[i] != nullptr)
					m_mouseMarkerLines[i]->Set_Hidden(true);
			return;
		}
		c = Vector3(world.x, world.y, world.z);
	}

	// Constant APPARENT size, referenced to the player: scale the cross by its distance from the
	// head, so it never shrinks crossing from the far battlefield onto a near menu - it is the same
	// cross the whole way.
	Vector3 headWorld = c;
	if (haveAnchor)
	{
		const VREyeView &head = TheOpenXR->getEyeView(0);
		const Real scale = TheOpenXR->getWorldUnitsPerMeter();
		Matrix3D headPose(TRUE);
		headPose.Set_Translation(Vector3(head.posX * scale, head.posY * scale, head.posZ * scale));
		Matrix3D headM;
		Matrix3D::Multiply(anchor, headPose, &headM);
		headWorld = headM.Get_Translation();
	}
	Real dist = (c - headWorld).Length();
	if (dist < 1.0f) dist = 1.0f;
	const Real half = 0.03f * dist;			// ~constant angular size (tune this factor)
	const Real width = half * 0.14f;

	// FLAT in both cases, parallel to the floor - the menu now lies flat on the floor like the
	// battlefield, so the cross must not stand up on it. Just lifted a touch so it reads on top.
	(void)onPanel;
	c.Z += half * 0.4f;
	const Vector3 axisX(half, 0.0f, 0.0f);
	const Vector3 axisY(0.0f, half, 0.0f);

	if (m_mouseMarkerLines[0] != nullptr)
	{
		m_mouseMarkerLines[0]->Reset(c - axisX, c + axisX, width);
		m_mouseMarkerLines[0]->Set_Hidden(false);
	}
	if (m_mouseMarkerLines[1] != nullptr)
	{
		m_mouseMarkerLines[1]->Reset(c - axisY, c + axisY, width);
		m_mouseMarkerLines[1]->Set_Hidden(false);
	}
}

//-------------------------------------------------------------------------------------------------
/** Edge-scroll: shove the mouse to the screen edge and the tactical camera pans that way, exactly
	* like the monitor. scrollBy is view-relative and zoom-aware, so a fixed delta feels the same at
	* every zoom, and because the VR eyes ride the tactical camera the whole headset view follows. */
//-------------------------------------------------------------------------------------------------
void VRControls::updateMouseScroll(W3DView *view)
{
	if (view == nullptr || TheMouse == nullptr || TheDisplay == nullptr)
		return;

	if (TheGlobalData == nullptr)
		return;

	const MouseIO *mio = TheMouse->getMouseStatus();

	// Don't edge-scroll while a mouse button is held: that is a click or drag - a minimap jump, a
	// command, a band-select. Scrolling underneath stole those (the minimap jump in particular). But
	// plain hover-scroll now works at EVERY edge again, including the bottom over the control bar.
	if (mio->leftState == MBS_Down || mio->rightState == MBS_Down)
		return;
	const Real w = (Real)TheDisplay->getWidth();
	const Real h = (Real)TheDisplay->getHeight();
	// A proper band, not a one-pixel sliver: the cursor is not captured in windowed VR, so it can
	// slip off the exact edge before a thin margin ever catches it. The outer ~5% scrolls, faster
	// the closer to the edge.
	const Real bandX = w * 0.05f;
	const Real bandY = h * 0.05f;
	const Real px = (Real)mio->pos.x;
	const Real py = (Real)mio->pos.y;

	// Penetration into the band, -1..1.
	Real sx = 0.0f, sy = 0.0f;
	if (px < bandX)				sx = -((bandX - px) / bandX);
	else if (px > w - bandX)	sx =  ((px - (w - bandX)) / bandX);
	if (py < bandY)				sy = -((bandY - py) / bandY);
	else if (py > h - bandY)	sy =  ((py - (h - bandY)) / bandY);
	if (sx == 0.0f && sy == 0.0f)
		return;

	// THE FIX for edge-scroll doing nothing: match the game's own magnitude. LookAtXlat's edge
	// scroll passes an offset of order SCROLL_AMT (200) times the user's scroll factors - about
	// 100 per frame - to userScrollBy; my old value of ~0.06 was a thousand times too small to
	// see. SCROLL_BASE is a touch under SCROLL_AMT to sit right at VR frame rates; the settings
	// menu can expose sensitivity later. Use userScrollBy (the same call the game uses), so the
	// scroll goes through the same permission gate.
	const Real SCROLL_BASE = 120.0f;
	const Real kScroll = TheGlobalData->m_keyboardScrollFactor;
	Coord2D delta;
	delta.x = sx * SCROLL_BASE * TheGlobalData->m_horizontalScrollSpeedFactor * kScroll;
	delta.y = sy * SCROLL_BASE * TheGlobalData->m_verticalScrollSpeedFactor * kScroll;

	const Coord3D camBefore = view->getPosition();
	view->userScrollBy(&delta);
	const Coord3D camAfter = view->getPosition();

	static UnsignedInt s_lastScrollLog = 0;
	const UnsignedInt nowMs = GetTickCount();
	if ((nowMs - s_lastScrollLog) > 500)
	{
		s_lastScrollLog = nowMs;
		DEBUG_LOG(("[VRSCROLL] mouse=(%d,%d) disp=%dx%d delta=(%.2f,%.2f) camXY %.1f,%.1f -> %.1f,%.1f",
			mio->pos.x, mio->pos.y, (Int)w, (Int)h, delta.x, delta.y,
			camBefore.x, camBefore.y, camAfter.x, camAfter.y));
	}
}

//-------------------------------------------------------------------------------------------------
/** Place and fade the fixed in-game HUD so it reads as the bottom of the monitor frame. Everything
	* is measured from the frame itself each frame - width, how far in front the near edge is, and
	* the ground drop - so the panel matches the "limits" and tracks them as the camera zooms. It
	* fades down when the mouse is not on the control-bar strip, so it does not veil the battle. */
//-------------------------------------------------------------------------------------------------
void VRControls::updateFixedHud(W3DView *view, Bool inGame)
{
	if (TheOpenXR == nullptr)
		return;

	const Bool show = m_mouseKbMode && inGame && view != nullptr && TheTacticalView != nullptr;
	TheOpenXR->setShowFixedHud(show);
	// (Whether a menu is up - which stands the HUD upright - is measured by updateUiCrop, which
	// pushes it to OpenXR itself; both input modes need it, not just this one.)
	if (!show)
		return;

	const Real scale = TheOpenXR->getWorldUnitsPerMeter();
	if (scale <= 0.0f)
		return;

	Matrix3D anchor;
	if (!getAnchor(view, anchor))
		return;
	const Vector3 camPos = anchor.Get_Translation();

	// Ground level under the headset -> how far down the table is, in metres.
	Real groundZ = camPos.Z;
	if (TheTerrainLogic != nullptr)
		groundZ = TheTerrainLogic->getGroundHeight(camPos.X, camPos.Y);
	const Real drop = (camPos.Z - groundZ) / scale;

	// The monitor frame's near (bottom) edge: width and how far in front it sits, both in metres,
	// from the same projection the frame is drawn with.
	Real widthM = 2.4f, forwardM = 1.2f;
	Int ox, oy;
	TheTacticalView->getOrigin(&ox, &oy);
	ICoord2D centrePix = { ox + TheTacticalView->getWidth() / 2, oy + TheTacticalView->getHeight() / 2 };
	Coord3D cw;
	Real planeZ = groundZ;
	if (TheTacticalView->screenToTerrain(&centrePix, &cw))
		planeZ = cw.z;
	Coord3D tl, tr, br, bl;
	if (TheTacticalView->getScreenCornerWorldPointsAtZ(&tl, &tr, &br, &bl, planeZ) != PlaneClass::NO_INTERSECTION)
	{
		const Vector3 blv(bl.x, bl.y, bl.z), brv(br.x, br.y, br.z);
		widthM = (brv - blv).Length() / scale;

		Vector3 fwd = -anchor.Get_Z_Vector();
		fwd.Z = 0.0f;
		if (fwd.Length2() > 0.0001f)
			fwd.Normalize();
		const Vector3 nearMid = (blv + brv) * 0.5f;
		forwardM = Vector3::Dot_Product(nearMid - camPos, fwd) / scale;
	}
	TheOpenXR->setFixedHudPlacement(drop, widthM, forwardM);

	// The cursor is "on the HUD" when it is on the control-bar strip OR when a full menu is open
	// (the whole screen is then the menu). That hides the battlefield crosshair - so you never see
	// two crosses at once - and keeps the HUD opaque while you use it. Default dim off it is 50%.
	const Real CONTROL_BAR_TOP = 0.66f;
	Bool overStrip = FALSE;
	if (TheMouse != nullptr && TheDisplay != nullptr)
		overStrip = ((Real)TheMouse->getMouseStatus()->pos.y >= CONTROL_BAR_TOP * (Real)TheDisplay->getHeight());
	m_mouseOverHud = overStrip || m_menuOpen;
	const Real targetAlpha = m_mouseOverHud ? 1.0f : 0.5f;
	m_fixedHudAlpha += (targetAlpha - m_fixedHudAlpha) * 0.2f;
	TheOpenXR->setFixedHudAlpha(m_fixedHudAlpha);
}

//-------------------------------------------------------------------------------------------------
/** Draw the mouse's drag-select band box on the ground. The game already does the actual selection
	* from the physical mouse; the flat UI just draws the rectangle in screen space, which the
	* headset cannot show - so we project its four corners onto the terrain and draw it there,
	* reusing the same line objects the controller box-select uses (idle in mouse mode). */
//-------------------------------------------------------------------------------------------------
void VRControls::updateMouseBoxSelect(W3DView *view)
{
	if (view == nullptr || TheTacticalView == nullptr || TheInGameUI == nullptr
		|| !TheInGameUI->isSelecting())
	{
		for (Int i = 0; i < 4; ++i)
			if (m_boxLines[i] != nullptr)
				m_boxLines[i]->Set_Hidden(true);
		return;
	}

	const IRegion2D *reg = TheInGameUI->getDragSelectRegion();
	if (reg == nullptr)
		return;

	ICoord2D sc[4] =
	{
		{ reg->lo.x, reg->lo.y },
		{ reg->hi.x, reg->lo.y },
		{ reg->hi.x, reg->hi.y },
		{ reg->lo.x, reg->hi.y },
	};

	const Real lift = 4.0f;
	Vector3 corners[4];
	for (Int c = 0; c < 4; ++c)
	{
		Coord3D wpt;
		if (!TheTacticalView->screenToTerrain(&sc[c], &wpt))
		{
			for (Int i = 0; i < 4; ++i)
				if (m_boxLines[i] != nullptr)
					m_boxLines[i]->Set_Hidden(true);
			return;
		}
		corners[c] = Vector3(wpt.x, wpt.y, wpt.z + lift);
	}

	const Real width = (3.0f + 0.004f * TheOpenXR->getWorldUnitsPerMeter()) * 0.2f;
	for (Int i = 0; i < 4; ++i)
	{
		if (m_boxLines[i] == nullptr)
			continue;
		m_boxLines[i]->Reset(corners[i], corners[(i + 1) % 4], width);
		m_boxLines[i]->Set_Hidden(false);
	}
}

//-------------------------------------------------------------------------------------------------
/** Hold the trigger and sweep the laser across the ground to take everything inside the box. A
	* short press is still a single click, so the two gestures do not fight. */
//-------------------------------------------------------------------------------------------------
void VRControls::updateBoxSelect(const Vector3 &origin, const Vector3 &dir)
{
	const VRControllerState &right = TheOpenXR->getController(VR_HAND_RIGHT);

	Coord3D ground;
	const Bool onGround = traceTerrain(origin, dir, ground);

	if (right.triggerPressed && onGround)
	{
		m_boxArmed = TRUE;
		m_boxing = FALSE;
		m_boxStart = ground;
		m_boxEnd = ground;
		m_boxPressTime = GetTickCount();

		// Take the player's heading now and hold it for the whole sweep, so the box does not
		// swivel under their hand if they turn their head mid-drag.
		m_boxRight = Vector3(1.0f, 0.0f, 0.0f);
		m_boxForward = Vector3(0.0f, 1.0f, 0.0f);

		Matrix3D anchor;
		if (getAnchor((W3DView *)TheTacticalView, anchor))
		{
			const VREyeView &head = TheOpenXR->getEyeView(0);
			Quaternion headQuat(head.quatX, head.quatY, head.quatZ, head.quatW);
			Matrix3D headRot;
			Build_Matrix3D(headQuat, headRot);
			Matrix3D headWorld;
			Matrix3D::Multiply(anchor, headRot, &headWorld);

			Vector3 gaze = -headWorld.Get_Z_Vector();
			gaze.Z = 0.0f;
			if (gaze.Length2() > 0.0001f)
			{
				gaze.Normalize();
				m_boxForward = gaze;
				Vector3::Cross_Product(gaze, Vector3(0.0f, 0.0f, 1.0f), &m_boxRight);
				m_boxRight.Normalize();
			}
		}
	}
	else if (right.trigger && m_boxArmed && onGround)
	{
		m_boxEnd = ground;

		// A box has to be HELD, and it has to be swept. Distance alone was not enough: a click
		// that drifts a few feet while the trigger is going down would quietly become a box and
		// take half the base with it. A quarter of a second is the line between the two.
		const UnsignedInt HOLD_TO_BOX_MS = 250;
		const Bool heldLongEnough = (GetTickCount() - m_boxPressTime) >= HOLD_TO_BOX_MS;

		const Real dx = m_boxEnd.x - m_boxStart.x;
		const Real dy = m_boxEnd.y - m_boxStart.y;
		const Real minSweep = 0.05f * TheOpenXR->getWorldUnitsPerMeter();
		const Bool sweptFarEnough = (dx * dx + dy * dy) > (minSweep * minSweep);

		if (!m_boxing && heldLongEnough && sweptFarEnough)
			m_boxing = TRUE;

		if (m_boxing)
			updateBoxVisual(TRUE);
	}
	else if (!right.trigger && m_boxArmed)
	{
		if (m_boxing)
			selectInBox(m_boxStart, m_boxEnd);

		m_boxArmed = FALSE;
		m_boxing = FALSE;
		updateBoxVisual(FALSE);
	}
}

//-------------------------------------------------------------------------------------------------
/** A short post over each selected unit, so a selection made from across the map is visible. */
//-------------------------------------------------------------------------------------------------
void VRControls::updateSelectionMarkers()
{
	Int used = 0;

	if (TheInGameUI != nullptr && TheGameLogic != nullptr && TheGameLogic->isInGame())
	{
		const DrawableList *selected = TheInGameUI->getAllSelectedDrawables();
		if (selected != nullptr)
		{
			const Real scale = TheOpenXR->getWorldUnitsPerMeter();

			// Everything is sized in metres of the PLAYER, so a bead stays a bead whether you
			// are a giant over a tabletop or standing among the tanks.
			const Real beadRadius = 0.012f * scale;
			const Real lift = 0.05f * scale;
			const Real barWidth = 0.10f * scale;
			const Real barThickness = 0.008f * scale;

			// Lay the health bar broadside to the player, so it reads from wherever they stand.
			Matrix3D anchor;
			Vector3 barRight(1.0f, 0.0f, 0.0f);
			if (getAnchor((W3DView *)TheTacticalView, anchor))
			{
				const VREyeView &head = TheOpenXR->getEyeView(0);
				Quaternion headQuat(head.quatX, head.quatY, head.quatZ, head.quatW);
				Matrix3D headRot;
				Build_Matrix3D(headQuat, headRot);
				Matrix3D headWorld;
				Matrix3D::Multiply(anchor, headRot, &headWorld);

				Vector3 gaze = -headWorld.Get_Z_Vector();
				gaze.Z = 0.0f;
				if (gaze.Length2() > 0.0001f)
				{
					gaze.Normalize();
					Vector3::Cross_Product(gaze, Vector3(0.0f, 0.0f, 1.0f), &barRight);
					barRight.Normalize();
				}
			}

			for (DrawableList::const_iterator it = selected->begin();
				it != selected->end() && used < MAX_SELECTION_MARKERS; ++it)
			{
				Drawable *draw = *it;
				if (draw == nullptr || draw->getFullyObscuredByShroud())
					continue;	// nothing under the fog gives its health away

				const Coord3D *pos = draw->getPosition();
				if (pos == nullptr)
					continue;

				// The bead sits above the unit's own height, not above its feet, or it would be
				// buried inside anything taller than a rifleman.
				Real top = pos->z + lift;
				const Object *obj = draw->getObject();
				if (obj != nullptr)
					top = pos->z + obj->getGeometryInfo().getMaxHeightAbovePosition() + lift;

				// The health bar alone says which units are yours to command - a bead on top of it
				// was saying the same thing twice.
				(void)beadRadius;

				// Health, floating over the unit.
				Real health = 1.0f;
				if (obj != nullptr && obj->getBodyModule() != nullptr)
				{
					const Real maxHealth = obj->getBodyModule()->getMaxHealth();
					if (maxHealth > 0.0f)
						health = obj->getBodyModule()->getHealth() / maxHealth;
				}
				if (health < 0.0f) health = 0.0f;
				if (health > 1.0f) health = 1.0f;

				const Vector3 barCentre(pos->x, pos->y, top);
				const Vector3 barLeft = barCentre - barRight * (barWidth * 0.5f);
				const Vector3 barEnd = barCentre + barRight * (barWidth * 0.5f);
				const Vector3 fillEnd = barLeft + barRight * (barWidth * health);

				m_healthBack[used]->Reset(barLeft, barEnd, barThickness);
				m_healthBack[used]->Set_Hidden(false);

				if (health > 0.01f)
				{
					m_healthFill[used]->Reset(barLeft, fillEnd, barThickness * 1.25f);
					m_healthFill[used]->Set_Hidden(false);
				}
				else
				{
					m_healthFill[used]->Set_Hidden(true);
				}

				// Control-group number: the seven-segment figure read as ugly 3D. Hidden for now; the
				// follow-up is a proper billboard TEXT numeral (a font, facing the camera like the
				// health bar), the way the flat game shows it.
				setGroupDigit(used, -1, barCentre, barRight, Vector3(0.0f, 0.0f, -1.0f), 1.0f, 1.0f, 1.0f);

				++used;
			}
		}
	}

	for (Int i = used; i < MAX_SELECTION_MARKERS; ++i)
	{
		if (m_selectionBeads[i] != nullptr) m_selectionBeads[i]->Set_Hidden(true);
		if (m_healthBack[i] != nullptr) m_healthBack[i]->Set_Hidden(true);
		if (m_healthFill[i] != nullptr) m_healthFill[i]->Set_Hidden(true);
		for (Int s = 0; s < 7; ++s)
			if (m_groupDigit[i][s] != nullptr) m_groupDigit[i][s]->Set_Hidden(true);
	}
}

//-------------------------------------------------------------------------------------------------
/** Paint one 0-9 control-group numeral as a seven-segment figure, or hide it (digit < 0). Segment
	* endpoints live in a [0,1] cell (x along `right`, y along `down`); we map them into the world
	* plane about `centre`. Same numbering the flat game shows under the health bar. */
//-------------------------------------------------------------------------------------------------
void VRControls::setGroupDigit(Int marker, Int digit, const Vector3 &centre, const Vector3 &right,
	const Vector3 &down, Real halfW, Real halfH, Real width)
{
	if (marker < 0 || marker >= MAX_SELECTION_MARKERS)
		return;

	// Segment bits: a=1 b=2 c=4 d=8 e=16 f=32 g=64.
	static const UnsignedInt SEG[10] =
		{ 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
	static const Real END[7][4] =
	{
		{ 0.0f, 0.0f, 1.0f, 0.0f },   // a  top
		{ 1.0f, 0.0f, 1.0f, 0.5f },   // b  top-right
		{ 1.0f, 0.5f, 1.0f, 1.0f },   // c  bottom-right
		{ 0.0f, 1.0f, 1.0f, 1.0f },   // d  bottom
		{ 0.0f, 0.5f, 0.0f, 1.0f },   // e  bottom-left
		{ 0.0f, 0.0f, 0.0f, 0.5f },   // f  top-left
		{ 0.0f, 0.5f, 1.0f, 0.5f },   // g  middle
	};

	const UnsignedInt mask = (digit >= 0 && digit <= 9) ? SEG[digit] : 0u;

	for (Int s = 0; s < 7; ++s)
	{
		Line3DClass *seg = m_groupDigit[marker][s];
		if (seg == nullptr)
			continue;
		if (!(mask & (1u << s)))
		{
			seg->Set_Hidden(true);
			continue;
		}
		const Vector3 p0 = centre + right * ((END[s][0] - 0.5f) * 2.0f * halfW)
		                          + down  * ((END[s][1] - 0.5f) * 2.0f * halfH);
		const Vector3 p1 = centre + right * ((END[s][2] - 0.5f) * 2.0f * halfW)
		                          + down  * ((END[s][3] - 0.5f) * 2.0f * halfH);
		seg->Reset(p0, p1, width);
		seg->Set_Hidden(false);
	}
}

//-------------------------------------------------------------------------------------------------
/** Order the selection to whatever the laser is on: attack an object, or move to a patch of
	* ground. Also world-space, for the same reason. */
//-------------------------------------------------------------------------------------------------
void VRControls::commandUnderRay(const Vector3 &origin, const Vector3 &dir)
{
	if (TheGameClient == nullptr || TheMessageStream == nullptr)
		return;

	// Holding the left hand's button forces the attack: shoot it whatever it is, ally, neutral
	// building or empty dirt. This is the headset's Ctrl key.
	if (TheOpenXR->getController(VR_HAND_LEFT).secondaryButton)
	{
		Drawable *target = pickDrawable(origin, dir);
		if (target != nullptr && target->getObject() != nullptr)
		{
			GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_DO_FORCE_ATTACK_OBJECT);
			msg->appendObjectIDArgument(target->getObject()->getID());
			return;
		}

		Coord3D spot;
		if (traceTerrain(origin, dir, spot))
		{
			GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_DO_FORCE_ATTACK_GROUND);
			msg->appendLocationArgument(spot);
		}
		return;
	}

	// Hand it to the engine's own context evaluation - the very thing a right-click goes
	// through. It decides between attack, capture, enter, repair, garrison and plain movement
	// by looking at what is under the pointer and what is selected. Hard-coding attack-or-move
	// here is why a building could never be captured: there is no message for "capture", only a
	// context that resolves to one.
	Drawable *draw = pickDrawable(origin, dir);

	Coord3D ground;
	const Bool onGround = traceTerrain(origin, dir, ground);
	if (draw == nullptr && !onGround)
		return;

	const Coord3D *pos = onGround ? &ground : nullptr;
	if (pos == nullptr && draw != nullptr)
		pos = draw->getPosition();

	TheGameClient->evaluateContextCommand(draw, pos, CommandTranslator::DO_COMMAND);
}

//-------------------------------------------------------------------------------------------------
/** The reticle. A cursor cannot follow the laser out here, so the BEAM is the cursor: it takes
	* the colour of whatever the game would do if you pressed the button now. Red to attack, amber
	* to take a building, green to move. You aim, and the beam tells you what will happen. */
//-------------------------------------------------------------------------------------------------
void VRControls::getReticleColor(const Vector3 &origin, const Vector3 &dir,
	Real &outR, Real &outG, Real &outB) const
{
	// Idle: the plain pointing colour.
	outR = 0.35f; outG = 0.85f; outB = 1.00f;

	if (TheGameClient == nullptr || TheInGameUI == nullptr)
		return;
	if (TheInGameUI->getAllSelectedDrawables() == nullptr
		|| TheInGameUI->getAllSelectedDrawables()->empty())
		return;	// nothing selected: nothing would happen, so promise nothing

	Drawable *draw = pickDrawable(origin, dir);
	Coord3D ground;
	const Bool onGround = traceTerrain(origin, dir, ground);

	const Coord3D *pos = onGround ? &ground : (draw != nullptr ? draw->getPosition() : nullptr);
	if (pos == nullptr)
		return;

	const GameMessage::Type command =
		TheGameClient->evaluateContextCommand(draw, pos, CommandTranslator::EVALUATE_ONLY);

	switch (command)
	{
		case GameMessage::MSG_DO_ATTACK_OBJECT:
		case GameMessage::MSG_DO_FORCE_ATTACK_OBJECT:
		case GameMessage::MSG_DO_FORCE_ATTACK_GROUND:
		case GameMessage::MSG_DO_WEAPON_AT_OBJECT:
		case GameMessage::MSG_DO_WEAPON_AT_LOCATION:
			outR = 1.00f; outG = 0.20f; outB = 0.15f;	// attack
			break;

		case GameMessage::MSG_DO_SPECIAL_POWER_AT_OBJECT:
		case GameMessage::MSG_DO_SPECIAL_POWER_AT_LOCATION:
		case GameMessage::MSG_ENTER:
		case GameMessage::MSG_GET_REPAIRED:
		case GameMessage::MSG_GET_HEALED:
		case GameMessage::MSG_DOCK:
		case GameMessage::MSG_DO_REPAIR:
		case GameMessage::MSG_RESUME_CONSTRUCTION:
			outR = 1.00f; outG = 0.75f; outB = 0.15f;	// take it, enter it, fix it
			break;

		case GameMessage::MSG_DO_MOVETO:
		case GameMessage::MSG_DO_ATTACKMOVETO:
			outR = 0.30f; outG = 1.00f; outB = 0.40f;	// move
			break;

		default:
			break;
	}
}

//-------------------------------------------------------------------------------------------------
/** Where the laser lands: on an object if it hits one, otherwise on the ground. */
//-------------------------------------------------------------------------------------------------
Bool VRControls::traceAim(const Vector3 &origin, const Vector3 &dir, Coord3D &outHit) const
{
	// While a building is being placed, aim at the GROUND ONLY. The placement ghost is a real
	// drawable that follows the cursor, so a scene cast would strike the ghost, place the cursor
	// on it, move the ghost there, and strike it again - the building crawls around under your
	// hand and can never be put down. This is also the only sane answer: you are choosing a
	// patch of ground, not clicking an object.
	if (TheInGameUI != nullptr && TheInGameUI->getPendingPlaceType() != nullptr)
		return traceTerrain(origin, dir, outHit);

	if (traceScene(origin, dir, outHit))
		return TRUE;
	return traceTerrain(origin, dir, outHit);
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

	// Grips are ONLY for resizing the world now: the left stick moves you, so a one-handed drag
	// competing with it was both redundant and the source of the runaway slide. Two grips
	// together pinch the battlefield bigger or smaller - pull your hands apart to shrink down
	// into it and see the fighting up close, push them together to grow and take in the whole map.
	const Bool leftGrab = left.grip && handValid[VR_HAND_LEFT];
	const Bool rightGrab = right.grip && handValid[VR_HAND_RIGHT];
	const Bool pinching = leftGrab && rightGrab;

	if (pinching)
	{
		const Vector3 span = handWorld[1] - handWorld[0];
		const Real spanNow = span.Length();

		if (!m_twoHandGrab)
		{
			// A pinch begins: remember what the world looked like at this hand separation.
			m_twoHandGrab = TRUE;
			m_grabScale = scale;
			m_grabHandSpan = spanNow;
		}
		else if (spanNow > 1.0f && m_grabHandSpan > 1.0f)
		{
			Real newScale = m_grabScale * (m_grabHandSpan / spanNow);
			if (newScale < MIN_SCALE) newScale = MIN_SCALE;
			if (newScale > MAX_SCALE) newScale = MAX_SCALE;
			TheOpenXR->setWorldUnitsPerMeter(newScale);
		}
	}
	else
	{
		m_twoHandGrab = FALSE;
	}

	// Thumbsticks: left pans, right turns and zooms.
	const Real dt = TheFramePacer != nullptr ? TheFramePacer->getUpdateTime() : (1.0f / 90.0f);

	const Real panX = applyDeadzone(left.stickX);
	const Real panY = applyDeadzone(left.stickY);
	if (panX != 0.0f || panY != 0.0f)
	{
		// Pan along the way the PLAYER is looking, not the way the tactical camera happens to
		// face. In a headset you turn your head and expect forward to be wherever you are now
		// looking; taking the direction from the anchor instead meant pushing the stick "forward"
		// sent you off sideways the moment you looked around.
		Vector3 forward = -anchor.Get_Z_Vector();
		Vector3 right2 = anchor.Get_X_Vector();

		const VREyeView &head = TheOpenXR->getEyeView(0);
		Quaternion headQuat(head.quatX, head.quatY, head.quatZ, head.quatW);
		Matrix3D headRot;
		Build_Matrix3D(headQuat, headRot);

		Matrix3D headWorld;
		Matrix3D::Multiply(anchor, headRot, &headWorld);

		Vector3 gaze = -headWorld.Get_Z_Vector();
		gaze.Z = 0.0f;	// flatten: looking down must not drive you into the ground
		if (gaze.Length2() > 0.0001f)
		{
			gaze.Normalize();
			forward = gaze;
			const Vector3 worldUp(0.0f, 0.0f, 1.0f);
			Vector3::Cross_Product(forward, worldUp, &right2);
			right2.Normalize();
		}

		const Real step = STICK_PAN_SPEED * scale * dt;

		Coord3D pos = view->getPosition();
		pos.x += (forward.X * panY + right2.X * panX) * step;
		pos.y += (forward.Y * panY + right2.Y * panX) * step;
		view->lookAt(&pos);
	}

	// The right stick does ONE thing at a time - whichever way you pushed it hardest. Letting a
	// diagonal both turn and resize meant every rotation quietly changed your size as well.
	const Real turn = applyDeadzone(right.stickX);
	const Real grow = applyDeadzone(right.stickY);

	if (fabsf(turn) > fabsf(grow))
	{
		if (turn != 0.0f)
		{
			// Push right, turn right.
			const Real delta = -turn * STICK_TURN_SPEED * dt;

			// Turn on the spot. The RTS camera orbits its look-at point, so turning swung the
			// player around a pivot far out on the battlefield - you were on the end of a boom,
			// not standing and looking around. To turn in place, spin the look-at point around
			// the PLAYER instead, which leaves the camera exactly where it is and only changes
			// which way it faces.
			CameraClass *camera = view->get3DCamera();
			if (camera != nullptr)
			{
				const Vector3 me = camera->Get_Position();
				const Coord3D target = view->getPosition();

				const Real dx = target.x - me.X;
				const Real dy = target.y - me.Y;
				const Real c = cosf(delta);
				const Real s = sinf(delta);

				Coord3D newTarget;
				newTarget.x = me.X + dx * c - dy * s;
				newTarget.y = me.Y + dx * s + dy * c;
				newTarget.z = target.z;

				view->setAngle(view->getAngle() + delta);

				// lookAt also marks the view dirty, which setAngle does not do - without it the
				// camera is never rebuilt and the heading is a number nothing reads.
				view->lookAt(&newTarget);
			}
		}
	}
	else if (grow != 0.0f)
	{
		// Up and down now simply FLY YOU, straight up and straight down over the battlefield -
		// no resizing, no change of scale, just altitude. Rising to look over the whole map and
		// dropping back down among the tanks is the movement people actually reach for; changing
		// how big you are was a stranger idea than it sounded.
		//
		// The step grows with the height you are already at, so it is fine at both ends: gentle
		// when you are down among the units, brisk when you are up looking at the whole map.
		view->setZoomLimited(FALSE);	// the RTS height clamp has no business up here

		const Real currentHeight = view->getHeightAboveGround();
		Real newHeight = currentHeight * (1.0f + grow * STICK_HEIGHT_SPEED * dt);
		if (newHeight < MIN_HEIGHT) newHeight = MIN_HEIGHT;
		if (newHeight > MAX_HEIGHT) newHeight = MAX_HEIGHT;
		view->setHeightAboveGround(newHeight);

		// The camera is only rebuilt when the view is marked dirty, and setHeightAboveGround does
		// not mark it - the same trap the rotation fell into.
		const Coord3D here = view->getPosition();
		view->lookAt(&here);
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
	if (mouse == nullptr || TheOpenXR == nullptr)
		return;

	ICoord2D screen;
	Bool haveTarget = FALSE;

	const VRControllerState &rightState = TheOpenXR->getController(VR_HAND_RIGHT);

	// A UI panel always wins over the world behind it: if the ray lands on the menu screen or on
	// a wrist panel, the cursor goes there. The panel already knows which pixel of the game's own
	// frame the ray hit, so the engine's GUI sees an ordinary cursor over an ordinary button.
	Int panelX = 0, panelY = 0;
	const OpenXRManager::VRPickKind pick = TheOpenXR->pickUiPanel(VR_HAND_RIGHT, panelX, panelY);

	if (pick == OpenXRManager::VR_PICK_GROUP_SLOT)
	{
		// The control-group bar is ours, not the game's: a trigger pull recalls that squad, and
		// holding the LEFT hand's button while pulling saves the current selection into it
		// instead. (The right hand's A now issues orders, so it cannot double as a modifier.)
		if (rightState.triggerPressed)
			applyControlGroup(panelX, TheOpenXR->getController(VR_HAND_LEFT).primaryButton);

		// Do not let the click fall through to the battlefield underneath.
		if (m_leftDown)
		{
			m_leftDown = FALSE;
			mouse->addWin32Event(WM_LBUTTONUP, 0, MAKELPARAM(0, 0), GetTickCount());
		}
		return;
	}

	if (pick == OpenXRManager::VR_PICK_SCREEN)
	{
		screen.x = panelX;
		screen.y = panelY;
		haveTarget = TRUE;
	}
	else if (!m_menuOpen && view != nullptr && TheGameLogic != nullptr && TheGameLogic->isInGame())
	{
		// Otherwise point at the battlefield. Crucially this hits UNITS AND BUILDINGS, not just
		// the ground: the cursor is placed on the point where the laser actually strikes the
		// object, so when the engine then does its own pick from that pixel it finds the same
		// object. Tracing only the terrain sent the cursor to the dirt behind the tank you were
		// aiming at, which is why units could not be clicked.
		const Bool placing = (TheInGameUI != nullptr && TheInGameUI->getPendingPlaceType() != nullptr);

		Vector3 placeOrigin, placeDir;
		if (placing && computeHandRay(VR_HAND_RIGHT, placeOrigin, placeDir))
		{
			// Placement is world-space and owns the trigger while it lasts; the cursor plays no
			// part in it. Nothing below should fire underneath it.
			updatePlacement(placeOrigin, placeDir);
			return;
		}

		Vector3 origin, dir;
		Coord3D hit;
		const Bool haveRay = computeHandRay(VR_HAND_RIGHT, origin, dir);
		const Bool haveObject = haveRay && !placing && traceScene(origin, dir, hit);
		const Bool haveGround = !haveObject && haveRay && traceTerrain(origin, dir, hit);
		const Bool onScreen = (haveObject || haveGround) && view->worldToScreen(&hit, &screen);

		if (onScreen)
		{
			m_hasAimPoint = TRUE;
			m_aimPoint = hit;
			haveTarget = TRUE;
		}

		// Say exactly where the chain breaks. A click that goes nowhere is otherwise silent:
		// the ray may miss, or it may hit something that the flat camera cannot see - and
		// worldToScreen then refuses, because the cursor we drive lives in the flat view.
		static Int diagCountdown = 0;
		if (--diagCountdown <= 0 || rightState.triggerPressed)
		{
			diagCountdown = 90;
			DEBUG_LOG(("OpenXR: aim: ray=%d object=%d ground=%d onScreen=%d screen=(%d,%d) hit=(%.0f %.0f %.0f)%s",
				haveRay, haveObject, haveGround, onScreen,
				onScreen ? screen.x : -1, onScreen ? screen.y : -1,
				hit.x, hit.y, hit.z,
				rightState.triggerPressed ? " [TRIGGER]" : ""));
		}
	}

	// On the battlefield, selection and orders go through the WORLD, not through a pixel - and
	// this must happen before the cursor bails out below, because the case it is fixing is
	// exactly the one where the cursor cannot be placed at all. The cursor still follows along
	// when it can (it drives the GUI, the hover highlight and building placement), but a click
	// no longer depends on it.
	// With a menu up, a ray that happens to miss the menu screen must NOT reach the battlefield:
	// selection is client-side and works even while the game sits paused, so a stray trigger
	// pull would silently rewrite the player's selection behind the Escape menu.
	if (pick == OpenXRManager::VR_PICK_NONE && !m_menuOpen && view != nullptr
		&& TheGameLogic != nullptr && TheGameLogic->isInGame()
		&& (TheInGameUI == nullptr || TheInGameUI->getPendingPlaceType() == nullptr))
	{
		Vector3 origin, dir;
		if (computeHandRay(VR_HAND_RIGHT, origin, dir))
		{
			// ONE trigger does both jobs, the way one mouse button does: what happens depends on
			// what you are pointing at, not on which button you chose. The decision waits for the
			// RELEASE - a press cannot know yet whether it is the start of a box sweep.
			const Bool wasBoxing = m_boxing;
			updateBoxSelect(origin, dir);

			if (rightState.triggerReleased && !wasBoxing)
			{
				Drawable *underRay = pickDrawable(origin, dir);
				const Bool isOwn = (underRay != nullptr && underRay->getObject() != nullptr
					&& underRay->getObject()->isLocallyControlled() && underRay->isSelectable());

				const DrawableList *selected = (TheInGameUI != nullptr)
					? TheInGameUI->getAllSelectedDrawables() : nullptr;
				const Bool haveSelection = (selected != nullptr && !selected->empty());

				// Does what is selected have something USEFUL to do with what is under the beam?
				// A worker and a half-built barracks means "go and finish it"; a worker and a
				// damaged one means "go and fix it". Selecting the building instead - which is
				// what a plain is-it-mine test does - throws the worker away and answers a
				// question nobody asked. So ask the engine what the pair would do, and if it has
				// an answer, do that; only fall back to selecting when it does not.
				Bool actionable = FALSE;
				if (haveSelection && underRay != nullptr && TheGameClient != nullptr)
				{
					Coord3D spot;
					const Coord3D *pos = traceTerrain(origin, dir, spot) ? &spot
						: underRay->getPosition();
					if (pos != nullptr)
					{
						const GameMessage::Type ctx = TheGameClient->evaluateContextCommand(
							underRay, pos, CommandTranslator::EVALUATE_ONLY);
						actionable = (ctx != GameMessage::MSG_INVALID)
							&& (ctx != GameMessage::MSG_DO_MOVETO);
					}
				}

				if (actionable)
					commandUnderRay(origin, dir);	// finish it, repair it, enter it, attack it
				else if (isOwn)
					selectUnderRay(origin, dir);	// your own unit, and nothing to do with it: take it
				else if (haveSelection)
					commandUnderRay(origin, dir);	// bare ground, or theirs: an order
				else
					selectUnderRay(origin, dir);	// nothing selected: a click on nothing clears
			}

			// A is still an explicit order, for when you want to command without any chance of
			// picking up whatever you happened to be pointing at.
			if (rightState.primaryPressed)
				commandUnderRay(origin, dir);

			// The left trigger drops the selection.
			const VRControllerState &leftState = TheOpenXR->getController(VR_HAND_LEFT);
			if (leftState.triggerPressed && TheInGameUI != nullptr && TheMessageStream != nullptr)
			{
				TheInGameUI->deselectAllDrawables();
				TheMessageStream->appendMessage(GameMessage::MSG_DESTROY_SELECTED_GROUP);
			}
		}
	}

	if (!haveTarget)
		return;	// pointing at nothing; leave the cursor where it is

	const DWORD now = GetTickCount();
	const LPARAM packed = MAKELPARAM(screen.x, screen.y);

	mouse->addWin32Event(WM_MOUSEMOVE, 0, packed, now);
	// Remember where the ray put the cursor so updateMouseActivity can tell this apart from a
	// real mouse move (which lands somewhere else) - the monitor frame is a physical-mouse aid.
	m_injectedCursorX = screen.x;
	m_injectedCursorY = screen.y;

	const VRControllerState &right = TheOpenXR->getController(VR_HAND_RIGHT);
	const VRControllerState &left = TheOpenXR->getController(VR_HAND_LEFT);

	// Synthetic clicks are for the things that genuinely live in screen space: the UI panels,
	// and placing a building (whose ghost follows the cursor). On the open battlefield the click
	// was handled in world space above, and firing a second one here would fight it - a click on
	// bare terrain at a stale cursor would undo the selection we just made.
	const Bool clickThroughCursor = (pick != OpenXRManager::VR_PICK_NONE)
		|| (TheInGameUI != nullptr && TheInGameUI->getPendingPlaceType() != nullptr)
		|| (TheGameLogic == nullptr || !TheGameLogic->isInGame());

	if (!clickThroughCursor)
		return;

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
/** Each hand's secondary button (B / Y) summons that hand's panel and dismisses it again, so
	* the HUD is there when you want it and gone when you are commanding units. */
void VRControls::updatePanelToggles()
{
	// The RIGHT hand's B summons the panel - onto the LEFT hand, where it belongs: you point with
	// the right and read with the left, and a hand cannot aim at the panel it is holding anyway.
	// The left hand's Y is no longer a toggle at all; it is the force-attack modifier.
	if (TheOpenXR->getController(VR_HAND_RIGHT).secondaryPressed)
		TheOpenXR->toggleWristPanel(VR_HAND_LEFT);

	// Click the right stick to jump the view to what you have selected - the headset's spacebar.
	if (TheOpenXR->getController(VR_HAND_RIGHT).stickClickPressed)
		jumpToSelection();
}

//-------------------------------------------------------------------------------------------------
/** Bring the view to the selected units. Across a battlefield you can now see all of, losing your
	* army is easy; this is the spacebar you would have reached for. */
//-------------------------------------------------------------------------------------------------
void VRControls::jumpToSelection()
{
	if (TheInGameUI == nullptr || TheTacticalView == nullptr)
		return;

	const DrawableList *selected = TheInGameUI->getAllSelectedDrawables();
	if (selected == nullptr || selected->empty())
		return;

	// The middle of the group, not whichever one happens to be first.
	Coord3D centre;
	centre.zero();
	Int count = 0;

	for (DrawableList::const_iterator it = selected->begin(); it != selected->end(); ++it)
	{
		Drawable *draw = *it;
		if (draw == nullptr)
			continue;
		const Coord3D *pos = draw->getPosition();
		if (pos == nullptr)
			continue;
		centre.x += pos->x;
		centre.y += pos->y;
		centre.z += pos->z;
		++count;
	}

	if (count == 0)
		return;

	centre.x /= (Real)count;
	centre.y /= (Real)count;
	centre.z /= (Real)count;

	TheTacticalView->lookAt(&centre);
	DEBUG_LOG(("OpenXR: jumped to selection (%d units)", count));
}

//-------------------------------------------------------------------------------------------------
/** Control groups, the RTS player's muscle memory, without a keyboard. Pointing at a slot and
	* pulling the trigger recalls that squad; holding the primary button (A / X) while doing it
	* saves the current selection there instead. These go through the same meta-messages the
	* keyboard hotkeys produce, so the engine cannot tell the difference. */
void VRControls::applyControlGroup(Int slot, Bool assign)
{
	if (TheMessageStream == nullptr || slot < 0 || slot > 9)
		return;

	const GameMessage::Type type = assign
		? (GameMessage::Type)(GameMessage::MSG_META_CREATE_TEAM0 + slot)
		: (GameMessage::Type)(GameMessage::MSG_META_SELECT_TEAM0 + slot);

	TheMessageStream->appendMessage(type);
	DEBUG_LOG(("OpenXR: control group %d %s", slot, assign ? "SAVED" : "recalled"));
}

//-------------------------------------------------------------------------------------------------
/** The visible laser. A pointer you cannot see is a pointer you cannot aim, so each hand casts a
	* beam that stops exactly where it lands - on a panel, or on the ground. */
void VRControls::updateRays(W3DView *view)
{
	const Bool inGame = (TheGameLogic != nullptr && TheGameLogic->isInGame());

	// In a battle the beams live in world units, alongside the battlefield. In the menus there
	// is no battlefield at all - the eye pass draws the rays and nothing else - so they live in
	// plain metres, the same space the menu screen hangs in. (W3DDisplay renders the menu eye
	// pass with an identity anchor and a scale of one, so the two agree.)
	const Real scale = inGame ? TheOpenXR->getWorldUnitsPerMeter() : 1.0f;

	for (Int hand = 0; hand < 2; ++hand)
	{
		Line3DClass *line = m_rayLines[hand];
		if (line == nullptr)
			continue;

		// GeneralsVR @feature Only the right hand points and clicks; the left hand holds the HUD and
		// never casts a usable beam, so its laser was just visual noise. Keep it permanently hidden.
		if (hand == VR_HAND_LEFT)
		{
			line->Set_Hidden(true);
			m_rayVisible[hand] = FALSE;
			continue;
		}

		const VRControllerState &c = TheOpenXR->getController(hand);

		// The hand holding the menu does not also carry a laser: the beam starts inside the panel
		// it is holding and lies across everything you are trying to read.
		if (!c.poseValid || TheOpenXR->isWristPanelOpen(hand))
		{
			line->Set_Hidden(true);
			m_rayVisible[hand] = FALSE;
			continue;
		}

		Vector3 origin, dir;
		if (inGame)
		{
			if (!computeHandRay(hand, origin, dir))
			{
				line->Set_Hidden(true);
				m_rayVisible[hand] = FALSE;
				continue;
			}
		}
		else
		{
			// Straight from the controller pose, in metres.
			Quaternion q(c.quatX, c.quatY, c.quatZ, c.quatW);
			Matrix3D handPose;
			Build_Matrix3D(q, handPose);
			origin = Vector3(c.posX, c.posY, c.posZ);
			dir = -handPose.Get_Z_Vector();	// controllers point down their -Z
			dir.Normalize();
		}

		// Stop the beam where it actually lands: on a panel if one is in the way, otherwise on
		// the ground, otherwise just fade out at arm's reach.
		Real length = RAY_MAX_METERS * scale;
		Bool onPanel = FALSE;

		Int px = 0, py = 0;
		Real panelDistance = 0.0f;
		if (TheOpenXR->pickUiPanel(hand, px, py, &panelDistance) != OpenXRManager::VR_PICK_NONE)
		{
			// Stop a hair short. Ending the beam exactly ON the panel's surface leaves the two
			// coplanar, and the depth test throws the beam away - which reads as a laser that
			// cannot touch the menu at all.
			length = panelDistance * scale * 0.97f;
			onPanel = TRUE;
		}
		else if (inGame)
		{
			// Stop on whatever it strikes - a tank, a building, or the ground - so the beam
			// visibly touches the thing you are about to click.
			Coord3D hit;
			if (traceAim(origin, dir, hit))
			{
				const Vector3 hitVec(hit.x, hit.y, hit.z);
				length = (hitVec - origin).Length();
			}
		}

		const Vector3 end = origin + dir * length;
		line->Reset(origin, end, RAY_WIDTH_METERS * scale);

		// The beam IS the reticle: it takes the colour of whatever the game would do if the
		// player pressed the button now. Only the pointing hand - the other stays its own colour.
		// Not while it rests on a panel, though: there the click is a menu click, and a beam
		// glowing attack-red because an enemy happens to stand BEHIND the options screen is a lie.
		if (inGame && hand == VR_HAND_RIGHT)
		{
			if (onPanel)
			{
				line->Re_Color(0.35f, 0.85f, 1.00f);	// the pointer's own cyan, as in the menus
			}
			else
			{
				Real r, g, b;
				getReticleColor(origin, dir, r, g, b);
				line->Re_Color(r, g, b);
			}
		}

		line->Set_Hidden(false);
		m_rayVisible[hand] = TRUE;
	}
}

//-------------------------------------------------------------------------------------------------
/** Draw the ten control-group slots. Deliberately plain: numbered plates, lit up where the ray
	* is resting, so the player can see which squad they are about to recall. */
//-------------------------------------------------------------------------------------------------
void VRControls::drawGroupBar()
{
	if (TheOpenXR == nullptr || !TheOpenXR->hasGroupBar() || TheDisplay == nullptr)
		return;

	IDirect3DSurface8 *surface = TheOpenXR->getGroupBarSurface();
	if (surface == nullptr)
		return;

	const Int barW = TheOpenXR->getGroupBarWidth();
	const Int barH = TheOpenXR->getGroupBarHeight();

	// Which slot is the ray resting on? That one gets highlighted.
	Int hoverSlot = -1;
	for (Int hand = 0; hand < 2 && hoverSlot < 0; ++hand)
	{
		Int slot = 0, unusedY = 0;
		if (TheOpenXR->pickUiPanel(hand, slot, unusedY) == OpenXRManager::VR_PICK_GROUP_SLOT)
			hoverSlot = slot;
	}
	const Bool assigning = TheOpenXR->getController(VR_HAND_LEFT).primaryButton;

	DX8Wrapper::Set_Render_Target(surface, TheOpenXR->getDepthSurface());

	if (WW3D::Begin_Render(true, false, Vector3(0.0f, 0.0f, 0.0f)) == WW3D_ERROR_OK)
	{
		const Int slotW = barW / 10;
		const Int pad = 6;

		for (Int slot = 0; slot < 10; ++slot)
		{
			const Int x0 = slot * slotW + pad;
			const Int y0 = pad;
			const Int w = slotW - pad * 2;
			const Int h = barH - pad * 2;

			// Amber while the primary button is held (you are about to SAVE into this slot),
			// otherwise a cool highlight for the slot under the ray.
			UnsignedInt fill;
			if (slot == hoverSlot)
				fill = assigning ? GameMakeColor(230, 150, 40, 235) : GameMakeColor(60, 150, 210, 225);
			else
				fill = GameMakeColor(20, 28, 38, 190);

			TheDisplay->drawFillRect(x0, y0, w, h, fill);
			TheDisplay->drawOpenRect(x0, y0, w, h, 2.0f, GameMakeColor(200, 220, 240, 220));

			// The slot's number, drawn as a bar of pips: the font system is not available on an
			// arbitrary render target, but a count is unmistakable at a glance.
			const Int label = (slot + 1) % 10;	// 1..9 then 0
			const Int pipW = 6;
			const Int pipH = 6;
			const Int pips = (label == 0) ? 10 : label;
			const Int pipsPerRow = 5;
			const Int rows = (pips + pipsPerRow - 1) / pipsPerRow;
			const Int blockH = rows * (pipH + 3);
			const Int startY = y0 + (h - blockH) / 2;

			for (Int p = 0; p < pips; ++p)
			{
				const Int row = p / pipsPerRow;
				const Int col = p % pipsPerRow;
				const Int inRow = (row == rows - 1) ? (pips - row * pipsPerRow)
					: pipsPerRow;
				const Int rowW = inRow * (pipW + 3) - 3;
				const Int px = x0 + (w - rowW) / 2 + col * (pipW + 3);
				const Int py = startY + row * (pipH + 3);
				TheDisplay->drawFillRect(px, py, pipW, pipH, GameMakeColor(255, 255, 255, 240));
			}
		}

		WW3D::End_Render(false);
	}

	DX8Wrapper::Set_Render_Target((IDirect3DSurface8 *)nullptr);
}

//-------------------------------------------------------------------------------------------------
void VRControls::update()
{
	if (TheOpenXR == nullptr || !TheOpenXR->isFrameActive())
		return;

	W3DView *view = (W3DView *)TheTacticalView;
	const Bool inGame = (TheGameLogic != nullptr && TheGameLogic->isInGame());

	// Entering or leaving a match warps and re-captures the cursor, sometimes over several
	// frames of loading. None of that is the player: whoever was genuinely driving on the
	// previous screen keeps control, and for a grace window the mouse cannot steal it.
	if (inGame != m_prevInGame)
	{
		m_prevInGame = inGame;
		const UnsignedInt now = GetTickCount();
		const Bool mouseWasDriving = (m_mouseLastMoveTime != 0) && ((now - m_mouseLastMoveTime) < 2000);
		m_mouseKbMode = mouseWasDriving;
		m_mouseSeeded = FALSE;
		m_mouseLastMoveTime = 0;
		m_modeGraceUntil = now + 1500;
	}

	// Watch the mouse first, so both the menu cursor and the in-game frame have a fresh answer.
	updateMouseActivity();
	// Then decide which input is driving - rays or mouse+keyboard - and switch softly between them.
	updateInputMode();

	// Keyboard shortcuts that work EVERYWHERE, from the title screen on - not gated by game state.
	if (TheKeyboard != nullptr && TheOpenXR != nullptr)
	{
		// Ctrl+Space recenters the VR view (the mouse+keyboard equivalent of the rays' reset).
		const Bool recenterCombo = TheKeyboard->isCtrl() && TheKeyboard->isKeyDown(KEY_SPACE);
		if (recenterCombo && !m_ctrlSpaceWasDown)
			TheOpenXR->recenter();
		m_ctrlSpaceWasDown = recenterCombo;

		// Ctrl+O pulls the whole battlefield CLOSER, Ctrl+L pushes it FURTHER, by scaling the VR
		// world size (world-units-per-metre). The tactical camera and its render limits do not move
		// at all - only how big the map feels around you. Held to ease in; clamped to a sane range.
		if (TheKeyboard->isCtrl())
		{
			Real s = TheOpenXR->getWorldUnitsPerMeter();
			if (TheKeyboard->isKeyDown(KEY_O)) s *= 0.99f;	// fewer units/metre -> map bigger -> closer
			if (TheKeyboard->isKeyDown(KEY_L)) s *= 1.01f;	// further
			if (s < 40.0f) s = 40.0f;
			if (s > 4000.0f) s = 4000.0f;
			TheOpenXR->setWorldUnitsPerMeter(s);
		}
	}

	// In mouse+keyboard mode there is no controller to summon the HUD, so ask for the fixed in-game
	// HUD panel and measure where it belongs (matched to the monitor frame) and how opaque.
	updateFixedHud(view, inGame);

	// Skipping the intro. The movie does not run in a blocking loop at all - it plays across
	// ordinary frames, and Escape reaches it through the window translator, which then calls
	// stopMovie(). So we call the same thing: no keyboard needed in a headset.
	if (TheDisplay != nullptr && TheDisplay->isMoviePlaying())
	{
		// No lasers across the intro. There is nothing to point at, and leaving them lit means
		// two beams hanging over the film - this path returns early, so they must be put away
		// here or they simply keep whatever state they were last left in.
		for (Int hand = 0; hand < 2; ++hand)
		{
			if (m_rayLines[hand] != nullptr)
				m_rayLines[hand]->Set_Hidden(true);
			m_rayVisible[hand] = FALSE;

			if (TheOpenXR->getController(hand).secondaryPressed)
			{
				TheDisplay->stopMovie();
				DEBUG_LOG(("OpenXR: movie skipped by controller"));
			}
		}
		updateMonitorFrame(FALSE);	// no battlefield behind the film - put the frame away
		updateMouseMarker(FALSE);
		return;	// nothing else to do while a movie is on screen
	}

	// Mouse + keyboard mode: the motion controllers are put away entirely - no rays, no ray pointer,
	// no grab-locomotion. The physical mouse and keyboard drive the game through its ordinary input,
	// exactly as on the monitor; we only add the VR-side visuals (cursor, frame, drag box) and
	// edge-scrolling. (A future settings menu flips m_mouseKbMode; for now it is forced on.)
	if (m_mouseKbMode)
	{
		for (Int hand = 0; hand < 2; ++hand)
		{
			if (m_rayLines[hand] != nullptr)
				m_rayLines[hand]->Set_Hidden(true);
			m_rayVisible[hand] = FALSE;
		}
		if (TheWritableGlobalData != nullptr)
			TheWritableGlobalData->m_vrAimValid = FALSE;	// no ray aim; placement follows the real mouse

		updateMouseScroll(inGame ? view : nullptr);
		updateMouseBoxSelect(inGame ? view : nullptr);
		updateSelectionMarkers();
		updateUiCrop();
		// Show the frame and crosshair whenever mouse+keyboard is the input (always, in this mode).
		// The crosshair is now the ONE cursor: it slides onto the HUD and menu panels as well as the
		// battlefield (mousePanelWorldPos), so it is NO LONGER hidden over the HUD.
		updateMonitorFrame(inGame && view != nullptr);
		updateMouseMarker(inGame && view != nullptr);
		return;
	}

	updatePanelToggles();

	// Locomotion only means something when there is a battlefield to move over. In the menus the
	// controllers must not fling the tactical camera around behind the player's back - and that
	// includes the IN-GAME menus: with the Escape menu up, the sticks and grips belong to the
	// menu screen, not to the paused world behind it.
	if (inGame && view != nullptr && !m_menuOpen)
		updateLocomotion(view);

	// Pointing works everywhere: at the menu screen floating in front of the player, at the
	// wrist panels, and at the battlefield.
	updatePointer(inGame ? view : nullptr);

	// Publish where the laser meets the ground. The engine's building placement reads this: its
	// ghost used to be derived from the mouse cursor, which only exists where the flat camera can
	// see, and so the building would never go where the player was pointing.
	// While a building is being TURNED it stays pinned where it was put down, so the aim point
	// must not be republished - doing so dragged the building along behind the beam, which is
	// exactly what the pin was for.
	if (TheWritableGlobalData != nullptr && !m_placeTurning)
	{
		Vector3 origin, dir;
		Coord3D ground;
		if (inGame && computeHandRay(VR_HAND_RIGHT, origin, dir)
			&& traceTerrain(origin, dir, ground))
		{
			TheWritableGlobalData->m_vrAimValid = TRUE;
			TheWritableGlobalData->m_vrAimPoint = ground;
		}
		else
		{
			TheWritableGlobalData->m_vrAimValid = FALSE;
		}
	}

	updateUiCrop();

	// The beams are drawn everywhere, including the menus - that is the whole point of them.
	updateRays(view);
	updateSelectionMarkers();

	// This is the RAYS path (m_mouseKbMode is false here), so the mouse+keyboard aids belong hidden -
	// the moment the mouse moves, update() takes the other branch and shows them.
	updateMonitorFrame(FALSE);
	updateMouseMarker(FALSE);
}

//-------------------------------------------------------------------------------------------------
/** How much of the screen the VR panel needs to show.
	*
	* The control bar along the bottom IS the in-game interface, so that strip is all the panel
	* normally carries - showing the whole frame would hang a monitor-sized black slab in the air.
	* But a full-screen window (the Generals promotion screen) opens ABOVE that strip, and cropping
	* it away showed the player half a menu. So we ask the window system where its visible windows
	* actually are, and let the panel grow just far enough to hold them.
	*/
//-------------------------------------------------------------------------------------------------
void VRControls::updateUiCrop()
{
	if (TheWritableGlobalData == nullptr || TheDisplay == nullptr)
		return;

	const Real BOTTOM_STRIP = 0.66f;	// where the control bar begins
	Real top = BOTTOM_STRIP;
	Bool menuOpen = FALSE;

	if (TheWindowManager != nullptr)
	{
		const Real screenW = (Real)TheDisplay->getWidth();
		const Real screenH = (Real)TheDisplay->getHeight();

		for (GameWindow *win = TheWindowManager->winGetWindowList(); win != nullptr;
			win = win->winGetNext())
		{
			if (win->winIsHidden())
				continue;

			Int x = 0, y = 0;
			win->winGetPosition(&x, &y);

			Int ww = 0, wh = 0;
			win->winGetSize(&ww, &wh);

			// A "visible" window parked outside the screen is the window system's way of putting
			// it away; counting one would raise the menu screen during ordinary play, forever.
			if (x + ww <= 0 || y + wh <= 0
				|| (screenW > 0.0f && (Real)x >= screenW)
				|| (screenH > 0.0f && (Real)y >= screenH))
				continue;

			if (y < 0)
				y = 0;

			const Real winTop = (Real)y / screenH;
			if (winTop < top)
				top = winTop;

			// A window reaching into the upper half of the screen is a menu or a dialog - the
			// Escape menu, options, a quit-confirm box, the promotion screen. The control bar and
			// its pieces all live on the bottom strip and never come up here. (The old test asked
			// for a near-full-screen window, which missed every small dialog: a quit-confirm box
			// opened invisibly, pausing the game behind nothing.)
			if (screenW > 0.0f && screenH > 0.0f
				&& winTop < 0.5f
				&& (Real)wh / screenH > 0.10f && (Real)ww / screenW > 0.15f)
				menuOpen = TRUE;
		}
	}

	if (top < 0.0f) top = 0.0f;
	if (top > BOTTOM_STRIP) top = BOTTOM_STRIP;

	TheWritableGlobalData->m_vrUiCropTop = top;

	if (menuOpen != m_menuOpen)
		DEBUG_LOG(("OpenXR: ui: in-game menu %s", menuOpen ? "opened" : "closed"));
	m_menuOpen = menuOpen;
	// Both modes read this when laying out the panels: mouse+keyboard stands the HUD upright,
	// rays raises the big menu screen.
	if (TheOpenXR != nullptr)
		TheOpenXR->setUiMenuOpen(menuOpen);
}
