#include "../ProperShadersStateJournal.h"
#include "../GtaSaCompatApiVersions.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>

static_assert(!std::is_copy_constructible_v<ProperShadersNativeCaptureScope>,
    "native capture scope must have unique ownership");
static_assert(!std::is_copy_assignable_v<ProperShadersNativeCaptureScope>,
    "native capture scope must not transfer ownership by copy assignment");

void JournalProbeObserveTransaction(
    const char*, std::span<const JournalProbeRecord>, bool)
{
}

std::atomic<bool> g_properShadersStateAttributionActive{ false };

unsigned g_attributionTransactions = 0;
unsigned g_attributionRestores = 0;
unsigned g_attributionEnds = 0;
unsigned g_attributionLastRecordCount = 0;
JournalProbeRecord g_attributionFirstRecord{};

void JournalAttributionObserveTransaction(
    const char*, std::span<const JournalProbeRecord> records, bool,
    const JournalProbeTransactionInfo&)
{
    ++g_attributionTransactions;
    g_attributionLastRecordCount = static_cast<unsigned>(records.size());
    if (!records.empty()) g_attributionFirstRecord = records[0];
}

void JournalAttributionObserveRestore(std::uint64_t, std::uint64_t)
{
    ++g_attributionRestores;
}

void JournalAttributionReset()
{
    g_attributionTransactions = 0;
    g_attributionRestores = 0;
    g_attributionEnds = 0;
    g_attributionLastRecordCount = 0;
    g_attributionFirstRecord = JournalProbeRecord{};
}

void JournalAttributionDump()
{
}

void JournalAttributionOnPresent()
{
}

void JournalAttributionStartCapture()
{
    JournalAttributionReset();
    g_properShadersStateAttributionActive.store(true, std::memory_order_release);
}

void JournalAttributionStopCapture()
{
    g_properShadersStateAttributionActive.store(false, std::memory_order_release);
}

bool JournalAttributionTryBeginTransaction()
{
    return g_properShadersStateAttributionActive.load(std::memory_order_acquire);
}

void JournalAttributionEndTransaction()
{
    ++g_attributionEnds;
}

std::uint64_t JournalAttributionCurrentFrame()
{
    return 0;
}

namespace
{
bool Check(bool condition, const char* message)
{
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

bool FloatEqual(float a, float b)
{
    return std::fabs(a - b) < 0.0001f;
}

struct LoadedD3D9
{
    HMODULE module = nullptr;
    IDirect3D9* d3d = nullptr;
};

LoadedD3D9 LoadD3D9(const char* explicitPath)
{
    std::string path;
    if (explicitPath) {
        path = explicitPath;
    } else {
        char systemDirectory[MAX_PATH]{};
        const UINT length = GetSystemDirectoryA(systemDirectory, MAX_PATH);
        if (!length || length >= MAX_PATH) return {};
        path.assign(systemDirectory, length);
        path += "\\d3d9.dll";
    }

    LoadedD3D9 loaded{};
    loaded.module = LoadLibraryExA(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!loaded.module) return loaded;

    using Direct3DCreate9Proc = IDirect3D9* (WINAPI*)(UINT);
    const auto create = reinterpret_cast<Direct3DCreate9Proc>(
        GetProcAddress(loaded.module, "Direct3DCreate9"));
    if (create) loaded.d3d = create(D3D_SDK_VERSION);
    if (!loaded.d3d) {
        FreeLibrary(loaded.module);
        loaded.module = nullptr;
    }
    return loaded;
}

LRESULT CALLBACK TestWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    return DefWindowProcA(window, message, wParam, lParam);
}

DWORD WINAPI ReleaseJournalOnWorkerThread(void* context)
{
    static_cast<ProperShadersStateJournal*>(context)->Release();
    return 0;
}
}

