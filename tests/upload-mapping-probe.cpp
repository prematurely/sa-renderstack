#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "lib/win32-import-hook.h"
#include "lib/benchmark-thread-affinity.h"

static unsigned checks;
static unsigned failures;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; std::printf("FAIL line=%u %s\n", __LINE__, #condition); } } while (0)

using MapView = decltype(&MapViewOfFile);
using UnmapView = decltype(&UnmapViewOfFile);
static MapView originalMap;
static UnmapView originalUnmap;
static DWORD ownerThread;
static bool countCalls;
static unsigned maps;
static unsigned unmaps;
static void* coldView;
static bool coldViewReclaimed;
static const unsigned char* uploadView;
static SIZE_T uploadViewBytes;
static unsigned copiesAfterColdReclaim;
using CopyMemory = void* (__cdecl*)(void*, const void*, size_t);
static CopyMemory originalCopy;

static LPVOID WINAPI CountMap(HANDLE file, DWORD access, DWORD high, DWORD low, SIZE_T size) {
    auto result = originalMap(file, access, high, low, size);
    if (GetCurrentThreadId() == ownerThread && countCalls && result)
        ++maps;
    if (GetCurrentThreadId() == ownerThread && countCalls && coldView && result) {
        uploadView = static_cast<const unsigned char*>(result);
        uploadViewBytes = size;
    }
    return result;
}

static BOOL WINAPI CountUnmap(LPCVOID address) {
    const auto result = originalUnmap(address);
    if (GetCurrentThreadId() == ownerThread && countCalls && result)
        ++unmaps;
    if (GetCurrentThreadId() == ownerThread && countCalls && address == coldView && result)
        coldViewReclaimed = true;
    return result;
}

static void* __cdecl CountCopy(void* destination, const void* source, size_t bytes) {
    if (GetCurrentThreadId() == ownerThread && countCalls && coldViewReclaimed && bytes >= 4096) {
        const auto address = reinterpret_cast<uintptr_t>(source);
        const auto first = reinterpret_cast<uintptr_t>(uploadView);
        if (address >= first && address - first < uploadViewBytes)
            ++copiesAfterColdReclaim;
    }
    return originalCopy(destination, source, bytes);
}

static double Milliseconds() {
    LARGE_INTEGER tick, frequency;
    QueryPerformanceCounter(&tick);
    QueryPerformanceFrequency(&frequency);
    return double(tick.QuadPart) * 1000 / double(frequency.QuadPart);
}

static void Flush(IDirect3DDevice9* device) {
    IDirect3DQuery9* query = nullptr;
    CHECK(device->CreateQuery(D3DQUERYTYPE_EVENT, &query) == S_OK);
    if (!query)
        return;
    CHECK(query->Issue(D3DISSUE_END) == S_OK);
    HRESULT result = S_FALSE;
    const auto deadline = GetTickCount64() + 15000;
    while (result == S_FALSE && GetTickCount64() < deadline)
        result = query->GetData(nullptr, 0, D3DGETDATA_FLUSH);
    CHECK(result == S_OK);
    query->Release();
}

static DWORD Color(unsigned level, unsigned version) {
    return 0xff000000u | ((17 + level * 11 + version * 23) << 16)
        | ((31 + level * 7 + version * 19) << 8) | (47 + level * 3 + version * 13);
}

static void Fill(IDirect3DTexture9* texture, unsigned version) {
    for (unsigned level = 0; level < texture->GetLevelCount(); ++level) {
        D3DSURFACE_DESC desc{};
        CHECK(texture->GetLevelDesc(level, &desc) == S_OK);
        D3DLOCKED_RECT lock{};
        CHECK(texture->LockRect(level, &lock, nullptr, 0) == S_OK);
        if (!lock.pBits)
            return;
        for (unsigned row = 0; row < desc.Height; ++row)
            std::fill_n(reinterpret_cast<DWORD*>(static_cast<char*>(lock.pBits) + row * lock.Pitch),
                        desc.Width, Color(level, version));
        CHECK(texture->UnlockRect(level) == S_OK);
    }
}

struct Vertex {
    float x, y, z, rhw, u, v;
};

static void Pixels(IDirect3DDevice9* device, IDirect3DTexture9* texture, unsigned version, bool partial = false) {
    IDirect3DSurface9* target = nullptr;
    IDirect3DSurface9* readback = nullptr;
    CHECK(device->GetRenderTarget(0, &target) == S_OK);
    CHECK(device->CreateOffscreenPlainSurface(32, 32, D3DFMT_A8R8G8B8,
        D3DPOOL_SYSTEMMEM, &readback, nullptr) == S_OK);
    if (!target || !readback)
        return;
    CHECK(device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1) == S_OK);
    CHECK(device->SetRenderState(D3DRS_ZENABLE, FALSE) == S_OK);
    CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE) == S_OK);
    CHECK(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1) == S_OK);
    CHECK(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE) == S_OK);
    CHECK(device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1) == S_OK);
    CHECK(device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE) == S_OK);
    CHECK(device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT) == S_OK);
    CHECK(device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT) == S_OK);
    CHECK(device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_POINT) == S_OK);
    CHECK(device->SetSamplerState(0, D3DSAMP_MIPMAPLODBIAS, 0xc1800000u) == S_OK); // -16.0f
    CHECK(device->SetTexture(0, texture) == S_OK);
    const Vertex vertices[] = {{0,0,.5f,1,0,0},{64,0,.5f,1,2,0},{0,64,.5f,1,0,2}};
    for (unsigned level = 0; level < texture->GetLevelCount(); ++level) {
        CHECK(device->SetSamplerState(0, D3DSAMP_MAXMIPLEVEL, level) == S_OK);
        CHECK(device->BeginScene() == S_OK);
        CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, vertices, sizeof(Vertex)) == S_OK);
        CHECK(device->EndScene() == S_OK);
        CHECK(device->GetRenderTargetData(target, readback) == S_OK);
        D3DLOCKED_RECT lock{};
        CHECK(readback->LockRect(&lock, nullptr, D3DLOCK_READONLY) == S_OK);
        if (lock.pBits) {
            const DWORD expected = partial && level == 0 ? 0xff112233 : Color(level, version);
            CHECK(*reinterpret_cast<DWORD*>(static_cast<char*>(lock.pBits) + 8 * lock.Pitch + 8 * 4)
                == expected);
            // A sample outside the modified rectangle must retain its original content.
            CHECK(*reinterpret_cast<DWORD*>(static_cast<char*>(lock.pBits) + 8 * lock.Pitch + 24 * 4)
                == Color(level, version));
            CHECK(readback->UnlockRect() == S_OK);
        }
    }
    CHECK(device->SetTexture(0, nullptr) == S_OK);
    target->Release();
    readback->Release();
}

