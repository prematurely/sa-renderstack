#include <windows.h>
#include <d3d9.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>
#include <ProperShadersRenderStateGuard.h>

// Only the external GTA addresses and observation APIs are supplied by the
// fixture. All device methods below forward to an actual HAL device.
static uintptr_t fixtureSetState, fixtureGetState, fixtureDeviceCell;
static thread_local unsigned long long queryCalls;
static unsigned snapshotAttempts;
static SIZE_T WINAPI ObservedVirtualQuery(LPCVOID p, PMEMORY_BASIC_INFORMATION m, SIZE_T n) {
    ++queryCalls; return ::VirtualQuery(p, m, n);
}
#define kRwD3D9SetRenderState fixtureSetState
#define kRwD3D9GetRenderState fixtureGetState
#define kRwD3D9DevicePointer fixtureDeviceCell
#define VirtualQuery ObservedVirtualQuery
#define CaptureProperShadersD3D9StateSnapshot ObservedSnapshotImplementation
#include <ProperShadersRenderStateGuard.cpp>
#undef CaptureProperShadersD3D9StateSnapshot
#undef VirtualQuery
#undef kRwD3D9SetRenderState
#undef kRwD3D9GetRenderState
#undef kRwD3D9DevicePointer
bool CaptureProperShadersD3D9StateSnapshot(ProperShadersD3D9StateSnapshot* s) {
    ++snapshotAttempts; return ObservedSnapshotImplementation(s);
}

static ULONGLONG fixtureTick = 100;
static bool fixtureKey;
static ULONGLONG WINAPI FixtureTick() { return fixtureTick; }
static SHORT WINAPI FixtureKey(int) { return fixtureKey ? SHORT(0x8000) : 0; }
#define GetTickCount64 FixtureTick
#define GetAsyncKeyState FixtureKey
#define Log RingFixtureLog
#include <ProperShadersStateRing.cpp>
#undef Log
#undef GetAsyncKeyState
#undef GetTickCount64

