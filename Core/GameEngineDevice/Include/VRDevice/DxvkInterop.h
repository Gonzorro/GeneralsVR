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

// FILE: DxvkInterop.h ///////////////////////////////////////////////////////////////////////////
// GeneralsVR @feature Minimal vendored declarations of DXVK's D3D9 Vulkan interop COM
// interfaces (dxvk/src/d3d9/d3d9_interfaces.h, zlib licensed), so the engine can reach the
// Vulkan device DXVK created for the game when running under DXVK's d3d8.dll. Interface
// GUIDs and vtable layouts must match the deployed DXVK binary (verified against v3.0.1).
///////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <unknwn.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

struct D3D9VkExtImageDesc;
struct IDirect3DResource9;

// {d56344f5-8d35-46fd-806d-94c351b472c1}
static const GUID IID_ID3D9VkInteropTexture =
	{ 0xd56344f5, 0x8d35, 0x46fd, { 0x80, 0x6d, 0x94, 0xc3, 0x51, 0xb4, 0x72, 0xc1 } };

struct ID3D9VkInteropTexture : public IUnknown
{
	virtual HRESULT STDMETHODCALLTYPE GetVulkanImageInfo(
		VkImage*              pHandle,
		VkImageLayout*        pLayout,
		VkImageCreateInfo*    pInfo) = 0;
};

// {2eaa4b89-0107-4bdb-87f7-0f541c493ce0}
static const GUID IID_ID3D9VkInteropDevice =
	{ 0x2eaa4b89, 0x0107, 0x4bdb, { 0x87, 0xf7, 0x0f, 0x54, 0x1c, 0x49, 0x3c, 0xe0 } };

struct ID3D9VkInteropDevice : public IUnknown
{
	virtual void STDMETHODCALLTYPE GetVulkanHandles(
		VkInstance*           pInstance,
		VkPhysicalDevice*     pPhysDev,
		VkDevice*             pDevice) = 0;

	virtual void STDMETHODCALLTYPE GetSubmissionQueue(
		VkQueue*              pQueue,
		uint32_t*             pQueueIndex,
		uint32_t*             pQueueFamilyIndex) = 0;

	virtual void STDMETHODCALLTYPE TransitionTextureLayout(
		ID3D9VkInteropTexture*    pTexture,
		const VkImageSubresourceRange* pSubresources,
		VkImageLayout             OldLayout,
		VkImageLayout             NewLayout) = 0;

	virtual void STDMETHODCALLTYPE FlushRenderingCommands() = 0;

	virtual void STDMETHODCALLTYPE LockSubmissionQueue() = 0;

	virtual void STDMETHODCALLTYPE ReleaseSubmissionQueue() = 0;

	virtual void STDMETHODCALLTYPE LockDevice() = 0;

	virtual void STDMETHODCALLTYPE UnlockDevice() = 0;

	virtual bool STDMETHODCALLTYPE WaitForResource(
		IDirect3DResource9*   pResource,
		DWORD                 MapFlags) = 0;

	virtual HRESULT STDMETHODCALLTYPE CreateImage(
		const D3D9VkExtImageDesc* desc,
		IDirect3DResource9**      ppResult) = 0;
};