static void Upload(IDirect3DTexture9* source, IDirect3DTexture9* destination, IDirect3DDevice9* device) {
    if (destination)
        CHECK(device->UpdateTexture(source, destination) == S_OK);
    else
        source->PreLoad();
}

static void Exercise(IDirect3DDevice9* device, bool managed, bool benchmark, bool observe,
                     unsigned budget, unsigned levels, unsigned iterations) {
    IDirect3DTexture9* source = nullptr;
    IDirect3DTexture9* destination = nullptr;
    CHECK(device->CreateTexture(1024, 1024, levels, 0, D3DFMT_A8R8G8B8,
        managed ? D3DPOOL_MANAGED : D3DPOOL_SYSTEMMEM, &source, nullptr) == S_OK);
    if (!managed)
        CHECK(device->CreateTexture(1024, 1024, levels, 0, D3DFMT_A8R8G8B8,
            D3DPOOL_DEFAULT, &destination, nullptr) == S_OK);
    if (!source || (!managed && !destination))
        return;
    Fill(source, 0);
    Flush(device);
    maps = unmaps = 0;
    countCalls = true;
    Upload(source, destination, device);
    countCalls = false;
    std::printf("mapping case=%s levels=%u maps=%u unmaps=%u budgetMiB=%u\n",
        managed ? "managed" : "systemmem", static_cast<unsigned>(source->GetLevelCount()), maps, unmaps, budget);
    if (!benchmark && !observe && budget == 1) {
        // One source mapping must survive the complete mip-chain transaction.
        CHECK(maps == 1);
        // The oversized source must still be reclaimed at the transaction end.
        CHECK(unmaps == 1);
    }
    if (!benchmark) {
        Pixels(device, destination ? destination : source, 0);
        Fill(source, 1);
        CHECK(source->AddDirtyRect(nullptr) == S_OK);
        Upload(source, destination, device);
        Pixels(device, destination ? destination : source, 1);
        const RECT rectangle{0, 0, 512, 512};
        D3DLOCKED_RECT lock{};
        CHECK(source->LockRect(0, &lock, &rectangle, 0) == S_OK);
        if (lock.pBits) {
            for (unsigned y = 0; y < 512; ++y)
                std::fill_n(reinterpret_cast<DWORD*>(static_cast<char*>(lock.pBits) + y * lock.Pitch),
                            512, 0xff112233);
            CHECK(source->UnlockRect(0) == S_OK);
        }
        Upload(source, destination, device);
        Pixels(device, destination ? destination : source, 1, true);
        if (managed) {
            D3DPRESENT_PARAMETERS params{};
            IDirect3DSwapChain9* swap = nullptr;
            CHECK(device->GetSwapChain(0, &swap) == S_OK);
            if (swap) {
                CHECK(swap->GetPresentParameters(&params) == S_OK);
                swap->Release();
                Flush(device);
                CHECK(device->Reset(&params) == S_OK);
                source->PreLoad();
                Pixels(device, source, 1, true);
            }
        }
    } else {
        // Warm driver clocks and upload throttling before the measured batch.
        for (unsigned i = 0; i < 64; ++i) {
            CHECK(source->AddDirtyRect(nullptr) == S_OK);
            Upload(source, destination, device);
        }
        Flush(device);
        BenchmarkThreadAffinity affinity;
        CHECK(affinity.valid());
        maps = unmaps = 0;
        countCalls = true;
        ULONG64 cyclesBefore = 0, cyclesAfter = 0;
        CHECK(QueryThreadCycleTime(GetCurrentThread(), &cyclesBefore));
        const auto start = Milliseconds();
        for (unsigned i = 0; i < iterations; ++i) {
            CHECK(source->AddDirtyRect(nullptr) == S_OK);
            Upload(source, destination, device);
        }
        const double elapsed = Milliseconds() - start;
        CHECK(QueryThreadCycleTime(GetCurrentThread(), &cyclesAfter));
        countCalls = false;
        std::printf("benchmark case=%s levels=%u iterations=%u elapsedMs=%.6f mainCycles=%llu maps=%u unmaps=%u budgetMiB=%u\n",
            managed ? "managed" : "systemmem", static_cast<unsigned>(source->GetLevelCount()),
            iterations, elapsed, static_cast<unsigned long long>(cyclesAfter - cyclesBefore), maps, unmaps, budget);
        Flush(device);
    }
    if (destination)
        destination->Release();
    source->Release();
    Flush(device);
}

