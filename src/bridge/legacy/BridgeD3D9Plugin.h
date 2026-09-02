#pragma once

#include <windows.h>
#include <d3d9.h>
#include "d3d9_gta_sa_api.h"

#define BRIDGE_D3D9_PLUGIN_API_VERSION_1 1u
#define BRIDGE_D3D9_PLUGIN_API_VERSION_2 2u
#define BRIDGE_D3D9_PLUGIN_API_VERSION BRIDGE_D3D9_PLUGIN_API_VERSION_2

struct BridgeD3D9PluginApi
{
    UINT apiVersion;
    void (__stdcall *Log)(const char* message);
};

struct BridgeD3D9PluginApi2
{
    UINT apiVersion;
    UINT structSize;
    void (__stdcall *Log)(const char* message);
    void* hostContext;
    HRESULT (__stdcall *GetVulkanStatus)(
        void* hostContext,
        D3D9GtaSaCompatStatus* status);
    HRESULT (__stdcall *RegisterVulkanPass)(
        void* hostContext,
        const D3D9GtaSaVulkanPassDesc* desc,
        UINT64* token);
    HRESULT (__stdcall *UnregisterVulkanPass)(
        void* hostContext,
        UINT64 token);
};

using PFN_BridgeD3D9_PluginInit = BOOL (__stdcall *)(const BridgeD3D9PluginApi* api);
using PFN_BridgeD3D9_PluginInit2 = BOOL (__stdcall *)(const BridgeD3D9PluginApi2* api);
using PFN_BridgeD3D9_PluginShutdown = void (__stdcall *)();
using PFN_BridgeD3D9_OnCreateDevice = void (__stdcall *)(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params);
using PFN_BridgeD3D9_OnResetBefore = void (__stdcall *)(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params);
using PFN_BridgeD3D9_OnResetAfter = void (__stdcall *)(IDirect3DDevice9* device, HRESULT result, D3DPRESENT_PARAMETERS* params);
using PFN_BridgeD3D9_OnEndScene = void (__stdcall *)(IDirect3DDevice9* device);
using PFN_BridgeD3D9_OnPresentBefore = void (__stdcall *)(IDirect3DDevice9* device, const RECT* src, const RECT* dst, HWND hwnd, const RGNDATA* dirty);
using PFN_BridgeD3D9_OnPresentAfter = void (__stdcall *)(IDirect3DDevice9* device, HRESULT result);
using PFN_BridgeD3D9_OnReleaseDevice = void (__stdcall *)(IDirect3DDevice9* device);

// Legacy host-side aliases. Plugin implementation files define
// BRIDGE_D3D9_PLUGIN_IMPLEMENTATION so exported function names remain usable.
#ifndef BRIDGE_D3D9_PLUGIN_IMPLEMENTATION
using BridgeD3D9_PluginInit = PFN_BridgeD3D9_PluginInit;
using BridgeD3D9_PluginInit2 = PFN_BridgeD3D9_PluginInit2;
using BridgeD3D9_PluginShutdown = PFN_BridgeD3D9_PluginShutdown;
using BridgeD3D9_OnCreateDevice = PFN_BridgeD3D9_OnCreateDevice;
using BridgeD3D9_OnResetBefore = PFN_BridgeD3D9_OnResetBefore;
using BridgeD3D9_OnResetAfter = PFN_BridgeD3D9_OnResetAfter;
using BridgeD3D9_OnEndScene = PFN_BridgeD3D9_OnEndScene;
using BridgeD3D9_OnPresentBefore = PFN_BridgeD3D9_OnPresentBefore;
using BridgeD3D9_OnPresentAfter = PFN_BridgeD3D9_OnPresentAfter;
using BridgeD3D9_OnReleaseDevice = PFN_BridgeD3D9_OnReleaseDevice;
#endif
