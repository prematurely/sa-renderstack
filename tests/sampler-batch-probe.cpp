#include <windows.h>
#include <tlhelp32.h>
#include <d3d9.h>
#include "../sdk/include/sa_renderstack/backend_api.h"
#include "../backend/dxvk/src/d3d9/d3d9_bridge_diagnostics.h"
#include "../backend/dxvk/src/d3d9/d3d9_bridge_thread_stats.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <thread>
#include <vector>

static unsigned checks = 0, failures = 0;
#define CHECK(expr) do { ++checks; if (!(expr)) { ++failures; std::printf("FAIL line=%u %s\n", __LINE__, #expr); } } while (0)
static const DWORD Slots[] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
  D3DDMAPSAMPLER,D3DVERTEXTEXTURESAMPLER0,D3DVERTEXTEXTURESAMPLER1,
  D3DVERTEXTEXTURESAMPLER2,D3DVERTEXTEXTURESAMPLER3};
static const DWORD Initial[] = {0,1,1,1,0xff102030,1,1,0,0,0,1,0,0,0};
static const DWORD Changed[] = {0,3,3,3,0xff405060,2,2,1,0x3f000000,1,2,1,1,1};
using Binding = D3D9GtaSaSamplerStateBinding;
static LONGLONG Qpc() { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; }
static UINT64 Cycles() { ULONG64 v = 0; if (!QueryThreadCycleTime(GetCurrentThread(), &v)) std::abort(); return v; }

struct Fixture {
  HMODULE module = nullptr;
  HWND window = nullptr;
  IDirect3D9* factory = nullptr;
  IDirect3DDevice9* d = nullptr;
  ID3D9GtaSaCompatDevice4* api = nullptr;
  ID3D9BridgeDiagnostics* diagnostic = nullptr;
  ID3D9BridgeThreadStats* stats = nullptr;
  D3DPRESENT_PARAMETERS pp{};
  bool protectedDevice;

  Fixture(const char* dll, bool protect, bool compat) : protectedDevice(protect) {
    module = LoadLibraryA(dll); CHECK(module != nullptr); if (!module) std::exit(2);
    auto create = reinterpret_cast<IDirect3D9* (WINAPI*)(UINT)>(GetProcAddress(module, "Direct3DCreate9"));
    CHECK(create != nullptr); if (!create) std::exit(2);
    factory = create(D3D_SDK_VERSION); CHECK(factory != nullptr); if (!factory) std::exit(2);
    window = CreateWindowA("STATIC", "sampler native probe", WS_OVERLAPPEDWINDOW, 0,0,64,64,nullptr,nullptr,nullptr,nullptr);
    CHECK(window != nullptr);
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = window;
    pp.BackBufferWidth = pp.BackBufferHeight = 32; pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    CHECK(factory->CreateDevice(0, D3DDEVTYPE_HAL, window,
      D3DCREATE_HARDWARE_VERTEXPROCESSING | (protect ? D3DCREATE_MULTITHREADED : 0), &pp, &d) == S_OK);
    if (!d) std::exit(2);
    const HRESULT result = d->QueryInterface(__uuidof(ID3D9GtaSaCompatDevice4), reinterpret_cast<void**>(&api));
    CHECK(compat ? result == S_OK && api : result == E_NOINTERFACE && !api);
    d->QueryInterface(IID_D3D9BridgeDiagnostics, reinterpret_cast<void**>(&diagnostic));
    CHECK(diagnostic != nullptr); // Diagnostics exists even when the optional API is disabled.
    CHECK(d->QueryInterface(IID_D3D9BridgeThreadStats,reinterpret_cast<void**>(&stats)) == S_OK && stats);
  }
  ~Fixture() {
    if (diagnostic) diagnostic->Release();
    if (stats) stats->Release();
    if (api) api->Release();
    if (d) d->Release();
    if (factory) factory->Release();
    if (window) DestroyWindow(window);
    if (module) FreeLibrary(module);
  }
  HRESULT Batch(const Binding* entries, UINT count) {
    D3D9GtaSaEffectStateBatch batch{};
    batch.StructSize = sizeof(batch); batch.ApiVersion = 5;
    batch.SamplerStateCount = count; batch.SamplerStates = entries;
    return api->SubmitEffectStateBatch(&batch);
  }
  D3D9BridgeThreadSnapshot Activity() {
    D3D9BridgeThreadSnapshot s{}; s.StructSize=sizeof(s); s.Version=1;
    CHECK(stats->GetThreadStats(&s) == S_OK);
    return s;
  }
  void WriteAll(const DWORD* values, bool batch) {
    std::vector<Binding> entries;
    for (DWORD slot : Slots) for (unsigned type=1; type<=13; ++type)
      entries.push_back({slot, D3DSAMPLERSTATETYPE(type), values[type]});
    if (batch) CHECK(Batch(entries.data(), UINT(entries.size())) == S_OK);
    else for (const auto& e : entries) CHECK(d->SetSamplerState(e.Sampler, e.Type, e.Value) == S_OK);
  }
  void ExpectAll(const DWORD* values) {
    for (DWORD slot : Slots) for (unsigned type=1; type<=13; ++type) {
      DWORD value = 0xdeadbeef;
      CHECK(d->GetSamplerState(slot, D3DSAMPLERSTATETYPE(type), &value) == S_OK);
      CHECK(value == values[type]);
    }
  }
  void Flush() {
    IDirect3DQuery9* query = nullptr;
    CHECK(d->CreateQuery(D3DQUERYTYPE_EVENT, &query) == S_OK && query);
    if (!query) return;
    CHECK(query->Issue(D3DISSUE_END) == S_OK);
    HRESULT result; const auto deadline = GetTickCount64() + 20000;
    do { result = query->GetData(nullptr, 0, D3DGETDATA_FLUSH); if (result == S_FALSE) Sleep(0); }
    while (result == S_FALSE && GetTickCount64() < deadline);
    CHECK(result == S_OK); query->Release();
  }
};