static void CubeFaces(IDirect3DDevice9* device, bool observe, unsigned budget) {
    IDirect3DCubeTexture9* source = nullptr;
    IDirect3DCubeTexture9* destination = nullptr;
    CHECK(device->CreateCubeTexture(512, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &source, nullptr) == S_OK);
    CHECK(device->CreateCubeTexture(512, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &destination, nullptr) == S_OK);
    if (!source || !destination)
        return;
    for (unsigned face = 0; face < 6; ++face) {
        for (unsigned level = 0; level < source->GetLevelCount(); ++level) {
            D3DSURFACE_DESC desc{};
            CHECK(source->GetLevelDesc(level, &desc) == S_OK);
            D3DLOCKED_RECT lock{};
            CHECK(source->LockRect(static_cast<D3DCUBEMAP_FACES>(face), level, &lock, nullptr, 0) == S_OK);
            if (lock.pBits) {
                for (unsigned y = 0; y < desc.Height; ++y)
                    std::fill_n(reinterpret_cast<DWORD*>(static_cast<char*>(lock.pBits) + y * lock.Pitch),
                                desc.Width, Color(level, face));
                CHECK(source->UnlockRect(static_cast<D3DCUBEMAP_FACES>(face), level) == S_OK);
            }
        }
    }
    Flush(device);
    maps = unmaps = 0;
    countCalls = true;
    CHECK(device->UpdateTexture(source, destination) == S_OK);
    countCalls = false;
    std::printf("mapping case=cube maps=%u unmaps=%u budgetMiB=%u\n", maps, unmaps, budget);
    if (!observe && budget == 1) {
        CHECK(maps == 1);
        CHECK(unmaps == 1);
    }
    IDirect3DSurface9* target = nullptr;
    IDirect3DSurface9* readback = nullptr;
    CHECK(device->GetRenderTarget(0, &target) == S_OK);
    CHECK(device->CreateOffscreenPlainSurface(32, 32, D3DFMT_A8R8G8B8,
        D3DPOOL_SYSTEMMEM, &readback, nullptr) == S_OK);
    for (unsigned face = 0; face < 6; ++face) {
        for (unsigned level = 0; level < destination->GetLevelCount(); ++level) {
            IDirect3DSurface9* surface = nullptr;
            CHECK(destination->GetCubeMapSurface(static_cast<D3DCUBEMAP_FACES>(face), level, &surface) == S_OK);
            CHECK(device->StretchRect(surface, nullptr, target, nullptr, D3DTEXF_POINT) == S_OK);
            surface->Release();
            CHECK(device->GetRenderTargetData(target, readback) == S_OK);
            D3DLOCKED_RECT lock{};
            CHECK(readback->LockRect(&lock, nullptr, D3DLOCK_READONLY) == S_OK);
            if (lock.pBits) {
                CHECK(*reinterpret_cast<DWORD*>(lock.pBits) == Color(level, face));
                CHECK(readback->UnlockRect() == S_OK);
            }
        }
    }
    readback->Release();
    target->Release();
    destination->Release();
    source->Release();
    Flush(device);
}