namespace {
unsigned checks, failures;
void Check(bool ok, const char* text) { ++checks; if (!ok) { ++failures; std::printf("FAIL %s\n", text); } }
struct Proxy { void** vtable; IDirect3DDevice9* real; };
static std::array<DWORD, 256> rwStates;
static unsigned textureGets, setVersion1, setVersion2;
void __cdecl RwGet(DWORD state, void* out) { *static_cast<DWORD*>(out) = rwStates.at(state); }
void __cdecl RwSet(DWORD state, DWORD value) { rwStates.at(state) = value; }
HRESULT WINAPI SetState(Proxy* p, DWORD s, DWORD v) { ++setVersion1; return p->real->SetRenderState(D3DRENDERSTATETYPE(s), v); }
HRESULT WINAPI SetState2(Proxy* p, DWORD s, DWORD v) { ++setVersion2; return p->real->SetRenderState(D3DRENDERSTATETYPE(s), v); }
HRESULT WINAPI GetState(Proxy* p, DWORD s, DWORD* v) { return p->real->GetRenderState(D3DRENDERSTATETYPE(s), v); }
HRESULT WINAPI GetTarget(Proxy* p, DWORD i, void** v) { return p->real->GetRenderTarget(i, reinterpret_cast<IDirect3DSurface9**>(v)); }
HRESULT WINAPI GetDepth(Proxy* p, void** v) { return p->real->GetDepthStencilSurface(reinterpret_cast<IDirect3DSurface9**>(v)); }
HRESULT WINAPI GetView(Proxy* p, ProperShadersD3D9Viewport* v) { return p->real->GetViewport(reinterpret_cast<D3DVIEWPORT9*>(v)); }
HRESULT WINAPI GetTexture(Proxy* p, DWORD i, void** v) { ++textureGets; return p->real->GetTexture(i, reinterpret_cast<IDirect3DBaseTexture9**>(v)); }
HRESULT WINAPI GetStage(Proxy* p, DWORD i, DWORD s, DWORD* v) { return p->real->GetTextureStageState(i, D3DTEXTURESTAGESTATETYPE(s), v); }
HRESULT WINAPI GetSampler(Proxy* p, DWORD i, DWORD s, DWORD* v) { return p->real->GetSamplerState(i, D3DSAMPLERSTATETYPE(s), v); }
HRESULT WINAPI GetVS(Proxy* p, void** v) { return p->real->GetVertexShader(reinterpret_cast<IDirect3DVertexShader9**>(v)); }
HRESULT WINAPI GetPS(Proxy* p, void** v) { return p->real->GetPixelShader(reinterpret_cast<IDirect3DPixelShader9**>(v)); }
HRESULT WINAPI WrongShaderGetter(Proxy*, void**) { return D3D_OK; }
void* deviceCell;
Proxy* current;
void __cdecl SceneBody() {
    current->real->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    current->real->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_LESS);
    current->real->SetRenderState(D3DRS_ALPHAREF, 0);
    current->real->SetRenderState(D3DRS_STENCILENABLE, TRUE);
}
void Boundary() {
#if !defined(BASELINE_SOURCE)
    InvalidateProperShadersDeviceApiCache();
#endif
}
struct Device {
    HMODULE library = nullptr;
    IDirect3D9* d3d = nullptr;
    IDirect3DDevice9* real = nullptr;
    IDirect3DTexture9* texture = nullptr;
    HWND window = nullptr;
    D3DPRESENT_PARAMETERS pp{};
    std::array<void*, 119> table{}, secondTable{};
    Proxy proxy{}, secondProxy{};
    explicit Device(const char* path) {
        if (!path) { static char system[MAX_PATH]; GetSystemDirectoryA(system, MAX_PATH); std::strcat(system, "\\d3d9.dll"); path = system; }
        library = LoadLibraryExA(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        auto create = library ? reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(library, "Direct3DCreate9")) : nullptr;
        d3d = create ? create(D3D_SDK_VERSION) : nullptr;
        window = CreateWindowA("STATIC", "StateRing fixture", WS_OVERLAPPEDWINDOW, 0, 0, 96, 96, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
        pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = window;
        pp.BackBufferWidth = pp.BackBufferHeight = 64; pp.BackBufferFormat = D3DFMT_A8R8G8B8;
        pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        if (d3d) d3d->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED, &pp, &real);
        if (!real) return;
        table[57] = reinterpret_cast<void*>(&SetState); table[58] = reinterpret_cast<void*>(&GetState);
        table[38] = reinterpret_cast<void*>(&GetTarget); table[40] = reinterpret_cast<void*>(&GetDepth);
        table[48] = reinterpret_cast<void*>(&GetView); table[64] = reinterpret_cast<void*>(&GetTexture);
        table[66] = reinterpret_cast<void*>(&GetStage); table[68] = reinterpret_cast<void*>(&GetSampler);
        table[93] = reinterpret_cast<void*>(&GetVS); table[108] = reinterpret_cast<void*>(&GetPS);
        secondTable = table; proxy = {table.data(), real}; secondProxy = {secondTable.data(), real};
        current = &proxy; deviceCell = current;
        real->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr);
        real->SetTexture(0, texture);
        real->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
        real->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
        real->SetRenderState(D3DRS_ALPHAREF, 128);
        real->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        for (auto s : kProperShadersAlphaRenderStates) real->GetRenderState(D3DRENDERSTATETYPE(s), &rwStates[s]);
        fixtureSetState = uintptr_t(&RwSet); fixtureGetState = uintptr_t(&RwGet);
        fixtureDeviceCell = uintptr_t(&deviceCell);
        g_properShadersRenderStateGuardConfig.enable = true;
        g_properShadersRenderStateGuardConfig.enableStateRing = true;
        g_properShadersRenderStateGuardApiReady = 1;
        g_properShadersPatchedRenderSceneTrampoline = uintptr_t(&SceneBody);
        Boundary();
    }
    ~Device() {
        g_properShadersPatchedRenderSceneTrampoline = 0;
        ShutdownProperShadersStateRing(); Boundary(); deviceCell = nullptr;
        if (real) real->SetTexture(0, nullptr);
        if (texture) texture->Release();
        if (real) real->Release();
        if (d3d) d3d->Release();
        if (library) FreeLibrary(library);
        if (window) DestroyWindow(window);
    }
};
void VerifyCorrected(Device& d) {
    DWORD value = 0;
    for (auto [state, expected] : {std::pair{D3DRS_ALPHATESTENABLE, 1u}, {D3DRS_ALPHAFUNC, DWORD(D3DCMP_GREATER)}, {D3DRS_ALPHAREF, 128u}, {D3DRS_STENCILENABLE, 0u}}) {
        Check(SUCCEEDED(d.real->GetRenderState(state, &value)) && value == expected, "guard preserves actual alpha/stencil restoration");
    }
}
void NativeBehavior(Device& d) {
    D3D9StateTraceApi traceApi{};
    const bool traceAvailable = ResolveD3D9StateTraceApi(&traceApi);
    std::printf("actual trace API available=%u shaderValidated=%ld shaderRejected=%ld textureGetter=%p\n",
        unsigned(traceAvailable), g_properShadersShaderSlotsValidated, g_properShadersShaderSlotsRejected,
        reinterpret_cast<void*>(traceApi.getTexture));
    for (auto slot : {38u, 40u, 48u, 57u, 58u, 64u, 66u, 68u, 93u, 108u})
        Check(IsExecutableCommitted(uintptr_t(d.table[slot])), "fixture forwarding method is executable");
    textureGets = snapshotAttempts = 0; fixtureKey = false; MaybeDumpProperShadersStateTraceRing();
    Bridge_ProperShadersPatchedRenderSceneGuard(); VerifyCorrected(d);
    Check(snapshotAttempts == 0, "enabled idle StateRing makes no full device snapshot attempts");
    D3D9RenderStateApi api{}; Boundary(); queryCalls = 0;
    Check(ResolveD3D9RenderStateApi(&api), "cold actual device API resolution succeeds");
    const auto cold = queryCalls;
    for (unsigned i = 0; i < 64; ++i) Check(ResolveD3D9RenderStateApi(&api), "warm actual API resolves");
    Check(cold > 0 && queryCalls == cold, "warm matching interface avoids repeated VirtualQuery");
    std::thread other([&] {
        queryCalls = 0; D3D9RenderStateApi local{};
        Check(ResolveD3D9RenderStateApi(&local) && queryCalls > 0, "second calling thread validates its own borrowed API cache");
    }); other.join();
    d.table[57] = reinterpret_cast<void*>(&SetState2);
    bool changed = false;
    Check(SynchronizeD3D9DeviceRenderState(15, 0, nullptr, &changed) && setVersion2 > 0, "in-place method change invokes current native forwarding method");
    d.table[57] = reinterpret_cast<void*>(&SetState);
    deviceCell = &d.secondProxy;
    Check(ResolveD3D9RenderStateApi(&api) && api.device == &d.secondProxy, "device/vtable replacement resolves current identity");
    deviceCell = &d.proxy;
    d.table[58] = nullptr; Check(!ResolveD3D9RenderStateApi(&api), "null method after warm resolution is rejected"); d.table[58] = reinterpret_cast<void*>(&GetState);
    auto* bad = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    d.proxy.vtable = static_cast<void**>(bad); Check(!ResolveD3D9RenderStateApi(&api), "inaccessible vtable fails safely after warm cache");
    d.proxy.vtable = d.table.data(); VirtualFree(bad, 0, MEM_RELEASE);
    Check(ResolveD3D9RenderStateApi(&api), "valid interface recovers after invalid access");
    auto* thunk = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    Check(thunk != nullptr, "disposable executable method thunk allocates");
    if (thunk) {
        thunk[0] = 0xE9; const uint32_t jump = uint32_t(uintptr_t(&GetState) - (uintptr_t(thunk) + 5));
        std::memcpy(thunk + 1, &jump, sizeof(jump)); FlushInstructionCache(GetCurrentProcess(), thunk, 5);
        d.table[58] = thunk; Check(ResolveD3D9RenderStateApi(&api), "temporary real forwarding method validates");
        DWORD old; Check(VirtualProtect(thunk, 4096, PAGE_NOACCESS, &old) != FALSE, "method page becomes inaccessible without changing pointer");
        uint32_t value = 0; Check(!ReadD3D9DeviceRenderState(15, &value), "guarded cached method fault returns unavailable");
        queryCalls = 0;
        Check(!ReadD3D9DeviceRenderState(15, &value) && queryCalls > 0, "access fault invalidates cache before the next resolution");
        d.table[58] = reinterpret_cast<void*>(&GetState); VirtualFree(thunk, 0, MEM_RELEASE);
    }
    fixtureKey = true; MaybeDumpProperShadersStateTraceRing();
    const auto before = snapshotAttempts;
    const ULONG refsBefore = d.texture ? d.texture->AddRef() : 0; if (d.texture) d.texture->Release();
    Bridge_ProperShadersPatchedRenderSceneGuard(); VerifyCorrected(d);
    Check(snapshotAttempts > before, "manual key edge attempts full actual device snapshots");
    Check(!traceAvailable || textureGets > 0, "supported native snapshot path reaches actual texture getter");
    if (traceAvailable) {
        d.table[93] = reinterpret_cast<void*>(&WrongShaderGetter);
        ProperShadersD3D9StateSnapshot badSnapshot{};
        Check(!CaptureProperShadersD3D9StateSnapshot(&badSnapshot), "changed shader getter is re-probed instead of trusting old slot validation");
        d.table[93] = reinterpret_cast<void*>(&GetVS);
        Check(CaptureProperShadersD3D9StateSnapshot(&badSnapshot), "valid shader getter recovers on the next method identity");
    }
    fixtureTick += 200; MaybeDumpProperShadersStateTraceRing();
    ProperShadersStateTraceRecord deadlineRecord{};
    InitializeProperShadersStateTraceRecord(&deadlineRecord, kProperShadersStateTraceRenderScene);
    CaptureProperShadersStateTracePoint(&deadlineRecord, 0);
    fixtureTick += 1000; // Deliberately no owner Present/key poll after deadline.
    const auto expiredAttempts = snapshotAttempts;
    const auto expiredSequence = g_properShadersStateTraceSequence;
    Bridge_ProperShadersPatchedRenderSceneGuard();
    StoreProperShadersStateTraceRecord(&deadlineRecord);
    Check(snapshotAttempts == expiredAttempts, "deadline rejects new snapshots without another owner poll");
    Check(g_properShadersStateTraceSequence == expiredSequence, "deadline rejects late stores before deferred export");
    MaybeDumpProperShadersStateTraceRing();
    const auto stopped = snapshotAttempts;
    Bridge_ProperShadersPatchedRenderSceneGuard();
    Check(snapshotAttempts == stopped, "bounded capture expires even while key remains held");
    fixtureKey = false; MaybeDumpProperShadersStateTraceRing(); fixtureKey = true; MaybeDumpProperShadersStateTraceRing();
    Bridge_ProperShadersPatchedRenderSceneGuard(); Check(snapshotAttempts > stopped, "released then pressed key starts a fresh capture");
    ProperShadersStateTraceRecord late{};
    InitializeProperShadersStateTraceRecord(&late, kProperShadersStateTraceRenderScene);
    CaptureProperShadersStateTracePoint(&late, 0);
    const ULONG refsAfter = d.texture ? d.texture->AddRef() : 0; if (d.texture) d.texture->Release();
    Check(refsBefore == refsAfter, "actual texture getter references are balanced across captures");
    ShutdownProperShadersStateRing(); const auto shutdownGets = snapshotAttempts;
    Bridge_ProperShadersPatchedRenderSceneGuard(); Check(snapshotAttempts == shutdownGets, "shutdown cancels detailed snapshot admission");
    fixtureKey = false; MaybeDumpProperShadersStateTraceRing(); fixtureKey = true; MaybeDumpProperShadersStateTraceRing();
    const auto sequenceBeforeLate = g_properShadersStateTraceSequence;
    StoreProperShadersStateTraceRecord(&late);
    Check(g_properShadersStateTraceSequence == sequenceBeforeLate, "late record from an earlier window cannot enter a new capture");
    ShutdownProperShadersStateRing();
    d.real->SetTexture(0, nullptr);
    Check(SUCCEEDED(d.real->Reset(&d.pp)), "actual native device Reset succeeds after snapshots"); Boundary();
    queryCalls = 0; Check(ResolveD3D9RenderStateApi(&api) && queryCalls > 0, "reset lifecycle boundary revalidates methods");
    d.real->SetTexture(0, d.texture);
}
void NativePixel(Device& d) {
    d.real->SetRenderState(D3DRS_LIGHTING, FALSE); d.real->SetRenderState(D3DRS_ZENABLE, FALSE);
    d.real->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE); d.real->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    d.real->SetVertexShader(nullptr); d.real->SetPixelShader(nullptr);
    d.real->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    d.real->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    d.real->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    d.real->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    d.real->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    Bridge_ProperShadersPatchedRenderSceneGuard(); VerifyCorrected(d);
    struct Vertex { float x,y,z,rhw; DWORD colour; };
    const Vertex triangle[]={{0,0,0,1,0xff00ff00},{63,0,0,1,0xff00ff00},{0,63,0,1,0xff00ff00}};
    d.real->SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE);
    Check(SUCCEEDED(d.real->Clear(0,nullptr,D3DCLEAR_TARGET,0xff000000,1,0)),"native pixel target clears");
    Check(SUCCEEDED(d.real->BeginScene()),"native pixel scene begins");
    Check(SUCCEEDED(d.real->DrawPrimitiveUP(D3DPT_TRIANGLELIST,1,triangle,sizeof(Vertex))),"native alpha-tested triangle draws");
    Check(SUCCEEDED(d.real->EndScene()),"native pixel scene ends");
    IDirect3DSurface9* target=nullptr; IDirect3DSurface9* copy=nullptr; D3DSURFACE_DESC desc{};
    HRESULT hr=d.real->GetRenderTarget(0,&target);
    if(SUCCEEDED(hr))hr=target->GetDesc(&desc);
    if(SUCCEEDED(hr))hr=d.real->CreateOffscreenPlainSurface(desc.Width,desc.Height,desc.Format,D3DPOOL_SYSTEMMEM,&copy,nullptr);
    if(SUCCEEDED(hr))hr=d.real->GetRenderTargetData(target,copy);
    D3DLOCKED_RECT lock{}; if(SUCCEEDED(hr))hr=copy->LockRect(&lock,nullptr,D3DLOCK_READONLY);
    Check(SUCCEEDED(hr),"actual render pixels read back");
    if(SUCCEEDED(hr)){const DWORD pixel=*reinterpret_cast<DWORD*>(static_cast<uint8_t*>(lock.pBits)+8*lock.Pitch+8*4);Check((pixel&0xffffff)==0x00ff00,"guard restoration preserves visible alpha-tested pixel");copy->UnlockRect();}
    if(copy)copy->Release();if(target)target->Release();
}
bool benchmarkMode;
void Benchmark(Device& d, unsigned rounds, unsigned iterations) {
    benchmarkMode = true;
    LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
    const auto ref = d.texture ? d.texture->AddRef() : 0; if(d.texture)d.texture->Release();
    for (unsigned round=0;round<rounds;++round) for(unsigned offset=0;offset<3;++offset) {
        const unsigned mode=(round%2)?2-offset:offset;
        ShutdownProperShadersStateRing(); Boundary();
        fixtureKey=false; g_properShadersRenderStateGuardConfig.enableStateRing=mode!=0;
        MaybeDumpProperShadersStateTraceRing();
        if(mode==2){fixtureKey=true;MaybeDumpProperShadersStateTraceRing();fixtureKey=false;MaybeDumpProperShadersStateTraceRing();}
        for(unsigned i=0;i<64;++i){Bridge_ProperShadersPatchedRenderSceneGuard();MaybeDumpProperShadersStateTraceRing();}
        queryCalls=0; snapshotAttempts=textureGets=0;
        LARGE_INTEGER begin{},end{}; QueryPerformanceCounter(&begin);
        for(unsigned i=0;i<iterations;++i){Bridge_ProperShadersPatchedRenderSceneGuard();MaybeDumpProperShadersStateTraceRing();}
        QueryPerformanceCounter(&end);
        VerifyCorrected(d);
        const auto queries=queryCalls; const auto attempts=snapshotAttempts;
        if(mode==2)Check(attempts==3ull*iterations && textureGets>0,"active benchmark completes full native snapshots");
#if !defined(BASELINE_SOURCE)
        if(mode!=2)Check(attempts==0,"idle benchmark admits no detailed snapshots");
#endif
        const double ns=double(end.QuadPart-begin.QuadPart)*1e9/frequency.QuadPart;
        std::printf("{\"type\":\"guard-round\",\"pid\":%lu,\"round\":%u,\"mode\":%u,\"iterations\":%u,\"qpcBegin\":%llu,\"qpcEnd\":%llu,\"qpcFrequency\":%llu,\"wallNsPerCall\":%.9f,\"virtualQueries\":%llu,\"snapshotAttempts\":%u,\"textureGets\":%u}\n",GetCurrentProcessId(),round,mode,iterations,static_cast<unsigned long long>(begin.QuadPart),static_cast<unsigned long long>(end.QuadPart),static_cast<unsigned long long>(frequency.QuadPart),ns/iterations,queries,attempts,textureGets);
    }
    const auto after=d.texture?d.texture->AddRef():0;if(d.texture)d.texture->Release();
    Check(ref==after,"benchmark leaves actual texture reference count unchanged");
}
}
int main(int argc, char** argv) {
    std::filesystem::create_directories("scripts");
    const bool bench=argc>1 && std::strcmp(argv[1],"--benchmark")==0;
    Device d(bench?(argc>2?argv[2]:nullptr):(argc>1?argv[1]:nullptr));
    Check(d.real != nullptr, "actual D3D9 HAL device created");
    if (d.real && bench) Benchmark(d,argc>3?std::strtoul(argv[3],nullptr,10):4,argc>4?std::strtoul(argv[4],nullptr,10):512);
    else if (d.real) { NativeBehavior(d); NativePixel(d); }
    std::printf("checks=%u failures=%u queryCalls=%llu\n", checks, failures, queryCalls);
    return failures ? 1 : 0;
}