static void RecordingAndJournal(Fixture& f, bool batch) {
  // Moving equality ahead of recording loses all these same-value captures.
  f.WriteAll(Initial, false);
  IDirect3DStateBlock9* block = nullptr;
  CHECK(f.d->BeginStateBlock() == S_OK);
  f.WriteAll(Initial, batch);
  CHECK(f.d->EndStateBlock(&block) == S_OK && block);
  f.WriteAll(Changed, false);
  CHECK(block->Apply() == S_OK); f.ExpectAll(Initial); block->Release();

  CHECK(f.d->BeginStateBlock() == S_OK);
  f.WriteAll(Changed, batch);
  f.ExpectAll(Initial); // Recording must not modify the live device.
  CHECK(f.d->EndStateBlock(&block) == S_OK && block);
  CHECK(block->Apply() == S_OK); f.ExpectAll(Changed);
  if (f.api) {
    f.WriteAll(Initial, false);
    CHECK(f.api->BeginStateJournal() == S_OK);
    f.WriteAll(Initial, batch); // Equal first write cannot replace original capture.
    f.WriteAll(Changed, batch);
    CHECK(f.d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_MIRROR) == S_OK);
    CHECK(f.api->RestoreStateJournal() == S_OK); f.ExpectAll(Initial);
    CHECK(f.api->BeginStateJournal() == S_OK);
    CHECK(block->Apply() == S_OK); f.ExpectAll(Changed);
    CHECK(f.api->RestoreStateJournal() == S_OK); f.ExpectAll(Initial);
  }
  block->Release();
}