static void ReclaimOtherMappings(IDirect3DDevice9* device) {
    IDirect3DTexture9* source = nullptr;
    IDirect3DTexture9* cold = nullptr;
    CHECK(device->CreateTexture(1024, 1024, 0, 0, D3DFMT_A8R8G8B8,
        D3DPOOL_MANAGED, &source, nullptr) == S_OK);
    CHECK(device->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8,
        D3DPOOL_SYSTEMMEM, &cold, nullptr) == S_OK);
    Fill(source, 0);
    D3DLOCKED_RECT lock{};
    CHECK(cold->LockRect(0, &lock, nullptr, 0) == S_OK);
    MEMORY_BASIC_INFORMATION memory{};
    CHECK(VirtualQuery(lock.pBits, &memory, sizeof(memory)) == sizeof(memory));
    coldView = memory.AllocationBase;
    CHECK(cold->UnlockRect(0) == S_OK);
    coldViewReclaimed = false;
    uploadView = nullptr;
    uploadViewBytes = 0;
    copiesAfterColdReclaim = 0;
    countCalls = true;
    source->PreLoad();
    countCalls = false;
    CHECK(coldViewReclaimed);
    // Other cold mappings must be reclaimed before later mips are copied.
    CHECK(copiesAfterColdReclaim > 0);
    coldView = nullptr;
    cold->Release();
    source->Release();
    Flush(device);
}

