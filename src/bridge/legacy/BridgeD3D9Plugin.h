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

typedef BOOL (__stdcall *PFN_BridgeD3D9_PluginInit)(const BridgeD3D9PluginApi* api);
typedef BOOL (__stdcall *PFN_BridgeD3D9_PluginInit2)(const BridgeD3D9PluginApi2* api);
typedef void (__stdcall *PFN_BridgeD3D9_PluginShutdown)();
typedef void (__stdcall *PFN_BridgeD3D9_OnCreateDevice)(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params);
typedef void (__stdcall *PFN_BridgeD3D9_OnResetBefore)(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params);
typedef void (__stdcall *PFN_BridgeD3D9_OnResetAfter)(IDirect3DDevice9* device, HRESULT result, D3DPRESENT_PARAMETERS* params);
typedef void (__stdcall *PFN_BridgeD3D9_OnEndScene)(IDirect3DDevice9* device);
typedef void (__stdcall *PFN_BridgeD3D9_OnPresentBefore)(IDirect3DDevice9* device, const RECT* src, const RECT* dst, HWND hwnd, const RGNDATA* dirty);
typedef void (__stdcall *PFN_BridgeD3D9_OnPresentAfter)(IDirect3DDevice9* device, HRESULT result);
typedef void (__stdcall *PFN_BridgeD3D9_OnReleaseDevice)(IDirect3DDevice9* device);

// Legacy host-side aliases. Plugin implementation files define
// BRIDGE_D3D9_PLUGIN_IMPLEMENTATION so exported function names remain usable.
#ifndef BRIDGE_D3D9_PLUGIN_IMPLEMENTATION
typedef PFN_BridgeD3D9_PluginInit BridgeD3D9_PluginInit;
typedef PFN_BridgeD3D9_PluginInit2 BridgeD3D9_PluginInit2;
typedef PFN_BridgeD3D9_PluginShutdown BridgeD3D9_PluginShutdown;
typedef PFN_BridgeD3D9_OnCreateDevice BridgeD3D9_OnCreateDevice;
typedef PFN_BridgeD3D9_OnResetBefore BridgeD3D9_OnResetBefore;
typedef PFN_BridgeD3D9_OnResetAfter BridgeD3D9_OnResetAfter;
typedef PFN_BridgeD3D9_OnEndScene BridgeD3D9_OnEndScene;
typedef PFN_BridgeD3D9_OnPresentBefore BridgeD3D9_OnPresentBefore;
typedef PFN_BridgeD3D9_OnPresentAfter BridgeD3D9_OnPresentAfter;
typedef PFN_BridgeD3D9_OnReleaseDevice BridgeD3D9_OnReleaseDevice;
#endif
