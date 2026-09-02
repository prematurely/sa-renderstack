#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define BRIDGE_D3D9_PLUGIN_IMPLEMENTATION
#include "..\BridgeD3D9Plugin.h"

#include <cstdio>
#include <format>
#include <string>

static HANDLE g_output = INVALID_HANDLE_VALUE;

static void WriteEvent(const char* eventName)
{
    if (g_output == INVALID_HANDLE_VALUE) return;
    const std::string line = std::format("{}\r\n", eventName);
    DWORD written = 0;
    WriteFile(g_output, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    FlushFileBuffers(g_output);
}

extern "C" __declspec(dllexport) BOOL __stdcall BridgeD3D9_PluginInit(
    const BridgeD3D9PluginApi* api)
{
    if (!api || api->apiVersion != BRIDGE_D3D9_PLUGIN_API_VERSION_1 || !api->Log) {
        return FALSE;
    }
    g_output = CreateFileA("bridge_legacy_plugin_probe.log", GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    WriteEvent("init-v1");
    return TRUE;
}

extern "C" __declspec(dllexport) void __stdcall BridgeD3D9_OnCreateDevice(
    IDirect3DDevice9*, D3DPRESENT_PARAMETERS*)
{
    WriteEvent("create-device");
}

extern "C" __declspec(dllexport) void __stdcall BridgeD3D9_OnResetBefore(
    IDirect3DDevice9*, D3DPRESENT_PARAMETERS*)
{
    WriteEvent("reset-before");
}

extern "C" __declspec(dllexport) void __stdcall BridgeD3D9_OnResetAfter(
    IDirect3DDevice9*, HRESULT result, D3DPRESENT_PARAMETERS*)
{
    WriteEvent(SUCCEEDED(result) ? "reset-after-ok" : "reset-after-failed");
}

extern "C" __declspec(dllexport) void __stdcall BridgeD3D9_OnReleaseDevice(
    IDirect3DDevice9*)
{
    WriteEvent("release-device");
}

extern "C" __declspec(dllexport) void __stdcall BridgeD3D9_PluginShutdown()
{
    WriteEvent("shutdown");
    if (g_output != INVALID_HANDLE_VALUE) {
        CloseHandle(g_output);
        g_output = INVALID_HANDLE_VALUE;
    }
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_DETACH && g_output != INVALID_HANDLE_VALUE) {
        CloseHandle(g_output);
        g_output = INVALID_HANDLE_VALUE;
    }
    return TRUE;
}