static void BatchContract(Fixture& f) {
  if (!f.api) return;
  CHECK(f.d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP) == S_OK);
  Binding values[] = {{0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP}, {16,D3DSAMP_ADDRESSU,D3DTADDRESS_WRAP}};
  CHECK(f.Batch(values, 2) == D3DERR_INVALIDCALL);
  DWORD actual = 0;
  CHECK(f.d->GetSamplerState(0, D3DSAMP_ADDRESSU, &actual) == S_OK && actual == D3DTADDRESS_WRAP);
  values[1] = {15, D3DSAMPLERSTATETYPE(14), 0};
  CHECK(f.Batch(values, 2) == D3DERR_INVALIDCALL);
  CHECK(f.d->GetSamplerState(0, D3DSAMP_ADDRESSU, &actual) == S_OK && actual == D3DTADDRESS_WRAP);
  CHECK(f.Batch(nullptr, 1) == D3DERR_INVALIDCALL);
  CHECK(f.Batch(values, 513) == D3DERR_INVALIDCALL);
  CHECK(f.Batch(nullptr, 0) == S_OK);
  const Binding sequence[] = {{0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP}, {0,D3DSAMP_ADDRESSU,D3DTADDRESS_WRAP},
    {0,D3DSAMP_MIPMAPLODBIAS,MAKEFOURCC('G','E','T','4')}, {0,D3DSAMP_MAGFILTER,D3DTEXF_POINT},
    {0,D3DSAMP_MIPMAPLODBIAS,MAKEFOURCC('G','E','T','1')}, {0,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR}};
  CHECK(f.Batch(sequence, UINT(std::size(sequence))) == S_OK);
  CHECK(f.d->GetSamplerState(0, D3DSAMP_ADDRESSU, &actual) == S_OK && actual == D3DTADDRESS_WRAP);
  CHECK(f.d->GetSamplerState(0, D3DSAMP_MIPMAPLODBIAS, &actual) == S_OK && actual == MAKEFOURCC('G','E','T','1'));
  CHECK(f.d->GetSamplerState(0, D3DSAMP_MAGFILTER, &actual) == S_OK && actual == D3DTEXF_LINEAR);
}

static void Audit(Fixture& f) {
  if (!f.diagnostic) return;
  CHECK(f.d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP) == S_OK);
  CHECK(f.diagnostic->SetCaptureOwner(TRUE) == S_OK);
  if (!f.api) {
    CHECK(f.diagnostic->StartAudit(81,10000) == D3DERR_NOTAVAILABLE);
    CHECK(f.diagnostic->SetCaptureOwner(FALSE) == S_OK);
    return;
  }
  CHECK(f.diagnostic->StartAudit(81,10000) == S_OK);
  CHECK(f.d->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_WRAP) == S_OK);
  CHECK(f.d->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_WRAP) == S_OK);
  CHECK(f.d->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP) == S_OK);
  Binding entries[] = {{0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP},{0,D3DSAMP_ADDRESSU,D3DTADDRESS_WRAP},
    {0,D3DSAMP_ADDRESSU,D3DTADDRESS_WRAP}};
  CHECK(f.Batch(entries,3) == S_OK);
  // Recording (including equal writes) must not contribute live audit calls.
  IDirect3DStateBlock9* recorded = nullptr;
  CHECK(f.d->BeginStateBlock() == S_OK);
  CHECK(f.d->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_WRAP) == S_OK);
  CHECK(f.Batch(entries,3) == S_OK);
  CHECK(f.d->EndStateBlock(&recorded) == S_OK && recorded);
  if (recorded) recorded->Release();
  CHECK(f.diagnostic->StopAudit(81) == S_OK);
  D3D9BridgeAuditSnapshot s{}; s.StructSize=sizeof(s); s.Version=1;
  CHECK(f.diagnostic->GetAudit(&s) == S_OK);
  CHECK(s.Counters[1].Calls == 6 && s.Counters[1].RedundantCalls == 4);
  CHECK(s.Counters[1].TimedCalls == 6);
  CHECK(f.diagnostic->SetCaptureOwner(FALSE) == S_OK);
}