int main(int argc, char** argv)
{
    const bool requireNativeJournal =
        argc == 2 && std::strcmp(argv[1], "--local") != 0;
    const bool explicitLocalJournal =
        argc == 3 && std::strcmp(argv[1], "--local") == 0;
    if (argc != 1 && !requireNativeJournal && !explicitLocalJournal) {
        std::fprintf(stderr,
            "usage: ProperShadersStateJournalTests.exe [d3d9.dll | --local d3d9.dll]\n");
        return 2;
    }

    const char* backendPath = requireNativeJournal
        ? argv[1]
        : explicitLocalJournal ? argv[2] : nullptr;
    if (backendPath) {
        SetEnvironmentVariableA(
            "DXVK_CONFIG",
            requireNativeJournal
                ? "d3d9.gtaSaCompat = True; d3d9.gtaSaCompatDiagnostics = True"
                : "d3d9.gtaSaCompat = False; d3d9.gtaSaCompatDiagnostics = False");
    }

    WNDCLASSA windowClass{};
    windowClass.lpfnWndProc = TestWindowProc;
    windowClass.hInstance = GetModuleHandleA(nullptr);
    windowClass.lpszClassName = "BridgeD3D9JournalTest";
    if (!RegisterClassA(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        std::fprintf(stderr, "FAIL: RegisterClass err=%lu\n", GetLastError());
        return 1;
    }

    HWND window = CreateWindowA(
        windowClass.lpszClassName, "test", WS_OVERLAPPEDWINDOW,
        0, 0, 64, 64, nullptr, nullptr, windowClass.hInstance, nullptr);
    if (!window) {
        std::fprintf(stderr, "FAIL: CreateWindow err=%lu\n", GetLastError());
        return 1;
    }

    LoadedD3D9 loaded = LoadD3D9(backendPath);
    IDirect3D9* d3d = loaded.d3d;
    if (!d3d) {
        std::fprintf(stderr, "FAIL: load Direct3DCreate9 from %s\n",
            backendPath ? backendPath : "system d3d9.dll");
        DestroyWindow(window);
        return 1;
    }

    D3DPRESENT_PARAMETERS parameters{};
    parameters.Windowed = TRUE;
    parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    parameters.hDeviceWindow = window;
    parameters.BackBufferFormat = D3DFMT_UNKNOWN;
    parameters.BackBufferWidth = 64;
    parameters.BackBufferHeight = 64;

    IDirect3DDevice9* device = nullptr;
    HRESULT hr = d3d->CreateDevice(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &parameters, &device);
    if (FAILED(hr) && !requireNativeJournal) {
        hr = d3d->CreateDevice(
            D3DADAPTER_DEFAULT, D3DDEVTYPE_REF, window,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &parameters, &device);
    }
    if (FAILED(hr) && !requireNativeJournal) {
        hr = d3d->CreateDevice(
            D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, window,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &parameters, &device);
    }
    if (FAILED(hr)) {
        std::fprintf(stderr, "FAIL: CreateDevice hr=0x%08X\n", static_cast<unsigned>(hr));
        d3d->Release();
        FreeLibrary(loaded.module);
        DestroyWindow(window);
        return 1;
    }

    bool ok = true;
    const ProperShadersStateJournalDiagnostics diagnosticsBefore =
        ProperShadersStateJournal::GetDiagnostics();
    auto* journal = new ProperShadersStateJournal(device);
    ok &= Check(journal->SupportsNativeStateJournal() == requireNativeJournal,
        requireNativeJournal
            ? "supplied backend exposes native state journal"
            : explicitLocalJournal
                ? "supplied backend uses local state journal fallback"
                : "system D3D9 uses local state journal fallback");
    if (requireNativeJournal) {
        ID3D9GtaSaCompatDevice* compat = nullptr;
        const HRESULT queryHr = device->QueryInterface(
            __uuidof(ID3D9GtaSaCompatDevice), reinterpret_cast<void**>(&compat));
        D3D9GtaSaCompatStatus status{};
        status.StructSize = sizeof(status);
        const HRESULT statusHr = compat ? compat->GetStatus(&status) : queryHr;
        ok &= Check(SUCCEEDED(queryHr) && SUCCEEDED(statusHr),
            "supplied backend exposes GTA SA compat status");
        ok &= Check(
            GtaSaCompatApiVersions::Supports(
                status.ApiVersion, GtaSaCompatApiVersions::kStateJournal),
            "supplied backend supports API v4 state journal");
        ok &= Check((status.Flags & D3D9_GTA_SA_COMPAT_STATE_JOURNAL) != 0,
            "supplied backend advertises API v4 state journal");
        ok &= Check(
            GtaSaCompatApiVersions::Supports(
                status.ApiVersion, GtaSaCompatApiVersions::kSelectiveStateJournal),
            "supplied backend supports API v7 selective state journal");
        ok &= Check(
            (status.Flags & D3D9_GTA_SA_COMPAT_SELECTIVE_STATE_JOURNAL) != 0,
            "supplied backend advertises API v7 selective state journal");
        if (compat) compat->Release();
    }
    D3DMATRIX initialTransform{};
    initialTransform._11 = initialTransform._22 = initialTransform._33 = initialTransform._44 = 1.0f;
    D3DMATRIX changedTransform = initialTransform;
    changedTransform._41 = 12.0f;
    D3DMATERIAL9 initialMaterial{};
    initialMaterial.Diffuse.r = 0.25f;
    D3DMATERIAL9 changedMaterial = initialMaterial;
    changedMaterial.Diffuse.r = 0.75f;
    IDirect3DTexture9* textureA = nullptr;
    IDirect3DTexture9* textureB = nullptr;
    device->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &textureA, nullptr);
    device->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &textureB, nullptr);
    const float initialVs[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
    const float changedVs[4] = { 5.0f, 6.0f, 7.0f, 8.0f };
    const float initialPs[4] = { 9.0f, 10.0f, 11.0f, 12.0f };
    const float changedPs[4] = { 13.0f, 14.0f, 15.0f, 16.0f };
    const float initialBatchVs[8] = {
        21.0f, 22.0f, 23.0f, 24.0f,
        25.0f, 26.0f, 27.0f, 28.0f,
    };
    const float changedBatchVs[8] = {
        31.0f, 32.0f, 33.0f, 34.0f,
        35.0f, 36.0f, 37.0f, 38.0f,
    };
    const float initialBatchPs[8] = {
        41.0f, 42.0f, 43.0f, 44.0f,
        45.0f, 46.0f, 47.0f, 48.0f,
    };
    const float changedBatchPs[8] = {
        51.0f, 52.0f, 53.0f, 54.0f,
        55.0f, 56.0f, 57.0f, 58.0f,
    };
    const BOOL initialBatchBool = FALSE;
    const BOOL changedBatchBool = TRUE;

    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    device->SetTransform(D3DTS_WORLD, &initialTransform);
    device->SetMaterial(&initialMaterial);
    device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
    if (textureA && textureB) device->SetTexture(0, textureA);
    const bool vertexConstantsTested = SUCCEEDED(
        device->SetVertexShaderConstantF(8, initialVs, 1));
    const bool pixelConstantsTested = SUCCEEDED(
        device->SetPixelShaderConstantF(4, initialPs, 1));
    const bool stateBatchTested = journal->SupportsStateBatch() && textureA && textureB;
    if (stateBatchTested) {
        device->SetVertexShaderConstantF(20, initialBatchVs, 2);
        device->SetVertexShaderConstantB(1, &initialBatchBool, 1);
        device->SetPixelShaderConstantF(20, initialBatchPs, 2);
        device->SetPixelShaderConstantB(1, &initialBatchBool, 1);
        device->SetTexture(1, textureA);
    }

    hr = journal->BeginTransaction(GetCurrentThreadId(), 0x1234);
    ok &= Check(SUCCEEDED(hr), "BeginTransaction");
    ok &= Check(journal->OriginalFlags() == 0x1234, "original flags");
    ok &= Check(!ProperShadersStateJournalRequiresBaselineRestart(journal),
        "active journal without an internal failure preserves effect errors");

    if (requireNativeJournal && SUCCEEDED(hr)) {
        const ProperShadersStateJournalDiagnostics scopeBefore =
            ProperShadersStateJournal::GetDiagnostics();
        const HRESULT outerBegin = journal->BeginNativeCaptureScope();
        const HRESULT innerBegin = journal->BeginNativeCaptureScope();
        HRESULT scopedStateHr = D3D_OK;
        if (SUCCEEDED(outerBegin) && SUCCEEDED(innerBegin)) {
            scopedStateHr = journal->SetRenderState(
                D3DRS_CULLMODE, D3DCULL_NONE);
        }
        const HRESULT innerEnd = journal->EndNativeCaptureScope();
        const HRESULT outerEnd = journal->EndNativeCaptureScope();
        const ProperShadersStateJournalDiagnostics scopeAfter =
            ProperShadersStateJournal::GetDiagnostics();
        ok &= Check(SUCCEEDED(outerBegin) && SUCCEEDED(innerBegin),
            "nested native capture scopes begin");
        ok &= Check(SUCCEEDED(scopedStateHr),
            "state write succeeds inside nested native capture scopes");
        ok &= Check(SUCCEEDED(innerEnd) && SUCCEEDED(outerEnd),
            "nested native capture scopes end");
        ok &= Check(scopeAfter.nativeCaptureEnables ==
                scopeBefore.nativeCaptureEnables + 1 &&
            scopeAfter.nativeCaptureDisables ==
                scopeBefore.nativeCaptureDisables + 1,
            "nested scopes toggle native capture once");

        const ProperShadersStateJournalDiagnostics operationBefore =
            ProperShadersStateJournal::GetDiagnostics();
        ProperShadersNativeCaptureScope operationScope(journal);
        HRESULT operationHr = operationScope.BeginResult();
        if (SUCCEEDED(operationHr)) {
            operationHr = journal->SetRenderState(
                D3DRS_ALPHABLENDENABLE, TRUE);
        }
        if (SUCCEEDED(operationHr)) {
            operationHr = journal->SetSamplerState(
                0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        }
        if (SUCCEEDED(operationHr)) {
            operationHr = journal->SetTextureStageState(
                0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        }
        operationHr = operationScope.Finish(operationHr);
        const ProperShadersStateJournalDiagnostics operationAfter =
            ProperShadersStateJournal::GetDiagnostics();
        ok &= Check(SUCCEEDED(operationHr),
            "native effect-call scope forwards multiple state writes");
        ok &= Check(operationAfter.nativeCaptureEnables ==
                operationBefore.nativeCaptureEnables + 1 &&
            operationAfter.nativeCaptureDisables ==
                operationBefore.nativeCaptureDisables + 1,
            "native effect-call scope amortizes multiple writes into one toggle pair");

        const ProperShadersStateJournalDiagnostics outsideBefore =
            ProperShadersStateJournal::GetDiagnostics();
        const HRESULT outsideStateHr = journal->SetRenderState(
            D3DRS_CULLMODE, D3DCULL_NONE);
        const ProperShadersStateJournalDiagnostics outsideAfter =
            ProperShadersStateJournal::GetDiagnostics();
        ok &= Check(SUCCEEDED(outsideStateHr),
            "state write outside scope succeeds");
        ok &= Check(outsideAfter.nativeCaptureEnables ==
                outsideBefore.nativeCaptureEnables + 1 &&
            outsideAfter.nativeCaptureDisables ==
                outsideBefore.nativeCaptureDisables + 1,
            "scope outside retains per-write capture semantics");
    }

    if (SUCCEEDED(hr)) hr = journal->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    ok &= Check(SUCCEEDED(hr), "SetRenderState through journal");
    if (SUCCEEDED(hr)) hr = journal->SetSamplerState(
        0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    ok &= Check(SUCCEEDED(hr), "SetSamplerState through journal");
    if (SUCCEEDED(hr)) hr = journal->SetTextureStageState(
        0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    ok &= Check(SUCCEEDED(hr), "SetTextureStageState through journal");
    if (SUCCEEDED(hr)) hr = journal->SetTransform(D3DTS_WORLD, &changedTransform);
    ok &= Check(SUCCEEDED(hr), "SetTransform through journal");
    if (SUCCEEDED(hr)) hr = journal->SetMaterial(&changedMaterial);
    ok &= Check(SUCCEEDED(hr), "SetMaterial through journal");
    if (SUCCEEDED(hr)) hr = journal->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
    ok &= Check(SUCCEEDED(hr), "SetFVF through journal");
    if (SUCCEEDED(hr) && textureA && textureB) hr = journal->SetTexture(0, textureB);
    ok &= Check(SUCCEEDED(hr), "SetTexture through journal");
    if (SUCCEEDED(hr) && vertexConstantsTested) {
        hr = journal->SetVertexShaderConstantF(8, changedVs, 1);
    }
    ok &= Check(SUCCEEDED(hr), "SetVertexShaderConstantF through journal");
    if (SUCCEEDED(hr) && pixelConstantsTested) {
        hr = journal->SetPixelShaderConstantF(4, changedPs, 1);
    }
    ok &= Check(SUCCEEDED(hr), "SetPixelShaderConstantF through journal");

    if (stateBatchTested) {
        const D3D9GtaSaFloatConstantRange vertexFloatRanges[] = {
            { 20, 2, changedBatchVs },
        };
        const D3D9GtaSaBoolConstantRange vertexBoolRanges[] = {
            { 1, 1, &changedBatchBool },
        };
        const D3D9GtaSaFloatConstantRange pixelFloatRanges[] = {
            { 20, 2, changedBatchPs },
        };
        const D3D9GtaSaBoolConstantRange pixelBoolRanges[] = {
            { 1, 1, &changedBatchBool },
        };
        const D3D9GtaSaTextureBinding textureBindings[] = {
            { 1, textureB },
        };
        D3D9GtaSaStateBatch batch{};
        batch.StructSize = sizeof(batch);
        batch.ApiVersion = GtaSaCompatApiVersions::kStateBatch;
        batch.VertexFloatRangeCount = 1;
        batch.VertexFloatRanges = vertexFloatRanges;
        batch.VertexBoolRangeCount = 1;
        batch.VertexBoolRanges = vertexBoolRanges;
        batch.PixelFloatRangeCount = 1;
        batch.PixelFloatRanges = pixelFloatRanges;
        batch.PixelBoolRangeCount = 1;
        batch.PixelBoolRanges = pixelBoolRanges;
        batch.TextureBindingCount = 1;
        batch.TextureBindings = textureBindings;

        hr = journal->SubmitStateBatch(&batch);
        ok &= Check(SUCCEEDED(hr), "SubmitStateBatch through journal");

        float currentBatchVs[8]{};
        float currentBatchPs[8]{};
        BOOL currentVertexBool = FALSE;
        BOOL currentPixelBool = FALSE;
        IDirect3DBaseTexture9* currentTexture = nullptr;
        device->GetVertexShaderConstantF(20, currentBatchVs, 2);
        device->GetVertexShaderConstantB(1, &currentVertexBool, 1);
        device->GetPixelShaderConstantF(20, currentBatchPs, 2);
        device->GetPixelShaderConstantB(1, &currentPixelBool, 1);
        device->GetTexture(1, &currentTexture);
        ok &= Check(std::memcmp(currentBatchVs, changedBatchVs,
            sizeof(changedBatchVs)) == 0, "batch vertex constants applied");
        ok &= Check(std::memcmp(currentBatchPs, changedBatchPs,
            sizeof(changedBatchPs)) == 0, "batch pixel constants applied");
        ok &= Check(currentVertexBool == TRUE, "batch vertex bool applied");
        ok &= Check(currentPixelBool == TRUE, "batch pixel bool applied");
        ok &= Check(currentTexture == textureB, "batch texture applied");
        if (currentTexture) currentTexture->Release();
    }

    hr = journal->Restore();
    ok &= Check(SUCCEEDED(hr), "Restore");
    const ProperShadersStateJournalDiagnostics diagnosticsAfter =
        ProperShadersStateJournal::GetDiagnostics();
    if (requireNativeJournal) {
        ok &= Check(diagnosticsAfter.nativeBegins == diagnosticsBefore.nativeBegins + 1,
            "native begin diagnostic incremented once");
        ok &= Check(diagnosticsAfter.nativeRestores == diagnosticsBefore.nativeRestores + 1,
            "native restore diagnostic incremented once");
        ok &= Check(diagnosticsAfter.localFallbacks == diagnosticsBefore.localFallbacks,
            "native transaction avoided local fallback");
        ok &= Check(diagnosticsAfter.nativeFailures == diagnosticsBefore.nativeFailures,
            "native transaction completed without failure");
    } else {
        ok &= Check(diagnosticsAfter.localFallbacks == diagnosticsBefore.localFallbacks + 1,
            "local fallback diagnostic incremented once");
        ok &= Check(diagnosticsAfter.nativeBegins == diagnosticsBefore.nativeBegins,
            "local fallback did not begin native journal");
        ok &= Check(diagnosticsAfter.nativeRestores == diagnosticsBefore.nativeRestores,
            "local fallback did not restore native journal");
    }

    DWORD value = 0;
    device->GetRenderState(D3DRS_CULLMODE, &value);
    ok &= Check(value == D3DCULL_CCW, "render state restored");
    device->GetSamplerState(0, D3DSAMP_MAGFILTER, &value);
    ok &= Check(value == D3DTEXF_POINT, "sampler state restored");
    device->GetTextureStageState(0, D3DTSS_COLOROP, &value);
    ok &= Check(value == D3DTOP_MODULATE, "texture stage state restored");

    if (requireNativeJournal) {
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
        auto* unclosedScopeJournal = new ProperShadersStateJournal(device);
        HRESULT unclosedScopeHr = unclosedScopeJournal->BeginTransaction(
            GetCurrentThreadId(), 0);
        if (SUCCEEDED(unclosedScopeHr)) {
            unclosedScopeHr = unclosedScopeJournal->BeginNativeCaptureScope();
        }
        if (SUCCEEDED(unclosedScopeHr)) {
            unclosedScopeHr = unclosedScopeJournal->SetRenderState(
                D3DRS_CULLMODE, D3DCULL_NONE);
        }
        const HRESULT unclosedScopeRestoreHr = unclosedScopeJournal->Restore();
        DWORD unclosedScopeValue = 0;
        device->GetRenderState(D3DRS_CULLMODE, &unclosedScopeValue);
        ok &= Check(SUCCEEDED(unclosedScopeHr) &&
                SUCCEEDED(unclosedScopeRestoreHr) &&
                unclosedScopeValue == D3DCULL_CCW,
            "Restore closes an unclosed native capture scope");
        unclosedScopeJournal->Release();
    }

    if (requireNativeJournal) {
        ID3D9GtaSaCompatDevice6* nativeCapture = nullptr;
        HRESULT retrySetupHr = device->QueryInterface(
            __uuidof(ID3D9GtaSaCompatDevice6),
            reinterpret_cast<void**>(&nativeCapture));
        auto* retryJournal = new ProperShadersStateJournal(device);
        if (SUCCEEDED(retrySetupHr)) {
            retrySetupHr = retryJournal->BeginTransaction(GetCurrentThreadId(), 0);
        }

        const ProperShadersStateJournalDiagnostics retryBefore =
            ProperShadersStateJournal::GetDiagnostics();
        HRESULT scopeBeginHr = retrySetupHr;
        HRESULT forcedCloseHr = retrySetupHr;
        HRESULT firstFinishHr = retrySetupHr;
        HRESULT reopenHr = retrySetupHr;
        {
            ProperShadersNativeCaptureScope retryScope(retryJournal);
            scopeBeginHr = retryScope.BeginResult();
            if (scopeBeginHr == D3D_OK && nativeCapture) {
                forcedCloseHr = nativeCapture->RestoreStateJournal();
            }
            if (SUCCEEDED(forcedCloseHr)) {
                firstFinishHr = retryScope.Finish(D3D_OK);
            }
            if (FAILED(firstFinishHr)) {
                reopenHr = nativeCapture->BeginSelectiveStateJournal();
            }
        }
        const ProperShadersStateJournalDiagnostics retryAfter =
            ProperShadersStateJournal::GetDiagnostics();
        const HRESULT reopenRestoreHr = SUCCEEDED(reopenHr)
            ? nativeCapture->RestoreStateJournal()
            : reopenHr;
        ok &= Check(SUCCEEDED(retrySetupHr) && SUCCEEDED(scopeBeginHr) &&
                SUCCEEDED(forcedCloseHr) && FAILED(firstFinishHr) &&
                SUCCEEDED(reopenHr) && SUCCEEDED(reopenRestoreHr),
            "capture-close retry fault injection completed");
        ok &= Check(retryAfter.nativeCaptureDisables ==
                retryBefore.nativeCaptureDisables + 1,
            "failed capture close retains ownership for destructor retry");
        ok &= Check(ProperShadersStateJournalRequiresBaselineRestart(retryJournal),
            "native capture failure requires baseline restart");
        retryJournal->Disable();
        retryJournal->Release();
        if (nativeCapture) nativeCapture->Release();
    }

    constexpr DWORD kUnownedBaselineColorWrite =
        D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
        D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA;
    constexpr DWORD kUnownedExternalColorWrite =
        D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN;
    device->SetRenderState(D3DRS_COLORWRITEENABLE, kUnownedBaselineColorWrite);
    auto* unownedStateJournal = new ProperShadersStateJournal(device);
    HRESULT unownedStateHr = unownedStateJournal->BeginTransaction(
        GetCurrentThreadId(), 0);
    if (SUCCEEDED(unownedStateHr)) {
        unownedStateHr = device->SetRenderState(
            D3DRS_COLORWRITEENABLE, kUnownedExternalColorWrite);
    }
    if (SUCCEEDED(unownedStateHr)) {
        unownedStateHr = unownedStateJournal->Restore();
    }
    DWORD unownedColorWrite = 0;
    const HRESULT unownedStateReadHr = device->GetRenderState(
        D3DRS_COLORWRITEENABLE, &unownedColorWrite);
    ok &= Check(SUCCEEDED(unownedStateHr) && SUCCEEDED(unownedStateReadHr) &&
            unownedColorWrite == kUnownedExternalColorWrite,
        "device state written outside the effect state manager survives restore");
    unownedStateJournal->Release();

    D3DMATRIX restoredTransform{};
    device->GetTransform(D3DTS_WORLD, &restoredTransform);
    ok &= Check(FloatEqual(restoredTransform._41, 0.0f), "transform restored");
    D3DMATERIAL9 restoredMaterial{};
    device->GetMaterial(&restoredMaterial);
    ok &= Check(FloatEqual(restoredMaterial.Diffuse.r, 0.25f), "material restored");
    device->GetFVF(&value);
    ok &= Check(value == (D3DFVF_XYZ | D3DFVF_DIFFUSE), "FVF restored");

    if (textureA && textureB) {
        IDirect3DBaseTexture9* restoredTexture = nullptr;
        device->GetTexture(0, &restoredTexture);
        ok &= Check(restoredTexture == textureA, "texture restored");
        if (restoredTexture) restoredTexture->Release();
    }

    float restoredVs[4]{};
    if (SUCCEEDED(device->GetVertexShaderConstantF(8, restoredVs, 1))) {
        ok &= Check(std::memcmp(restoredVs, initialVs, sizeof(initialVs)) == 0,
            "vertex constants restored");
    }
    float restoredPs[4]{};
    if (SUCCEEDED(device->GetPixelShaderConstantF(4, restoredPs, 1))) {
        ok &= Check(std::memcmp(restoredPs, initialPs, sizeof(initialPs)) == 0,
            "pixel constants restored");
    }

    if (stateBatchTested) {
        float restoredBatchVs[8]{};
        float restoredBatchPs[8]{};
        BOOL restoredVertexBool = TRUE;
        BOOL restoredPixelBool = TRUE;
        IDirect3DBaseTexture9* restoredBatchTexture = nullptr;
        device->GetVertexShaderConstantF(20, restoredBatchVs, 2);
        device->GetVertexShaderConstantB(1, &restoredVertexBool, 1);
        device->GetPixelShaderConstantF(20, restoredBatchPs, 2);
        device->GetPixelShaderConstantB(1, &restoredPixelBool, 1);
        device->GetTexture(1, &restoredBatchTexture);
        ok &= Check(std::memcmp(restoredBatchVs, initialBatchVs,
            sizeof(initialBatchVs)) == 0, "batch vertex constants restored");
        ok &= Check(std::memcmp(restoredBatchPs, initialBatchPs,
            sizeof(initialBatchPs)) == 0, "batch pixel constants restored");
        ok &= Check(restoredVertexBool == FALSE, "batch vertex bool restored");
        ok &= Check(restoredPixelBool == FALSE, "batch pixel bool restored");
        ok &= Check(restoredBatchTexture == textureA, "batch texture restored");
        if (restoredBatchTexture) restoredBatchTexture->Release();
    }

    constexpr DWORD kPreviouslyUndefinedLight = 63;
    D3DLIGHT9 undefinedLight{};
    undefinedLight.Type = D3DLIGHT_DIRECTIONAL;
    undefinedLight.Direction.z = 1.0f;
    D3DLIGHT9 queriedLight{};
    ok &= Check(FAILED(device->GetLight(kPreviouslyUndefinedLight, &queriedLight)),
        "undefined-light test starts without a light");
    auto* undefinedLightJournal = new ProperShadersStateJournal(device);
    HRESULT undefinedLightHr = undefinedLightJournal->BeginTransaction(
        GetCurrentThreadId(), 0);
    if (SUCCEEDED(undefinedLightHr)) {
        undefinedLightHr = undefinedLightJournal->SetLight(
            kPreviouslyUndefinedLight, &undefinedLight);
    }
    const HRESULT undefinedLightRestoreHr = undefinedLightJournal->Restore();
    ok &= Check(requireNativeJournal
            ? SUCCEEDED(undefinedLightHr) && !undefinedLightJournal->HasFailed()
            : FAILED(undefinedLightHr) && undefinedLightJournal->HasFailed(),
        requireNativeJournal
            ? "native journal accepts a previously undefined light"
            : "local journal fails closed when a previously undefined light is set");
    ok &= Check(SUCCEEDED(undefinedLightRestoreHr),
        "undefined-light transaction remains restorable");
    ok &= Check(FAILED(device->GetLight(kPreviouslyUndefinedLight, &queriedLight)),
        "undefined-light restore does not leak a new light");
    undefinedLightJournal->Release();

    constexpr DWORD kPreviouslyUndefinedLightEnable = 62;
    BOOL queriedLightEnable = FALSE;
    ok &= Check(FAILED(device->GetLightEnable(
            kPreviouslyUndefinedLightEnable, &queriedLightEnable)),
        "undefined-light-enable test starts without a light");
    auto* undefinedLightEnableJournal = new ProperShadersStateJournal(device);
    HRESULT undefinedLightEnableHr = undefinedLightEnableJournal->BeginTransaction(
        GetCurrentThreadId(), 0);
    if (SUCCEEDED(undefinedLightEnableHr)) {
        undefinedLightEnableHr = undefinedLightEnableJournal->LightEnable(
            kPreviouslyUndefinedLightEnable, TRUE);
    }
    const HRESULT undefinedLightEnableRestoreHr =
        undefinedLightEnableJournal->Restore();
    ok &= Check(requireNativeJournal
            ? SUCCEEDED(undefinedLightEnableHr) &&
                !undefinedLightEnableJournal->HasFailed()
            : FAILED(undefinedLightEnableHr) &&
                undefinedLightEnableJournal->HasFailed(),
        requireNativeJournal
            ? "native journal accepts enabling a previously undefined light"
            : "local journal fails closed when a previously undefined light is enabled");
    ok &= Check(SUCCEEDED(undefinedLightEnableRestoreHr),
        "undefined-light-enable transaction remains restorable");
    ok &= Check(FAILED(device->GetLightEnable(
            kPreviouslyUndefinedLightEnable, &queriedLightEnable)),
        "undefined-light-enable restore does not leak a new light");
    undefinedLightEnableJournal->Release();

    constexpr DWORD kEnabledCustomLight = 7;
    D3DLIGHT9 customLight{};
    customLight.Type = D3DLIGHT_DIRECTIONAL;
    customLight.Diffuse.r = 0.375f;
    customLight.Direction.x = 0.25f;
    customLight.Direction.z = 0.75f;
    device->SetLight(kEnabledCustomLight, &customLight);
    device->LightEnable(kEnabledCustomLight, TRUE);
    auto* enabledCustomLightJournal = new ProperShadersStateJournal(device);
    HRESULT enabledCustomLightHr = enabledCustomLightJournal->BeginTransaction(
        GetCurrentThreadId(), 0);
    if (SUCCEEDED(enabledCustomLightHr)) {
        enabledCustomLightHr = enabledCustomLightJournal->LightEnable(
            kEnabledCustomLight, FALSE);
    }
    if (SUCCEEDED(enabledCustomLightHr)) {
        enabledCustomLightHr = enabledCustomLightJournal->Restore();
    }
    D3DLIGHT9 restoredCustomLight{};
    BOOL restoredCustomLightEnable = FALSE;
    const HRESULT restoredCustomLightHr = device->GetLight(
        kEnabledCustomLight, &restoredCustomLight);
    const HRESULT restoredCustomLightEnableHr = device->GetLightEnable(
        kEnabledCustomLight, &restoredCustomLightEnable);
    ok &= Check(SUCCEEDED(enabledCustomLightHr) &&
            SUCCEEDED(restoredCustomLightHr) &&
            SUCCEEDED(restoredCustomLightEnableHr) &&
            FloatEqual(restoredCustomLight.Diffuse.r, customLight.Diffuse.r) &&
            FloatEqual(restoredCustomLight.Direction.x, customLight.Direction.x) &&
            FloatEqual(restoredCustomLight.Direction.z, customLight.Direction.z) &&
            restoredCustomLightEnable != FALSE,
        "light-enable restore preserves the enabled custom light definition");
    enabledCustomLightJournal->Release();

    constexpr DWORD kEnableOnlyStateBlockLight = 11;
    IDirect3DStateBlock9* enableOnlyStateBlock = nullptr;
    HRESULT enableOnlyStateBlockHr = device->BeginStateBlock();
    if (SUCCEEDED(enableOnlyStateBlockHr)) {
        enableOnlyStateBlockHr = device->LightEnable(
            kEnableOnlyStateBlockLight, FALSE);
    }
    if (SUCCEEDED(enableOnlyStateBlockHr)) {
        enableOnlyStateBlockHr = device->EndStateBlock(&enableOnlyStateBlock);
    }

    D3DLIGHT9 captureSourceLight{};
    captureSourceLight.Type = D3DLIGHT_DIRECTIONAL;
    captureSourceLight.Diffuse.r = 0.25f;
    captureSourceLight.Direction.z = 1.0f;
    if (SUCCEEDED(enableOnlyStateBlockHr)) {
        enableOnlyStateBlockHr = device->SetLight(
            kEnableOnlyStateBlockLight, &captureSourceLight);
    }
    if (SUCCEEDED(enableOnlyStateBlockHr)) {
        enableOnlyStateBlockHr = device->LightEnable(
            kEnableOnlyStateBlockLight, TRUE);
    }
    if (SUCCEEDED(enableOnlyStateBlockHr)) {
        enableOnlyStateBlockHr = enableOnlyStateBlock->Capture();
    }

    D3DLIGHT9 postCaptureLight = captureSourceLight;
    postCaptureLight.Diffuse.r = 0.875f;
    postCaptureLight.Direction.x = 0.5f;
    if (SUCCEEDED(enableOnlyStateBlockHr)) {
        enableOnlyStateBlockHr = device->SetLight(
            kEnableOnlyStateBlockLight, &postCaptureLight);
    }
    if (SUCCEEDED(enableOnlyStateBlockHr)) {
        enableOnlyStateBlockHr = device->LightEnable(
            kEnableOnlyStateBlockLight, FALSE);
    }
    if (SUCCEEDED(enableOnlyStateBlockHr)) {
        enableOnlyStateBlockHr = enableOnlyStateBlock->Apply();
    }

    D3DLIGHT9 enableOnlyRestoredLight{};
    BOOL enableOnlyRestored = FALSE;
    const HRESULT enableOnlyGetLightHr = device->GetLight(
        kEnableOnlyStateBlockLight, &enableOnlyRestoredLight);
    const HRESULT enableOnlyGetEnableHr = device->GetLightEnable(
        kEnableOnlyStateBlockLight, &enableOnlyRestored);
    ok &= Check(SUCCEEDED(enableOnlyStateBlockHr) &&
            SUCCEEDED(enableOnlyGetLightHr) &&
            SUCCEEDED(enableOnlyGetEnableHr) &&
            FloatEqual(enableOnlyRestoredLight.Diffuse.r, postCaptureLight.Diffuse.r) &&
            FloatEqual(enableOnlyRestoredLight.Direction.x, postCaptureLight.Direction.x) &&
            enableOnlyRestored != FALSE,
        "state-block Capture updates light enable without capturing its definition");
    if (enableOnlyStateBlock) enableOnlyStateBlock->Release();

    if (requireNativeJournal) {
        ID3D9GtaSaCompatDevice3* nativeJournal = nullptr;
        const HRESULT nativeQueryHr = device->QueryInterface(
            __uuidof(ID3D9GtaSaCompatDevice3),
            reinterpret_cast<void**>(&nativeJournal));
        ok &= Check(SUCCEEDED(nativeQueryHr) && nativeJournal,
            "native journal interface is available for state-block exclusion test");
        if (nativeJournal) {
            device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
            HRESULT nativeBeginHr = nativeJournal->BeginStateJournal();
            if (SUCCEEDED(nativeBeginHr)) {
                nativeBeginHr = device->SetRenderState(
                    D3DRS_CULLMODE, D3DCULL_NONE);
            }
            const HRESULT stateBlockBeginHr = SUCCEEDED(nativeBeginHr)
                ? device->BeginStateBlock()
                : nativeBeginHr;
            const HRESULT nativeRestoreHr = SUCCEEDED(nativeBeginHr)
                ? nativeJournal->RestoreStateJournal()
                : nativeBeginHr;
            if (SUCCEEDED(stateBlockBeginHr)) {
                IDirect3DStateBlock9* recorded = nullptr;
                device->EndStateBlock(&recorded);
                if (recorded) recorded->Release();
            }
            DWORD restoredCullMode = 0;
            device->GetRenderState(D3DRS_CULLMODE, &restoredCullMode);
            ok &= Check(stateBlockBeginHr == D3DERR_INVALIDCALL,
                "active native journal rejects BeginStateBlock");
            ok &= Check(SUCCEEDED(nativeRestoreHr) && restoredCullMode == D3DCULL_CCW,
                "state-block rejection preserves native journal restoration");
            nativeJournal->Release();
        }
    }

    auto* wrongThreadJournal = new ProperShadersStateJournal(device);
    ok &= Check(FAILED(wrongThreadJournal->BeginTransaction(
        GetCurrentThreadId() + 1, 0)), "wrong thread rejects transaction");
    ok &= Check(!wrongThreadJournal->IsActive(),
        "wrong-thread rejection leaves journal inactive");
    wrongThreadJournal->Release();

    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
    auto* nestedJournal = new ProperShadersStateJournal(device);
    hr = nestedJournal->BeginTransaction(GetCurrentThreadId(), 0);
    if (SUCCEEDED(hr)) hr = nestedJournal->SetRenderState(
        D3DRS_CULLMODE, D3DCULL_NONE);
    ok &= Check(SUCCEEDED(hr), "nested test setup");
    ok &= Check(FAILED(nestedJournal->BeginTransaction(GetCurrentThreadId(), 0)),
        "nested transaction rejected");
    device->GetRenderState(D3DRS_CULLMODE, &value);
    ok &= Check(value == D3DCULL_CCW, "nested begin restores active transaction");
    ok &= Check(!nestedJournal->IsActive() && nestedJournal->IsDisabled(),
        "nested begin leaves journal inactive and disabled");
    nestedJournal->Release();

    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
    auto* disabledActiveJournal = new ProperShadersStateJournal(device);
    hr = disabledActiveJournal->BeginTransaction(GetCurrentThreadId(), 0);
    if (SUCCEEDED(hr)) hr = disabledActiveJournal->SetRenderState(
        D3DRS_CULLMODE, D3DCULL_NONE);
    disabledActiveJournal->Disable();
    device->GetRenderState(D3DRS_CULLMODE, &value);
    ok &= Check(SUCCEEDED(hr) && value == D3DCULL_CCW,
        "Disable restores an owner-thread active transaction");
    ok &= Check(!disabledActiveJournal->IsActive() && disabledActiveJournal->IsDisabled(),
        "Disable leaves journal inactive and disabled");
    disabledActiveJournal->Release();

    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
    auto* destructionJournal = new ProperShadersStateJournal(device);
    hr = destructionJournal->BeginTransaction(GetCurrentThreadId(), 0);
    if (SUCCEEDED(hr)) hr = destructionJournal->SetRenderState(
        D3DRS_CULLMODE, D3DCULL_NONE);
    destructionJournal->Release();
    device->GetRenderState(D3DRS_CULLMODE, &value);
    ok &= Check(SUCCEEDED(hr) && value == D3DCULL_CCW,
        "destruction restores an active transaction");

    if (requireNativeJournal) {
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
        auto* crossThreadDestructionJournal = new ProperShadersStateJournal(device);
        HRESULT crossThreadHr = crossThreadDestructionJournal->BeginTransaction(
            GetCurrentThreadId(), 0);
        if (SUCCEEDED(crossThreadHr)) {
            crossThreadHr = crossThreadDestructionJournal->SetRenderState(
                D3DRS_CULLMODE, D3DCULL_NONE);
        }

        HANDLE releaseThread = nullptr;
        if (SUCCEEDED(crossThreadHr)) {
            releaseThread = CreateThread(
                nullptr, 0, ReleaseJournalOnWorkerThread,
                crossThreadDestructionJournal, 0, nullptr);
            if (!releaseThread) crossThreadHr = HRESULT_FROM_WIN32(GetLastError());
        }
        if (releaseThread) {
            WaitForSingleObject(releaseThread, INFINITE);
            CloseHandle(releaseThread);
        } else {
            crossThreadDestructionJournal->Release();
        }

        DWORD crossThreadRestoredCullMode = 0;
        device->GetRenderState(D3DRS_CULLMODE, &crossThreadRestoredCullMode);
        ID3D9GtaSaCompatDevice3* reusableNativeJournal = nullptr;
        HRESULT nativeReuseHr = device->QueryInterface(
            __uuidof(ID3D9GtaSaCompatDevice3),
            reinterpret_cast<void**>(&reusableNativeJournal));
        if (SUCCEEDED(nativeReuseHr) && reusableNativeJournal) {
            nativeReuseHr = reusableNativeJournal->BeginStateJournal();
            if (SUCCEEDED(nativeReuseHr)) {
                nativeReuseHr = reusableNativeJournal->RestoreStateJournal();
            } else {
                reusableNativeJournal->RestoreStateJournal();
            }
            reusableNativeJournal->Release();
        }
        ok &= Check(SUCCEEDED(crossThreadHr) &&
                crossThreadRestoredCullMode == D3DCULL_CCW &&
                SUCCEEDED(nativeReuseHr),
            "cross-thread destruction restores and releases the native journal");
    }

    // Regression test for observation-only attribution cleanup. Restore must
    // flush the transaction even when the caller does not explicitly flush it;
    // this is the path used by fallback, disable, and teardown handling.
    JournalAttributionStartCapture();
    auto* attributionJournal = new ProperShadersStateJournal(device);
    HRESULT attributionBeginHr = attributionJournal->BeginTransaction(
        GetCurrentThreadId(), 0);
    if (SUCCEEDED(attributionBeginHr)) {
        attributionJournal->SetCurrentPass(7);
        attributionBeginHr = attributionJournal->SetRenderState(
            D3DRS_CULLMODE, D3DCULL_NONE);
    }
    const HRESULT attributionRestoreHr = attributionJournal->Restore();
    JournalAttributionStopCapture();
    ok &= Check(SUCCEEDED(attributionBeginHr) && SUCCEEDED(attributionRestoreHr),
        "attribution transaction restores without explicit flush");
    ok &= Check(g_attributionTransactions == 1 &&
            g_attributionLastRecordCount == 1,
        "attribution publishes exactly one transaction and record");
    ok &= Check(g_attributionRestores == 1 && g_attributionEnds == 1,
        "attribution publishes restore timing and ends in-flight transaction");
    ok &= Check(g_attributionFirstRecord.pass == 7 &&
            g_attributionFirstRecord.hash == 0,
        "attribution records the current pass without hashing values");
    attributionJournal->Release();

    journal->Disable();
    ok &= Check(FAILED(journal->BeginTransaction(GetCurrentThreadId(), 0)),
        "disabled journal rejects new transaction");
    journal->Release();

    device->SetTexture(0, nullptr);
    device->SetTexture(1, nullptr);
    if (textureA) textureA->Release();
    if (textureB) textureB->Release();

    if (requireNativeJournal) {
        auto* resetJournal = new ProperShadersStateJournal(device);
        hr = resetJournal->BeginTransaction(GetCurrentThreadId(), 0);
        if (SUCCEEDED(hr)) hr = resetJournal->SetRenderState(
            D3DRS_CULLMODE, D3DCULL_NONE);
        const HRESULT resetHr = SUCCEEDED(hr) ? device->Reset(&parameters) : hr;
        const HRESULT resetRestoreHr = SUCCEEDED(resetHr)
            ? resetJournal->Restore()
            : resetHr;
        ok &= Check(SUCCEEDED(resetHr), "device reset during native journal");
        ok &= Check(FAILED(resetRestoreHr) && resetJournal->HasFailed(),
            "native restore failure after reset is reported");
        resetJournal->Release();
    }
    device->Release();
    d3d->Release();
    FreeLibrary(loaded.module);
    DestroyWindow(window);

    if (!ok) return 1;
    std::printf("PASS: ProperShaders %s state journal restored D3D9 state%s\n",
        requireNativeJournal ? "native" : "local",
        stateBatchTested ? " including API v3 batch" : "");
    return 0;
}
