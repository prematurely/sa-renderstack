#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cstring>
#include <string>
#include "lib/win32-import-hook.h"
#include "lib/benchmark-thread-affinity.h"
#include "patch-retry-sites.h"
#include "../backend/dxvk/src/d3d9/d3d9_bridge_lifecycle.h"

static unsigned checks;
static unsigned failures;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; std::printf("FAIL line=%u %s\n", __LINE__, #condition); } } while (0)
static decltype(&GetModuleHandleExA) originalPin;
static HMODULE fixtureModule;
static unsigned pinAttempts;
static bool failFirstPin;
static DWORD ownerThread;
static void RestoreFixtureSignatures() {
    for (const auto& patch : fixtureCallSites) {
        auto site = reinterpret_cast<unsigned char*>(fixtureModule) + patch.rva;
        DWORD old = 0, ignored = 0;
        CHECK(VirtualProtect(site, sizeof(patch.bytes), PAGE_EXECUTE_READWRITE, &old));
        std::memcpy(site, patch.bytes, sizeof(patch.bytes));
        CHECK(VirtualProtect(site, sizeof(patch.bytes), old, &ignored));
    }
}

static BOOL WINAPI ObservePin(DWORD flags, LPCSTR address, HMODULE* result) {
    if ((flags & GET_MODULE_HANDLE_EX_FLAG_PIN) &&
        address == reinterpret_cast<LPCSTR>(fixtureModule) && GetCurrentThreadId() == ownerThread) {
        ++pinAttempts;
        if (failFirstPin && pinAttempts == 1) {
            SetLastError(ERROR_NOT_READY);
            return FALSE;
        }
    }
    return originalPin(flags, address, result);
}

static void Present(IDirect3DDevice9* device, unsigned frames) {
    for (unsigned i = 0; i < frames; ++i)
        CHECK(device->Present(nullptr, nullptr, nullptr, nullptr) == S_OK);
}

static double ClockMilliseconds() {
    LARGE_INTEGER ticks, frequency;
    QueryPerformanceCounter(&ticks);
    QueryPerformanceFrequency(&frequency);
    return double(ticks.QuadPart) * 1000 / frequency.QuadPart;
}

static double ThreadCpuMilliseconds() {
    FILETIME created, exited, kernel, user;
    CHECK(GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user));
    ULARGE_INTEGER k{}, u{};
    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;
    return double(k.QuadPart + u.QuadPart) / 10000;
}

int main(int argc, char** argv) {
    if (argc != 4)
        return 2;
    ownerThread = GetCurrentThreadId();
    failFirstPin = !std::strcmp(argv[3], "transient");
    const bool recover = !std::strcmp(argv[3], "restore");
    const bool benchmark = !std::strcmp(argv[3], "bench");
    fixtureModule = LoadLibraryA(argv[2]);
    CHECK(fixtureModule != nullptr);
    HMODULE bridge = LoadLibraryA(argv[1]);
    CHECK(bridge != nullptr);
    if (!bridge || !fixtureModule)
        return 1;
    {
        ImportHook pin;
        CHECK(pin.install(bridge, "GetModuleHandleExA", reinterpret_cast<void*>(&ObservePin)));
        originalPin = reinterpret_cast<decltype(originalPin)>(pin.original);
        if (!originalPin)
            return 1;
        using Create = IDirect3D9* (WINAPI*)(UINT);
        const auto create = reinterpret_cast<Create>(GetProcAddress(bridge, "Direct3DCreate9"));
        CHECK(create != nullptr);
        IDirect3D9* factory = create(D3D_SDK_VERSION);
        CHECK(factory != nullptr);
        WNDCLASSA wc{};
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "RenderStackPatchRetry";
        RegisterClassA(&wc);
        HWND window = CreateWindowA(wc.lpszClassName, "retry", WS_OVERLAPPEDWINDOW,
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
            unsigned char before[6];
            auto site = reinterpret_cast<unsigned char*>(fixtureModule) + 0x12dc9;
            std::memcpy(before, site, sizeof(before));
            Present(device, 16);
            if (!benchmark)
                CHECK(pinAttempts == (failFirstPin ? 2u : 1u));
            CHECK(std::memcmp(before, site, sizeof(before)) == 0);
            if (benchmark) {
                BenchmarkThreadAffinity affinity;
                CHECK(affinity.valid());
                const auto cpuStart = ThreadCpuMilliseconds();
                ULONG64 cyclesBefore = 0, cyclesAfter = 0;
                CHECK(QueryThreadCycleTime(GetCurrentThread(), &cyclesBefore));
                const auto start = ClockMilliseconds();
                Present(device, 2000);
                const auto elapsed = ClockMilliseconds() - start;
                const auto cpu = ThreadCpuMilliseconds() - cpuStart;
                CHECK(QueryThreadCycleTime(GetCurrentThread(), &cyclesAfter));
                std::printf("benchmark presents=2000 elapsedMs=%.6f mainCpuMs=%.6f mainCycles=%llu\n",
                    elapsed, cpu, static_cast<unsigned long long>(cyclesAfter - cyclesBefore));
            }
            if (recover) {
                RestoreFixtureSignatures();
                Present(device, 1);
                CHECK(pinAttempts == 2u);
                CHECK(site[0] == 0xe8 && site[5] == 0x90);
            }
            CHECK(device->Reset(&params) == S_OK);
            Present(device, 8);
            if (!benchmark)
                CHECK(pinAttempts == (recover || failFirstPin ? 2u : 1u));
            CHECK(device->Release() == 0);
        }
        ID3D9BridgeRuntime* runtime = nullptr;
        CHECK(factory->QueryInterface(IID_D3D9BridgeRuntime, reinterpret_cast<void**>(&runtime)) == S_OK);
        if (runtime) {
            CHECK(runtime->Shutdown() == S_OK);
            CHECK(runtime->Shutdown() == S_FALSE);
            runtime->Release();
        }
        if (recover) {
            for (const auto& patch : fixtureCallSites)
                CHECK(std::memcmp(reinterpret_cast<unsigned char*>(fixtureModule) + patch.rva,
                    patch.bytes, sizeof(patch.bytes)) == 0);
        }
        factory->Release();
        DestroyWindow(window);
    }
    std::printf("pinAttempts=%u transient=%u checks=%u failures=%u\n",
        pinAttempts, unsigned(failFirstPin), checks, failures);
    FreeLibrary(bridge);
    FreeLibrary(fixtureModule);
    return failures ? 1 : 0;
}