static void ProtectedBatchSnapshots(Fixture& f) {
  if (!f.protectedDevice || !f.api) return;
  std::array<Binding,16> a, b;
  for (unsigned i=0;i<16;++i) { a[i]={i,D3DSAMP_ADDRESSU,D3DTADDRESS_WRAP}; b[i]={i,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP}; }
  CHECK(f.Batch(a.data(),16) == S_OK);
  std::atomic<bool> stop{false}, started{false}; std::atomic<unsigned> errors{0};
  std::thread writer([&] {
    unsigned i=0;
    do {
      if (f.Batch((i++ & 1) ? a.data() : b.data(),16) != S_OK) ++errors;
      started.store(true);
      if (!(i&63)) std::this_thread::yield();
    } while (!stop.load());
  });
  while (!started.load()) std::this_thread::yield();
  std::vector<IDirect3DStateBlock9*> snapshots;
  for (unsigned i=0;i<64;++i) {
    IDirect3DStateBlock9* block=nullptr;
    CHECK(f.d->CreateStateBlock(D3DSBT_PIXELSTATE,&block) == S_OK && block);
    if (block) snapshots.push_back(block);
  }
  stop.store(true); writer.join(); CHECK(errors.load() == 0);
  for (auto block : snapshots) {
    CHECK(block->Apply() == S_OK);
    DWORD first=0; CHECK(f.d->GetSamplerState(0,D3DSAMP_ADDRESSU,&first) == S_OK);
    CHECK(first == D3DTADDRESS_WRAP || first == D3DTADDRESS_CLAMP);
    for (unsigned i=1;i<16;++i) { DWORD value=0; CHECK(f.d->GetSamplerState(i,D3DSAMP_ADDRESSU,&value) == S_OK && value == first); }
    block->Release();
  }
}

struct Pixels {
  struct Vertex { float x,y,z,rhw,u,v; };
  Fixture& f; IDirect3DTexture9* texture=nullptr; IDirect3DSurface9* readback=nullptr;
  Pixels(Fixture& fixture) : f(fixture) {
    CHECK(f.d->CreateTexture(2,2,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&texture,nullptr) == S_OK);
    CHECK(f.d->CreateOffscreenPlainSurface(32,32,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&readback,nullptr) == S_OK);
    Fill(false);
    CHECK(f.d->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1) == S_OK);
    f.d->SetRenderState(D3DRS_ZENABLE,FALSE); f.d->SetRenderState(D3DRS_LIGHTING,FALSE);
    f.d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE); f.d->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);
    f.d->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1);
    f.d->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE);
    f.d->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1);
    f.d->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE);
    CHECK(f.d->SetTexture(0,texture) == S_OK);
    for (unsigned type=1;type<=13;++type) CHECK(f.d->SetSamplerState(0,D3DSAMPLERSTATETYPE(type),Initial[type]) == S_OK);
  }
  ~Pixels() { f.d->SetTexture(0,nullptr); if (readback) readback->Release(); if (texture) texture->Release(); }
  void Fill(bool gray) {
    D3DLOCKED_RECT lock{}; CHECK(texture->LockRect(0,&lock,nullptr,0) == S_OK);
    for (unsigned y=0;y<2;++y) { auto row=reinterpret_cast<DWORD*>(static_cast<char*>(lock.pBits)+y*lock.Pitch);
      row[0]=gray?0xff808080:0xffff0000; row[1]=gray?0xff808080:0xff0000ff; }
    CHECK(texture->UnlockRect(0) == S_OK);
  }
  HRESULT Draw(float u=.5f) {
    Vertex vertices[]={{0,0,.5f,1,u,.5f},{64,0,.5f,1,u,.5f},{0,64,.5f,1,u,.5f}};
    return f.d->DrawPrimitiveUP(D3DPT_TRIANGLELIST,1,vertices,sizeof(Vertex));
  }
  void Expect(DWORD expected, float u=.5f, const char* label="sampler pixel") {
    CHECK(f.d->BeginScene() == S_OK); CHECK(Draw(u) == S_OK); CHECK(f.d->EndScene() == S_OK);
    IDirect3DSurface9* target=nullptr; CHECK(f.d->GetRenderTarget(0,&target) == S_OK);
    CHECK(f.d->GetRenderTargetData(target,readback) == S_OK); target->Release();
    D3DLOCKED_RECT lock{}; CHECK(readback->LockRect(&lock,nullptr,D3DLOCK_READONLY) == S_OK);
    DWORD actual=*reinterpret_cast<DWORD*>(static_cast<char*>(lock.pBits)+4*lock.Pitch+4*4);
    const auto failuresBefore = failures;
    for (unsigned shift : {0u,8u,16u,24u}) CHECK(std::abs(int((actual>>shift)&255)-int((expected>>shift)&255)) <= 2);
    if (failures != failuresBefore)
      std::fprintf(stderr,"%s expected=%08lx actual=%08lx\n",label,expected,actual);
    CHECK(readback->UnlockRect() == S_OK);
  }
};

