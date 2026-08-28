#include <windows.h>
#include <d3d9.h>

#include "../d3d9_gta_sa_api.h"
#include "../GtaSaCompatApiVersions.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

using Direct3DCreate9Proc = IDirect3D9* (WINAPI*)(UINT);

namespace
{
constexpr IID kCompatIids[] = {
    {0x9f89b542, 0x4f50, 0x4e7d, {0xb2, 0xa4, 0xe8, 0xea, 0xb3, 0xc7, 0xd9, 0xf1}},
    {0x9f89b542, 0x4f50, 0x4e7d, {0xb2, 0xa4, 0xe8, 0xea, 0xb3, 0xc7, 0xd9, 0xf2}},
    {0x9f89b542, 0x4f50, 0x4e7d, {0xb2, 0xa4, 0xe8, 0xea, 0xb3, 0xc7, 0xd9, 0xf3}},
    {0x9f89b542, 0x4f50, 0x4e7d, {0xb2, 0xa4, 0xe8, 0xea, 0xb3, 0xc7, 0xd9, 0xf4}},
    {0x9f89b542, 0x4f50, 0x4e7d, {0xb2, 0xa4, 0xe8, 0xea, 0xb3, 0xc7, 0xd9, 0xf5}},
    {0x9f89b542, 0x4f50, 0x4e7d, {0xb2, 0xa4, 0xe8, 0xea, 0xb3, 0xc7, 0xd9, 0xf6}},
    {0x9f89b542, 0x4f50, 0x4e7d, {0xb2, 0xa4, 0xe8, 0xea, 0xb3, 0xc7, 0xd9, 0xf7}},
};

LRESULT CALLBACK SmokeWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcA(window, message, wparam, lparam);
}

