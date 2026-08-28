#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define BRIDGE_D3D9_PLUGIN_IMPLEMENTATION
#include "..\BridgeD3D9Plugin.h"

#include <cstdio>
#include <cstring>

struct ProbePassState
{
    char id;
    bool failFirstCall;
    volatile LONG calls;
};

static BridgeD3D9PluginApi2 g_api{};
static UINT64 g_tokens[4]{};
static HANDLE g_output = INVALID_HANDLE_VALUE;
static ProbePassState g_passes[] = {
    { 'A', false, 0 },
    { 'B', false, 0 },
    { 'C', false, 0 },
    { 'F', true,  0 },
};

static HRESULT STDMETHODCALLTYPE RecordProbePass(
    void* userData,
    const D3D9GtaSaVulkanFrameContext* frame)
{
    auto* pass = static_cast<ProbePassState*>(userData);
    if (!pass || !frame ||
        frame->StructSize < sizeof(D3D9GtaSaVulkanFrameContext) ||
        frame->ApiVersion != D3D9_GTA_SA_COMPAT_API_VERSION ||
        frame->CommandBuffer == VK_NULL_HANDLE ||
        frame->OutputImage == VK_NULL_HANDLE) {
        return E_INVALIDARG;
    }

    LONG call = InterlockedIncrement(&pass->calls);
    if (g_output != INVALID_HANDLE_VALUE) {
        char line[64]{};
        int length = std::snprintf(line, sizeof(line), "%c frame=%llu call=%ld\r\n",
            pass->id,
            static_cast<unsigned long long>(frame->FrameId),
            call);
        if (length > 0) {
            DWORD written = 0;
            WriteFile(g_output, line, static_cast<DWORD>(length), &written, nullptr);
            FlushFileBuffers(g_output);
        }
    }

    return pass->failFirstCall && call == 1 ? E_FAIL : D3D_OK;
}

extern "C" __declspec(dllexport) BOOL __stdcall BridgeD3D9_PluginInit2(
    const BridgeD3D9PluginApi2* api)
{
    if (!api || api->apiVersion < BRIDGE_D3D9_PLUGIN_API_VERSION_2 ||
        api->structSize < sizeof(BridgeD3D9PluginApi2) ||
        !api->RegisterVulkanPass || !api->UnregisterVulkanPass) {
        return FALSE;
    }

    g_api = *api;
    g_output = CreateFileA("bridge_vulkan_pass_probe.log", GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);

    const INT priorities[] = { 200, 100, 200, 150 };
    for (UINT i = 0; i < ARRAYSIZE(g_passes); ++i) {
        D3D9GtaSaVulkanPassDesc desc{};
        desc.StructSize = sizeof(desc);
        desc.ApiVersion = D3D9_GTA_SA_COMPAT_API_VERSION;
        desc.Priority = priorities[i];
        desc.Stage = D3D9_GTA_SA_VULKAN_PASS_AFTER_BLIT;
        desc.Flags = D3D9_GTA_SA_VULKAN_PASS_RESTORES_LAYOUTS;
        std::snprintf(desc.Name, sizeof(desc.Name), "BridgeProbe%c", g_passes[i].id);
        desc.Record = &RecordProbePass;
        desc.UserData = &g_passes[i];

        if (FAILED(g_api.RegisterVulkanPass(g_api.hostContext, &desc, &g_tokens[i]))) {
            return FALSE;
        }
    }

    return TRUE;
}

extern "C" __declspec(dllexport) void __stdcall BridgeD3D9_PluginShutdown()
{
    if (g_api.UnregisterVulkanPass) {
        for (UINT i = 0; i < ARRAYSIZE(g_tokens); ++i) {
            if (g_tokens[i]) {
                g_api.UnregisterVulkanPass(g_api.hostContext, g_tokens[i]);
                g_tokens[i] = 0;
            }
        }
    }

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