static void PixelBehavior(Fixture& f) {
  Pixels p(f); p.Expect(0xff0000ff);
  for (bool batched : {false,true}) {
    if (batched && !f.api) continue;
    auto set=[&](D3DSAMPLERSTATETYPE type,DWORD value) {
      Binding entry{0,type,value}; CHECK((batched?f.Batch(&entry,1):f.d->SetSamplerState(0,type,value)) == S_OK);
    };
    set(D3DSAMP_MINFILTER,D3DTEXF_LINEAR); set(D3DSAMP_MAGFILTER,D3DTEXF_LINEAR); p.Expect(0xff800080);
    set(D3DSAMP_MINFILTER,D3DTEXF_POINT); set(D3DSAMP_MAGFILTER,D3DTEXF_POINT);
    set(D3DSAMP_ADDRESSU,D3DTADDRESS_WRAP); p.Expect(0xffff0000,1.25f);
    set(D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP); p.Expect(0xff0000ff,1.25f);
    set(D3DSAMP_BORDERCOLOR,0xff00ff00); set(D3DSAMP_ADDRESSU,D3DTADDRESS_BORDER); p.Expect(0xff00ff00,2);
    IDirect3DStateBlock9* block=nullptr;
    CHECK(f.d->BeginStateBlock() == S_OK); set(D3DSAMP_ADDRESSU,D3DTADDRESS_BORDER);
    CHECK(f.d->EndStateBlock(&block) == S_OK);
    set(D3DSAMP_ADDRESSU,D3DTADDRESS_WRAP); p.Expect(0xffff0000,1.25f);
    CHECK(block->Apply() == S_OK); p.Expect(0xff00ff00,2); block->Release();
    if (f.api) { CHECK(f.api->BeginStateJournal() == S_OK); set(D3DSAMP_ADDRESSU,D3DTADDRESS_WRAP);
      p.Expect(0xffff0000,1.25f); CHECK(f.api->RestoreStateJournal() == S_OK); p.Expect(0xff00ff00,2); }
    set(D3DSAMP_ADDRESSU,D3DTADDRESS_WRAP);
  }
  p.Fill(true);
  for (DWORD srgb : {1u,3u,2u,0u}) {
    CHECK(f.d->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,srgb) == S_OK);
    p.Expect(srgb&1?0xff373737:0xff808080);
  }

  // Getters alone cannot detect a stale GPU sampler or sRGB texture view.
  for (bool batched : {false,true}) for (bool journal : {false,true}) {
    if ((batched || journal) && !f.api) continue;
    auto set=[&](D3DSAMPLERSTATETYPE type,DWORD value) {
      Binding entry{0,type,value}; CHECK((batched?f.Batch(&entry,1):f.d->SetSamplerState(0,type,value)) == S_OK);
    };
    IDirect3DStateBlock9* block = nullptr;
    auto restore=[&] {
      if (journal) CHECK(f.api->RestoreStateJournal() == S_OK);
      else { CHECK(block->Apply() == S_OK); block->Release(); block=nullptr; }
    };

    p.Fill(false);
    set(D3DSAMP_MINFILTER,D3DTEXF_POINT); set(D3DSAMP_MAGFILTER,D3DTEXF_POINT);
    p.Expect(0xff0000ff);
    if (journal) CHECK(f.api->BeginStateJournal() == S_OK);
    else {
      CHECK(f.d->BeginStateBlock() == S_OK);
      set(D3DSAMP_MINFILTER,D3DTEXF_POINT); set(D3DSAMP_MAGFILTER,D3DTEXF_POINT);
      CHECK(f.d->EndStateBlock(&block) == S_OK && block);
    }
    set(D3DSAMP_MINFILTER,D3DTEXF_LINEAR); set(D3DSAMP_MAGFILTER,D3DTEXF_LINEAR);
    p.Expect(0xff800080); restore();
    p.Expect(0xff0000ff,.5f,journal?"journal filter restore":"stateblock filter restore");

    p.Fill(true); set(D3DSAMP_SRGBTEXTURE,1); p.Expect(0xff373737);
    if (journal) CHECK(f.api->BeginStateJournal() == S_OK);
    else {
      CHECK(f.d->BeginStateBlock() == S_OK); set(D3DSAMP_SRGBTEXTURE,1);
      CHECK(f.d->EndStateBlock(&block) == S_OK && block);
    }
    set(D3DSAMP_SRGBTEXTURE,0); p.Expect(0xff808080); restore();
    p.Expect(0xff373737,.5f,journal?"journal sRGB restore":"stateblock sRGB restore");
    set(D3DSAMP_SRGBTEXTURE,0);
  }
}

