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
// GeneralsVR @feature OpenXR session lifecycle for the VR port. Phase 1 scope: instance and
// system bootstrap with graceful fallback to flat rendering when no runtime or headset is
// available. Swapchains and the frame loop arrive with the stereo renderer.
///////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Lib/BaseType.h"

#include <openxr/openxr.h>

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

	XrInstance getInstance() const { return m_instance; }
	XrSystemId getSystemId() const { return m_systemId; }

	/// Recommended per-eye render target size reported by the runtime.
	Int getEyeWidth() const { return m_eyeWidth; }
	Int getEyeHeight() const { return m_eyeHeight; }

private:
	Bool hasExtension(const char* name) const;

	XrInstance m_instance;
	XrSystemId m_systemId;
	Int m_eyeWidth;
	Int m_eyeHeight;
	Bool m_supportsVulkan;
	Bool m_supportsD3D11;
};

extern OpenXRManager* TheOpenXR; ///< nullptr unless the game was launched with -vr