bool NearlyEqual(float left, float right)
{
    return std::fabs(left - right) < 0.0001f;
}
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: GtaSaCompatApi3Smoke.exe <d3d9.dll>\n");
        return 2;
    }

    SetEnvironmentVariableA(
        "DXVK_CONFIG",
        "d3d9.gtaSaCompat=True;d3d9.gtaSaCompatDiagnostics=True;d3d9.presentInterval=0");

    HMODULE module = LoadLibraryA(argv[1]);
    if (!module) {
        std::fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError());
        return 3;
    }

    auto create9 = reinterpret_cast<Direct3DCreate9Proc>(
        GetProcAddress(module, "Direct3DCreate9"));
    if (!create9) {
        std::fprintf(stderr, "Direct3DCreate9 export missing\n");
        FreeLibrary(module);
        return 4;
    }

    WNDCLASSA windowClass{};
    windowClass.lpfnWndProc = SmokeWindowProc;
    windowClass.hInstance = GetModuleHandleA(nullptr);
    windowClass.lpszClassName = "BridgeD3D9Api3Smoke";
    RegisterClassA(&windowClass);

    HWND window = CreateWindowExA(
        0, windowClass.lpszClassName, "BridgeD3D9 API v3 smoke",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 320, 240,
        nullptr, nullptr, windowClass.hInstance, nullptr);
    if (!window) {
        std::fprintf(stderr, "CreateWindow failed: %lu\n", GetLastError());
        FreeLibrary(module);
        return 5;
    }

    IDirect3D9* d3d9 = create9(D3D_SDK_VERSION);
    if (!d3d9) {
        std::fprintf(stderr, "Direct3DCreate9 returned null\n");
        DestroyWindow(window);
        FreeLibrary(module);
        return 6;
    }

    D3DPRESENT_PARAMETERS params{};
    params.BackBufferWidth = 320;
    params.BackBufferHeight = 240;
    params.BackBufferFormat = D3DFMT_A8R8G8B8;
    params.BackBufferCount = 1;
    params.SwapEffect = D3DSWAPEFFECT_DISCARD;
    params.hDeviceWindow = window;
    params.Windowed = TRUE;
    params.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    IDirect3DDevice9* device = nullptr;
    HRESULT result = d3d9->CreateDevice(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
        D3DCREATE_HARDWARE_VERTEXPROCESSING, &params, &device);
    if (FAILED(result)) {
        std::fprintf(stderr, "CreateDevice failed: 0x%08lX\n", ULONG(result));
        d3d9->Release();
        DestroyWindow(window);
        FreeLibrary(module);
        return 7;
    }

    ID3D9GtaSaCompatDevice2* compat = nullptr;
    result = device->QueryInterface(
        __uuidof(ID3D9GtaSaCompatDevice2), reinterpret_cast<void**>(&compat));
    if (FAILED(result) || !compat) {
        std::fprintf(stderr, "API v3 interface unavailable: 0x%08lX\n", ULONG(result));
        device->Release();
        d3d9->Release();
        DestroyWindow(window);
        FreeLibrary(module);
        return 8;
    }

    D3D9GtaSaCompatStatus status{};
    status.StructSize = sizeof(status);
    result = compat->GetStatus(&status);

    bool legacyInterfacesAvailable = true;
    IUnknown* stableCompatIdentity = nullptr;
    for (std::size_t index = 0; index < 7u; ++index) {
        IUnknown* queried = nullptr;
        const HRESULT queryResult = device->QueryInterface(
            kCompatIids[index], reinterpret_cast<void**>(&queried));
        if (queryResult != S_OK || !queried) {
            std::fprintf(
                stderr, "API %u QueryInterface failed: 0x%08lX\n",
                static_cast<unsigned>(index + 1u), ULONG(queryResult));
            legacyInterfacesAvailable = false;
            if (queried) queried->Release();
            continue;
        }

        IUnknown* identity = nullptr;
        const HRESULT identityResult = queried->QueryInterface(
            IID_IUnknown, reinterpret_cast<void**>(&identity));
        if (identityResult != S_OK || !identity) {
            std::fprintf(
                stderr, "API %u IUnknown query failed: 0x%08lX\n",
                static_cast<unsigned>(index + 1u), ULONG(identityResult));
            legacyInterfacesAvailable = false;
        } else if (!stableCompatIdentity) {
            stableCompatIdentity = identity;
            identity = nullptr;
        } else if (identity != stableCompatIdentity) {
            std::fprintf(
                stderr, "API %u returned a different compatibility IUnknown\n",
                static_cast<unsigned>(index + 1u));
            legacyInterfacesAvailable = false;
        }

        if (identity) identity->Release();
        queried->Release();
    }
    if (stableCompatIdentity) stableCompatIdentity->Release();

    const float expected[4] = { 11.0f, 22.0f, 33.0f, 44.0f };
    D3D9GtaSaFloatConstantRange range{ 7u, 1u, expected };
    D3D9GtaSaStateBatch batch{};
    batch.StructSize = sizeof(batch);
    batch.ApiVersion = GtaSaCompatApiVersions::kStateBatch;
    batch.VertexFloatRangeCount = 1u;
    batch.VertexFloatRanges = &range;

    if (SUCCEEDED(result)) result = compat->SubmitStateBatch(&batch);

    float actual[4]{};
    if (SUCCEEDED(result)) result = device->GetVertexShaderConstantF(7u, actual, 1u);
    const bool valuesMatch = SUCCEEDED(result) &&
        NearlyEqual(actual[0], expected[0]) &&
        NearlyEqual(actual[1], expected[1]) &&
        NearlyEqual(actual[2], expected[2]) &&
        NearlyEqual(actual[3], expected[3]);

    D3D9GtaSaStateBatch tooNew = batch;
    tooNew.ApiVersion = GtaSaCompatApiVersions::kStateDrawBatch;
    const HRESULT tooNewResult = compat->SubmitStateBatch(&tooNew);

    ID3D9GtaSaCompatDevice3* fullJournal = nullptr;
    HRESULT fullJournalResult = device->QueryInterface(
        __uuidof(ID3D9GtaSaCompatDevice3),
        reinterpret_cast<void**>(&fullJournal));
    DWORD fullJournalCullMode = 0;
    if (SUCCEEDED(fullJournalResult)) {
        fullJournalResult = device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
    }
    if (SUCCEEDED(fullJournalResult)) {
        fullJournalResult = fullJournal->BeginStateJournal();
    }
    if (SUCCEEDED(fullJournalResult)) {
        fullJournalResult = device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW);
    }
    if (SUCCEEDED(fullJournalResult)) {
        fullJournalResult = fullJournal->RestoreStateJournal();
    }
    if (SUCCEEDED(fullJournalResult)) {
        fullJournalResult = device->GetRenderState(
            D3DRS_CULLMODE, &fullJournalCullMode);
    }
    const bool fullJournalWorks =
        SUCCEEDED(fullJournalResult) && fullJournalCullMode == D3DCULL_CCW;

    ID3D9GtaSaCompatDevice6* selectiveJournal = nullptr;
    const HRESULT selectiveQueryResult = device->QueryInterface(
        __uuidof(ID3D9GtaSaCompatDevice6),
        reinterpret_cast<void**>(&selectiveJournal));
    HRESULT selectiveNoOwnedResult = selectiveQueryResult;
    DWORD selectiveNoOwnedCullMode = 0;
    if (SUCCEEDED(selectiveNoOwnedResult)) {
        selectiveNoOwnedResult = device->SetRenderState(
            D3DRS_CULLMODE, D3DCULL_CCW);
    }
    if (SUCCEEDED(selectiveNoOwnedResult)) {
        selectiveNoOwnedResult = selectiveJournal->BeginSelectiveStateJournal();
    }
    if (SUCCEEDED(selectiveNoOwnedResult)) {
        selectiveNoOwnedResult = device->SetRenderState(
            D3DRS_CULLMODE, D3DCULL_CW);
    }
    if (SUCCEEDED(selectiveNoOwnedResult)) {
        selectiveNoOwnedResult = selectiveJournal->RestoreStateJournal();
    }
    if (SUCCEEDED(selectiveNoOwnedResult)) {
        selectiveNoOwnedResult = device->GetRenderState(
            D3DRS_CULLMODE, &selectiveNoOwnedCullMode);
    }

    const HRESULT captureOutsideResult = selectiveJournal
        ? selectiveJournal->SetStateJournalCaptureEnabled(TRUE)
        : selectiveQueryResult;
    HRESULT selectiveOwnedResult = selectiveQueryResult;
    DWORD selectiveOwnedCullMode = 0;
    if (SUCCEEDED(selectiveOwnedResult)) {
        selectiveOwnedResult = device->SetRenderState(
            D3DRS_CULLMODE, D3DCULL_CCW);
    }
    if (SUCCEEDED(selectiveOwnedResult)) {
        selectiveOwnedResult = selectiveJournal->BeginSelectiveStateJournal();
    }
    if (SUCCEEDED(selectiveOwnedResult)) {
        selectiveOwnedResult = device->SetRenderState(
            D3DRS_CULLMODE, D3DCULL_CW);
    }
    if (SUCCEEDED(selectiveOwnedResult)) {
        selectiveOwnedResult =
            selectiveJournal->SetStateJournalCaptureEnabled(TRUE);
    }
    if (SUCCEEDED(selectiveOwnedResult)) {
        selectiveOwnedResult = device->SetRenderState(
            D3DRS_CULLMODE, D3DCULL_NONE);
    }
    if (SUCCEEDED(selectiveOwnedResult)) {
        selectiveOwnedResult =
            selectiveJournal->SetStateJournalCaptureEnabled(FALSE);
    }
    if (SUCCEEDED(selectiveOwnedResult)) {
        selectiveOwnedResult = selectiveJournal->RestoreStateJournal();
    }
    if (SUCCEEDED(selectiveOwnedResult)) {
        selectiveOwnedResult = device->GetRenderState(
            D3DRS_CULLMODE, &selectiveOwnedCullMode);
    }
    const bool selectiveJournalWorks =
        SUCCEEDED(selectiveNoOwnedResult) &&
        selectiveNoOwnedCullMode == D3DCULL_CW &&
        captureOutsideResult == D3DERR_INVALIDCALL &&
        SUCCEEDED(selectiveOwnedResult) &&
        selectiveOwnedCullMode == D3DCULL_CW;
    const bool api7Advertised =
        GtaSaCompatApiVersions::Supports(
            status.ApiVersion, GtaSaCompatApiVersions::kSelectiveStateJournal) &&
        (status.Flags & D3D9_GTA_SA_COMPAT_SELECTIVE_STATE_JOURNAL) != 0;

    std::printf(
        "api=%u flags=0x%08X state_batch_v3=%s legacy_qi=%s "
        "api4_journal=%s api7_selective=%s result=0x%08lX v6_result=0x%08lX\n",
        status.ApiVersion, status.Flags, valuesMatch ? "yes" : "no",
        legacyInterfacesAvailable ? "yes" : "no",
        fullJournalWorks ? "yes" : "no",
        selectiveJournalWorks && api7Advertised ? "yes" : "no",
        ULONG(result), ULONG(tooNewResult));

    if (selectiveJournal) selectiveJournal->Release();
    if (fullJournal) fullJournal->Release();
    compat->Release();
    device->Release();
    d3d9->Release();
    DestroyWindow(window);
    FreeLibrary(module);

    return valuesMatch && legacyInterfacesAvailable && fullJournalWorks &&
        selectiveJournalWorks && api7Advertised &&
        GtaSaCompatApiVersions::Supports(
            status.ApiVersion, GtaSaCompatApiVersions::kStateBatch) &&
        (status.ApiVersion >= GtaSaCompatApiVersions::kStateDrawBatch ||
            tooNewResult == D3DERR_INVALIDCALL)
        ? 0
        : 9;
}