static void Check(Fixture& f) {
  RecordingAndJournal(f,false); if (f.api) RecordingAndJournal(f,true);
  BatchContract(f); Audit(f); ProtectedBatchSnapshots(f);
  CHECK(f.d->SetSamplerState(16,D3DSAMP_ADDRESSU,9) == S_OK); // Existing invalid-slot no-op contract.
  CHECK(f.d->GetSamplerState(0,D3DSAMP_ADDRESSU,nullptr) == D3DERR_INVALIDCALL);
  if (f.api) { CHECK(f.api->BeginStateJournal() == S_OK); CHECK(f.d->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP) == S_OK); }
  CHECK(f.d->Reset(&f.pp) == S_OK);
  if (f.api) CHECK(f.api->RestoreStateJournal() == D3DERR_INVALIDCALL);
  DWORD value=0; CHECK(f.d->GetSamplerState(0,D3DSAMP_ADDRESSU,&value) == S_OK && value == D3DTADDRESS_WRAP);
  PixelBehavior(f); f.Flush();
}

static void Benchmark(Fixture& f, const char* variant, unsigned round, const std::string& name) {
  const bool draw=name.rfind("draw",0)==0, batch=name.rfind("batch",0)==0;
  const bool change=name.find("change")!=std::string::npos;
  const bool block=name=="stateblock";
  const unsigned count=(draw?2048:(batch||block?20000:1000000));
  Pixels pixels(f); std::array<Binding,16> a,b;
  for (unsigned i=0;i<16;++i) { a[i]={i,D3DSAMP_ADDRESSU,D3DTADDRESS_WRAP}; b[i]={i,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP}; }
  IDirect3DStateBlock9* blocks[2]={};
  if (block) for (unsigned i=0;i<2;++i) {
    CHECK(f.d->BeginStateBlock() == S_OK); CHECK(f.Batch(i?b.data():a.data(),16) == S_OK);
    CHECK(f.d->EndStateBlock(&blocks[i]) == S_OK);
  }
  auto work=[&](unsigned i) {
    if (block) return blocks[i&1]->Apply();
    if (batch) return f.Batch(change && (i&1)?b.data():a.data(),16);
    if (!draw || name!="draw-control") {
      HRESULT hr=f.d->SetSamplerState(0,D3DSAMP_ADDRESSU,change&&(i&1)?D3DTADDRESS_CLAMP:D3DTADDRESS_WRAP);
      if (FAILED(hr)) return hr;
    }
    return draw?pixels.Draw():S_OK;
  };
  CHECK(f.d->BeginScene() == S_OK);
  HRESULT status=S_OK;
  for (unsigned i=0;i<2048;++i) if (FAILED(work(i))) status=E_FAIL;
  CHECK(f.d->EndScene() == S_OK); f.Flush();
  const auto activityBefore=f.Activity();
  CHECK(f.d->BeginScene() == S_OK);
  const auto start=Qpc(); const auto cycles=Cycles();
  for (unsigned i=0;i<count;++i) if (FAILED(work(i))) status=E_FAIL;
  const auto endCycles=Cycles(); const auto end=Qpc();
  CHECK(f.d->EndScene() == S_OK); f.Flush(); const auto complete=Qpc();
  const auto activityAfter=f.Activity();
  CHECK(status == S_OK);
  LARGE_INTEGER frequency; QueryPerformanceFrequency(&frequency);
  std::printf("{\"kind\":\"sample\",\"case\":\"%s\",\"variant\":\"%s\",\"round\":%u,\"protected\":%u,\"iterations\":%u,\"ns\":%.6f,\"cycles\":%.6f,\"completeNs\":%.6f,\"csCpu100ns\":%llu,\"csIdleUs\":%llu,\"csWaitUs\":%llu,\"gpuWaitUs\":%llu,\"chunks\":%llu,\"submits\":%llu}\n",
    name.c_str(),variant,round,unsigned(f.protectedDevice),count,1e9*double(end-start)/frequency.QuadPart/count,
    double(endCycles-cycles)/count,1e9*double(complete-start)/frequency.QuadPart/count,
    activityAfter.CsCpu100ns-activityBefore.CsCpu100ns,activityAfter.CsQueueIdleUs-activityBefore.CsQueueIdleUs,
    activityAfter.CsSyncUs-activityBefore.CsSyncUs,activityAfter.GpuSyncUs-activityBefore.GpuSyncUs,
    activityAfter.Chunks-activityBefore.Chunks,activityAfter.Submits-activityBefore.Submits);
  for (auto value : blocks) if (value) value->Release();
}