int main(int argc, char** argv) {
    if (argc != 5 && argc != 6)
        return 2;
    ownerThread = GetCurrentThreadId();
    const bool benchmark = !std::strcmp(argv[2], "bench");
    const bool observe = !std::strcmp(argv[2], "observe");
    const unsigned budget = std::strtoul(argv[3], nullptr, 10);
    const unsigned levels = std::strtoul(argv[4], nullptr, 10);
    const unsigned iterations = argc == 6 ? std::strtoul(argv[5], nullptr, 10) : 64;
    const std::string config = "d3d9.textureMemory=" + std::to_string(budget)
        + ";d3d9.presentInterval=0;d3d9.gtaSaCompat=False;dxvk.hud=";
    SetEnvironmentVariableA("DXVK_CONFIG", config.c_str());
    SetEnvironmentVariableA("DXVK_CONFIG_FILE", "upload-mapping-no-external.conf");
    SetEnvironmentVariableA("DXVK_LOG_LEVEL", "error");
    HMODULE backend = LoadLibraryA(argv[1]);
    CHECK(backend != nullptr);
    if (!backend)
        return 1;
    using Create = IDirect3D9* (WINAPI*)(UINT);
    const auto create = reinterpret_cast<Create>(GetProcAddress(backend, "Direct3DCreate9"));
    CHECK(create != nullptr);
    if (!create)
        return 1;
    {
        ImportHook mapHook, unmapHook, copyHook;
        CHECK(mapHook.install(backend, "MapViewOfFile", reinterpret_cast<void*>(&CountMap)));
        CHECK(unmapHook.install(backend, "UnmapViewOfFile", reinterpret_cast<void*>(&CountUnmap)));
        originalMap = reinterpret_cast<MapView>(mapHook.original);
        originalUnmap = reinterpret_cast<UnmapView>(unmapHook.original);
        if (!originalMap || !originalUnmap)
            return 1;
        if (!benchmark) {
            CHECK(copyHook.install(backend, "memcpy", reinterpret_cast<void*>(&CountCopy)));
            originalCopy = reinterpret_cast<CopyMemory>(copyHook.original);
            if (!originalCopy)
                return 1;
        }
        IDirect3D9* factory = create(D3D_SDK_VERSION);
        CHECK(factory != nullptr);
        WNDCLASSA wc{};
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "RenderStackUploadMapping";
        RegisterClassA(&wc);
        HWND window = CreateWindowA(wc.lpszClassName, "mapping", WS_OVERLAPPEDWINDOW,
            0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
        D3DPRESENT_PARAMETERS params{};
        params.BackBufferWidth = params.BackBufferHeight = 32;
        params.BackBufferFormat = D3DFMT_A8R8G8B8;
        params.BackBufferCount = 1;
        params.SwapEffect = D3DSWAPEFFECT_DISCARD;
        params.Windowed = TRUE;
        params.hDeviceWindow = window;
        params.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        IDirect3DDevice9* device = nullptr;
        CHECK(factory->CreateDevice(0, D3DDEVTYPE_HAL, window,
            D3DCREATE_HARDWARE_VERTEXPROCESSING, &params, &device) == S_OK);
        if (device) {
            Exercise(device, true, benchmark, observe, budget, levels, iterations);
            Exercise(device, false, benchmark, observe, budget, levels, iterations);
            if (!benchmark)
                CubeFaces(device, observe, budget);
            if (!benchmark && budget == 1)
                ReclaimOtherMappings(device);
            CHECK(device->Reset(&params) == S_OK);
            CHECK(device->Release() == 0);
        }
        factory->Release();
        DestroyWindow(window);
    }
    FreeLibrary(backend);
    std::printf("checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