static void NoMods() {
  HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,GetCurrentProcessId()); CHECK(snapshot!=INVALID_HANDLE_VALUE);
  MODULEENTRY32 entry{}; entry.dwSize=sizeof(entry);
  if (Module32First(snapshot,&entry)) do {
    std::string name=entry.szModule; std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return char(std::tolower(c));});
    CHECK(name.find("propershaders")==std::string::npos && name.find("cleo")==std::string::npos && name.find("flacompat")==std::string::npos);
  } while (Module32Next(snapshot,&entry)); CloseHandle(snapshot);
}

int main(int argc,char** argv) {
  if (argc>=5 && std::string(argv[1])=="check") {
    Fixture f(argv[2],std::atoi(argv[3])!=0,std::atoi(argv[4])!=0); Check(f); NoMods();
  } else if (argc==6 && std::string(argv[1])=="pair") {
    const unsigned seed=std::atoi(argv[4]), rounds=std::atoi(argv[5]);
    for (bool protect : {false,true}) {
      Fixture first(seed&1?argv[3]:argv[2],protect,true), second(seed&1?argv[2]:argv[3],protect,true);
      auto& baseline=seed&1?second:first; auto& candidate=seed&1?first:second;
      const bool same=GetModuleHandleA(argv[2])==GetModuleHandleA(argv[3]);
      std::printf("{\"kind\":\"metadata\",\"protected\":%u,\"sameModule\":%u,\"seed\":%u,\"rounds\":%u}\n",unsigned(protect),unsigned(same),seed,rounds);
      Check(baseline); Check(candidate); NoMods();
      const std::vector<std::string> cases={"sampler-repeat","sampler-change","batch-repeat","batch-change","stateblock","draw-control","draw-repeat","draw-change"};
      for (unsigned round=0;round<rounds;++round) for (unsigned k=0;k<cases.size();++k) {
        const auto& name=cases[(k+seed+round)%cases.size()];
        if ((seed+round)&1) { Benchmark(candidate,"candidate",round,name); Benchmark(baseline,"baseline",round,name); }
        else { Benchmark(baseline,"baseline",round,name); Benchmark(candidate,"candidate",round,name); }
      }
    }
  } else { std::fprintf(stderr,"usage: sampler-batch-probe check DLL protected compat | pair BASE CAND seed rounds\n"); return 2; }
  std::printf("{\"kind\":\"result\",\"checks\":%u,\"failures\":%u}\n",checks,failures);
  return failures?1:0;
}
