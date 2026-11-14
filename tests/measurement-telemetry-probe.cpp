#include <windows.h>
#include <d3d9.h>
#include "../backend/dxvk/src/d3d9/d3d9_bridge_telemetry.h"
#include "../backend/dxvk/src/dxvk/dxvk_telemetry.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include <stdexcept>

static std::atomic<unsigned> checks{0}, failures{0};
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; std::printf("FAIL line=%u %s\n", __LINE__, #c); } } while (0)

static D3D9BridgeTelemetryConfig Config(UINT64 id, UINT maxEvents = 16384) {
  D3D9BridgeTelemetryConfig c{}; c.StructSize=sizeof(c); c.Version=1;
  c.CaptureId=id; c.MaxEvents=maxEvents; return c;
}
static D3D9BridgeTelemetryRead Read(ID3D9BridgeTelemetry* t, UINT64 id,
    D3D9BridgeTelemetryEvent* events=nullptr, UINT count=0) {
  D3D9BridgeTelemetryRead r{}; r.StructSize=sizeof(r); r.Version=1;
  CHECK(t->ReadEvents(id,&r,events,count)==S_OK); return r;
}
static D3D9BridgeMemorySnapshot Snapshot(ID3D9BridgeTelemetry* t, UINT64 id,
    D3D9BridgeMemoryChunk* chunks, UINT chunkCount, UINT* masks, UINT maskCount) {
  D3D9BridgeMemorySnapshot s{}; s.StructSize=sizeof(s); s.Version=1;
  CHECK(t->GetMemorySnapshot(id,&s,chunks,chunkCount,masks,maskCount)==S_OK); return s;
}
// Deterministically hold a real recorder token over Stop and Start. A mere
// scheduling race in the device test cannot guarantee this ordering.
static void TestRecorderGenerations() {
  dxvk::DxvkTelemetry recorder;
  D3D9BridgeTelemetryRead r{}; r.StructSize=sizeof(r); r.Version=1;
  CHECK(FAILED(recorder.read(10,&r,nullptr,0)));
  auto c=Config(10,2); CHECK(recorder.start(&c)==S_OK);
  auto old=recorder.begin(); CHECK(old.generation!=0);
  CHECK(recorder.stop(10)==S_OK);
  CHECK(recorder.read(10,&r,nullptr,0)==S_OK);
  CHECK(r.Started==1 && r.InFlight==1 && r.InFlightAtStop==1);
  c=Config(11,2); CHECK(recorder.start(&c)==S_OK);
  recorder.finish(old,D3D9_BRIDGE_TELEMETRY_CS_WAIT,123,0,0,0);
  CHECK(recorder.read(11,&r,nullptr,0)==S_OK);
  CHECK(r.Started==0 && r.InFlight==0 && r.Remaining==0);
  auto current=recorder.begin(); recorder.finish(current,D3D9_BRIDGE_TELEMETRY_CS_WAIT,456,0,0,0);
  D3D9BridgeTelemetryEvent e[2]{};
  CHECK(recorder.read(11,&r,e,2)==S_OK);
  CHECK(r.Written==1 && e[0].CaptureId==11 && e[0].Identity==456);
  auto late=recorder.begin(); CHECK(recorder.stop(11)==S_OK);
  recorder.finish(late,D3D9_BRIDGE_TELEMETRY_CS_WAIT,789,0,0,0);
  CHECK(recorder.read(11,&r,nullptr,0)==S_OK);
  CHECK(r.InFlightAtStop==1 && r.InFlight==0 && r.LateDropped==1);
  CHECK(FAILED(recorder.start(&c)));
  c=Config(12,2); c.MinDurationNs=~UINT64(0); c.MinBytes=~UINT64(0);
  CHECK(recorder.start(&c)==S_OK);
  auto filtered=recorder.begin(); recorder.finish(filtered,D3D9_BRIDGE_TELEMETRY_ALLOC,1,1,0,0);
  try {
    dxvk::DxvkTelemetryScope scope(&recorder,D3D9_BRIDGE_TELEMETRY_ALLOC,2,1);
    throw std::runtime_error("fixture allocation failure");
  } catch(const std::runtime_error&) { }
  CHECK(recorder.stop(12)==S_OK); CHECK(recorder.read(12,&r,e,2)==S_OK);
  CHECK(r.Filtered==1 && r.Written==1 && (e[0].Flags&D3D9_BRIDGE_TELEMETRY_FAILED) && e[0].Result<0);
  c=Config(13,2); CHECK(recorder.start(&c)==S_OK);
  for(unsigned i=0;i<4;i++) { auto token=recorder.begin(); recorder.finish(token,D3D9_BRIDGE_TELEMETRY_ALLOC,i,1,0,0); }
  CHECK(recorder.stop(13)==S_OK); CHECK(recorder.read(13,&r,e,2)==S_OK);
  CHECK(r.Written==2 && r.Dropped==2 && e[0].Identity==0 && e[1].Identity==1);
}
static void Fence(IDirect3DDevice9* d) {
  IDirect3DQuery9* query=nullptr;
  if(SUCCEEDED(d->CreateQuery(D3DQUERYTYPE_EVENT,&query)) && query) {
    CHECK(SUCCEEDED(query->Issue(D3DISSUE_END)));
    HRESULT hr=S_FALSE; const auto deadline=GetTickCount64()+10000;
    while(hr==S_FALSE && GetTickCount64()<deadline) {
      hr=query->GetData(nullptr,0,D3DGETDATA_FLUSH);
      if(hr==S_FALSE) Sleep(1);
    }
    if(hr!=S_OK) std::printf("fence result=0x%08lx\n",(unsigned long)hr);
    CHECK(hr==S_OK); query->Release();
  } else CHECK(false);
}
static void Work(IDirect3DDevice9* d, unsigned iterations, bool waitEach=true) {
  for (unsigned i=0;i<iterations;++i) {
    IDirect3DTexture9 *source=nullptr,*destination=nullptr;
    IDirect3DVertexBuffer9* vb=nullptr;
    CHECK(SUCCEEDED(d->CreateTexture(512,512,1,0,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&source,nullptr)));
    CHECK(SUCCEEDED(d->CreateTexture(512,512,1,0,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&destination,nullptr)));
    CHECK(SUCCEEDED(d->CreateVertexBuffer(131072,D3DUSAGE_DYNAMIC|D3DUSAGE_WRITEONLY,0,D3DPOOL_DEFAULT,&vb,nullptr)));
    if(source && destination) {
      D3DLOCKED_RECT rect{}; CHECK(SUCCEEDED(source->LockRect(0,&rect,nullptr,0)));
      if(rect.pBits) { for(unsigned y=0;y<512;y++) std::memset(static_cast<char*>(rect.pBits)+y*rect.Pitch,int(i&255),512*4); }
      CHECK(SUCCEEDED(source->UnlockRect(0)));
      CHECK(SUCCEEDED(d->UpdateTexture(source,destination)));
    }
    if(vb) {
      void* data=nullptr; CHECK(SUCCEEDED(vb->Lock(0,0,&data,D3DLOCK_DISCARD)));
      if(data) std::memset(data,0x5a,131072);
      CHECK(SUCCEEDED(vb->Unlock()));
    }
    if(waitEach) Fence(d);
    if(vb) vb->Release(); if(destination) destination->Release(); if(source) source->Release();
  }
}
static void CheckPixels(IDirect3DDevice9* d) {
  IDirect3DTexture9 *source=nullptr,*destination=nullptr;
  CHECK(SUCCEEDED(d->CreateTexture(16,16,1,0,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&source,nullptr)));
  CHECK(SUCCEEDED(d->CreateTexture(16,16,1,0,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&destination,nullptr)));
  if(source && destination) {
    D3DLOCKED_RECT r{}; CHECK(SUCCEEDED(source->LockRect(0,&r,nullptr,0)));
    if(r.pBits) for(unsigned y=0;y<16;y++) for(unsigned x=0;x<16;x++)
      reinterpret_cast<DWORD*>(static_cast<char*>(r.pBits)+r.Pitch*y)[x]=0xff3a6b9c;
    CHECK(SUCCEEDED(source->UnlockRect(0))); CHECK(SUCCEEDED(d->UpdateTexture(source,destination)));
    struct Vertex { float x,y,z,rhw; DWORD color; float u,v; };
    const Vertex v[]={{-0.5f,-0.5f,0,1,0xffffffff,0,0},{63.5f,-0.5f,0,1,0xffffffff,1,0},
                      {-0.5f,63.5f,0,1,0xffffffff,0,1},{63.5f,63.5f,0,1,0xffffffff,1,1}};
    CHECK(SUCCEEDED(d->SetTexture(0,destination)));
    CHECK(SUCCEEDED(d->SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_TEX1)));
    CHECK(SUCCEEDED(d->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1)));
    CHECK(SUCCEEDED(d->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE)));
    CHECK(SUCCEEDED(d->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1)));
    CHECK(SUCCEEDED(d->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE)));
    CHECK(SUCCEEDED(d->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_POINT)));
    CHECK(SUCCEEDED(d->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_POINT)));
    CHECK(SUCCEEDED(d->SetRenderState(D3DRS_ZENABLE,FALSE)));
    CHECK(SUCCEEDED(d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE)));
    CHECK(SUCCEEDED(d->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE)));
    CHECK(SUCCEEDED(d->Clear(0,nullptr,D3DCLEAR_TARGET,0xff000000,1.0f,0)));
    CHECK(SUCCEEDED(d->BeginScene())); CHECK(SUCCEEDED(d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,v,sizeof(Vertex))));
    CHECK(SUCCEEDED(d->EndScene())); CHECK(SUCCEEDED(d->SetTexture(0,nullptr)));
  }
  IDirect3DSurface9 *target=nullptr,*copy=nullptr;
  CHECK(SUCCEEDED(d->GetRenderTarget(0,&target)));
  CHECK(SUCCEEDED(d->CreateOffscreenPlainSurface(64,64,D3DFMT_X8R8G8B8,D3DPOOL_SYSTEMMEM,&copy,nullptr)));
  if(target && copy) {
    CHECK(SUCCEEDED(d->GetRenderTargetData(target,copy)));
    D3DLOCKED_RECT r{}; CHECK(SUCCEEDED(copy->LockRect(&r,nullptr,D3DLOCK_READONLY)));
    if(r.pBits) CHECK((reinterpret_cast<const DWORD*>(static_cast<const char*>(r.pBits)+r.Pitch*32)[32]&0xffffff)==0x3a6b9c);
    CHECK(SUCCEEDED(copy->UnlockRect()));
  }
  if(copy) copy->Release(); if(target) target->Release();
  if(destination) destination->Release(); if(source) source->Release();
}
static UINT64 TestNowNs() {
  LARGE_INTEGER counter{},frequency{};
  QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&counter);
  const UINT64 t=UINT64(counter.QuadPart), f=UINT64(frequency.QuadPart);
  return (t/f)*1000000000ull+(t%f)*1000000000ull/f;
}
static void TestPresentLatency(HMODULE module) {
  using CreateEx = HRESULT (WINAPI*)(UINT,IDirect3D9Ex**);
  auto createEx=reinterpret_cast<CreateEx>(GetProcAddress(module,"Direct3DCreate9Ex"));
  CHECK(createEx!=nullptr); if(!createEx) return;
  IDirect3D9Ex* factory=nullptr; CHECK(SUCCEEDED(createEx(D3D_SDK_VERSION,&factory)) && factory);
  if(!factory) return;
  HWND window=CreateWindowA("STATIC","Present latency fixture",WS_POPUP,0,0,256,256,nullptr,nullptr,GetModuleHandle(nullptr),nullptr);
  CHECK(window!=nullptr);
  ShowWindow(window,SW_SHOWNOACTIVATE); UpdateWindow(window);
  D3DPRESENT_PARAMETERS pp{}; pp.Windowed=TRUE; pp.SwapEffect=D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow=window; pp.BackBufferWidth=256; pp.BackBufferHeight=256;
  pp.BackBufferFormat=D3DFMT_X8R8G8B8; pp.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
  IDirect3DDevice9Ex* device=nullptr;
  auto hr=factory->CreateDeviceEx(0,D3DDEVTYPE_HAL,window,D3DCREATE_HARDWARE_VERTEXPROCESSING|D3DCREATE_MULTITHREADED,
    &pp,nullptr,&device);
  CHECK(SUCCEEDED(hr) && device);
  if(device) {
    CHECK(SUCCEEDED(device->SetMaximumFrameLatency(1)));
    ID3D9BridgeTelemetry* t=nullptr;
    CHECK(device->QueryInterface(IID_D3D9BridgeTelemetry,reinterpret_cast<void**>(&t))==S_OK && t);
    if(t) {
      auto c=Config(1,65536); CHECK(t->StartCapture(&c)==S_OK);
      // No frame has been queued, so the existing latency fence is already
      // satisfied. The no-op SetMaximumFrameLatency wait must not be logged.
      CHECK(SUCCEEDED(device->SetMaximumFrameLatency(1)));
      D3D9BridgeTelemetryEvent initial[128]{};
      auto initialRead=Read(t,1,initial,128);
      for(UINT i=0;i<initialRead.Written;i++) CHECK(initial[i].Kind!=9);
      struct Interval { UINT64 begin,end; }; std::vector<Interval> intervals;
      unsigned successfulPresents=0;
      for(unsigned i=0;i<80;i++) {
        // Keep the real GPU/presenter queue occupied before each Present.
        for(unsigned j=0;j<16;j++) CHECK(SUCCEEDED(device->Clear(0,nullptr,D3DCLEAR_TARGET,0xff000000u|(i<<8)|j,1.0f,0)));
        const auto begin=TestNowNs(); hr=device->PresentEx(nullptr,nullptr,nullptr,nullptr,0); const auto end=TestNowNs();
        successfulPresents+=hr==S_OK; CHECK(hr==S_OK); intervals.push_back({begin,end});
      }
      CHECK(t->StopCapture(1)==S_OK);
      std::vector<D3D9BridgeTelemetryEvent> events(65536);
      auto read=Read(t,1,events.data(),UINT(events.size()));
      unsigned frameWaits=0,withinPresent=0,acquires=0,acquiresWithinPresent=0;
      UINT64 maxWaitNs=0,maxPresentNs=0;
      for(const auto& interval:intervals) maxPresentNs=std::max(maxPresentNs,interval.end-interval.begin);
      constexpr UINT frameLatencyKind=9; // Private contract expected by this regression.
      constexpr UINT presentAcquireKind=10;
      for(UINT i=0;i<read.Written;i++) {
        const auto& event=events[i];
        if(event.Kind==frameLatencyKind || event.Kind==presentAcquireKind) {
          const bool frame=event.Kind==frameLatencyKind;
          if(frame) { ++frameWaits; maxWaitNs=std::max(maxWaitNs,event.EndNs-event.BeginNs); } else ++acquires;
          CHECK(event.Identity!=0 && event.ThreadId==GetCurrentThreadId() && event.EndNs>=event.BeginNs);
          for(const auto& interval:intervals) if(event.BeginNs>=interval.begin && event.EndNs<=interval.end) {
            if(frame) ++withinPresent; else ++acquiresWithinPresent;
            break;
          }
        }
      }
      std::printf("present_latency successful_presents=%u frame_waits=%u within_present=%u acquires=%u acquires_within_present=%u max_frame_wait_ns=%llu max_present_ns=%llu dropped=%llu\n",
        successfulPresents,frameWaits,withinPresent,acquires,acquiresWithinPresent,maxWaitNs,maxPresentNs,read.Dropped);
      CHECK(frameWaits>0 && withinPresent==frameWaits && read.Dropped==0);
      CHECK(acquires>=intervals.size() && acquiresWithinPresent==acquires);
      t->Release();
    }
    device->Release();
  }
  factory->Release(); if(window) DestroyWindow(window);
}
static void Test(IDirect3DDevice9* d, ID3D9BridgeTelemetry* t, IDirect3D9* factory, HWND window, D3DPRESENT_PARAMETERS pp) {
  auto c=Config(101);
  CHECK(t->StartCapture(nullptr)==E_POINTER);
  auto bad=c; bad.StructSize=0; CHECK(FAILED(t->StartCapture(&bad)));
  bad=c; bad.Version=2; CHECK(FAILED(t->StartCapture(&bad)));
  bad=c; bad.CaptureId=0; CHECK(FAILED(t->StartCapture(&bad)));
  bad=c; bad.MaxEvents=0; CHECK(FAILED(t->StartCapture(&bad)));
  bad=c; bad.MaxEvents=D3D9_BRIDGE_TELEMETRY_MAX_EVENTS+1; CHECK(FAILED(t->StartCapture(&bad)));
  CHECK(t->StartCapture(&c)==S_OK); CHECK(FAILED(t->StartCapture(&c)));
  CHECK(FAILED(t->StopCapture(102)));
  D3D9BridgeTelemetryRead invalid{}; invalid.StructSize=sizeof(invalid); invalid.Version=1;
  CHECK(FAILED(t->ReadEvents(102,&invalid,nullptr,0)));
  CHECK(FAILED(t->ReadEvents(101,&invalid,nullptr,1)));
  auto before=Snapshot(t,101,nullptr,0,nullptr,0);
  CHECK(before.TotalChunks==0 || (before.Flags&D3D9_BRIDGE_MEMORY_CHUNKS_TRUNCATED));
  Work(d,12); CheckPixels(d); Fence(d);
  std::vector<D3D9BridgeMemoryChunk> chunks(4096);
  std::vector<UINT> masks(65536);
  auto base=Snapshot(t,101,chunks.data(),UINT(chunks.size()),masks.data(),UINT(masks.size()));
  std::vector<D3D9BridgeMemoryChunk> baseChunks(chunks.begin(),chunks.begin()+base.CopiedChunks);
  std::vector<UINT> baseMasks(masks.begin(),masks.begin()+base.CopiedMasks);
  std::vector<IDirect3DTexture9*> held;
  for(unsigned i=0;i<8;i++) {
    IDirect3DTexture9* texture=nullptr;
    CHECK(SUCCEEDED(d->CreateTexture(1024,1024,1,0,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&texture,nullptr)));
    if(texture) held.push_back(texture);
  }
  Fence(d);
  auto snap=Snapshot(t,101,chunks.data(),UINT(chunks.size()),masks.data(),UINT(masks.size()));
  CHECK(snap.TimestampNs>0 && snap.TotalChunks>=snap.CopiedChunks && snap.TotalMasks>=snap.CopiedMasks);
  CHECK(snap.CopiedChunks>0 && snap.TotalCapacity>=snap.TotalUsed);
  for(UINT i=0;i<snap.CopiedChunks;i++) {
    const auto& x=chunks[i]; CHECK(x.Cookie && x.Used<=x.Capacity);
    CHECK(x.MaskOffset<=snap.CopiedMasks && x.MaskCount<=snap.CopiedMasks-x.MaskOffset);
  }
  CHECK(snap.TotalUsed>base.TotalUsed);
  bool sameCookieChanged=false;
  for(UINT i=0;i<snap.CopiedChunks;i++) for(const auto& old:baseChunks)
    if(chunks[i].Cookie==old.Cookie && (chunks[i].Used!=old.Used || chunks[i].PageCount!=old.PageCount ||
       (chunks[i].MaskCount==old.MaskCount && chunks[i].MaskCount &&
        std::memcmp(masks.data()+chunks[i].MaskOffset,baseMasks.data()+old.MaskOffset,old.MaskCount*sizeof(UINT))!=0)))
      sameCookieChanged=true;
  CHECK(sameCookieChanged || snap.TotalChunks!=base.TotalChunks);
  D3D9BridgeMemoryChunk tiny{};
  auto truncated=Snapshot(t,101,&tiny,1,nullptr,0);
  CHECK(truncated.TotalChunks<=1 || (truncated.Flags&D3D9_BRIDGE_MEMORY_CHUNKS_TRUNCATED));
  CHECK(!truncated.TotalMasks || (truncated.Flags&D3D9_BRIDGE_MEMORY_MASKS_TRUNCATED));
  for(auto* texture:held) texture->Release();
  Fence(d);
  auto released=Snapshot(t,101,chunks.data(),UINT(chunks.size()),masks.data(),UINT(masks.size()));
  CHECK(released.TotalUsed<snap.TotalUsed);
  // Exceeds one quarter of a 256 MiB device pool, forcing a dedicated
  // backing allocation on supported devices; releasing and fencing it
  // exercises an actual Vulkan free during the capture.
  IDirect3DTexture9* large=nullptr;
  CHECK(SUCCEEDED(d->CreateTexture(4608,4096,1,0,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&large,nullptr)));
  if(large) large->Release(); Fence(d);
  CHECK(t->StopCapture(101)==S_OK); CHECK(t->StopCapture(101)==S_FALSE);
  CHECK(FAILED(t->StartCapture(&c)));
  D3D9BridgeMemorySnapshot stopped{}; stopped.StructSize=sizeof(stopped); stopped.Version=1;
  CHECK(FAILED(t->GetMemorySnapshot(101,&stopped,nullptr,0,nullptr,0)));
  std::vector<D3D9BridgeTelemetryEvent> events(16384);
  auto r=Read(t,101,events.data(),UINT(events.size()));
  CHECK(r.Written>0 && !r.Active && r.Filtered==0 && r.Dropped==0);
  unsigned kinds[9]={}; unsigned resourceAlloc=0,resourceFree=0,deviceAlloc=0,deviceFree=0;
  bool fileMap=false,fileUnmap=false,osMap=false,osUnmap=false,matchedOs=false,matchedCache=false;
  for(UINT i=0;i<r.Written;i++) {
    const auto& e=events[i]; CHECK(e.CaptureId==101 && e.BeginNs && e.EndNs>=e.BeginNs && e.ThreadId);
    if(e.Kind<9) kinds[e.Kind]++;
    if(e.Flags&D3D9_BRIDGE_TELEMETRY_RESOURCE) {
      resourceAlloc+=e.Kind==D3D9_BRIDGE_TELEMETRY_ALLOC;
      resourceFree+=e.Kind==D3D9_BRIDGE_TELEMETRY_FREE;
      if(e.Flags&D3D9_BRIDGE_TELEMETRY_CACHE_RETURN)
        for(UINT j=0;j<i;j++) if((events[j].Flags&D3D9_BRIDGE_TELEMETRY_CACHE_HIT) &&
            events[j].Identity==e.Identity) matchedCache=true;
    }
    if(e.Flags&D3D9_BRIDGE_TELEMETRY_DEVICE_MEMORY) {
      deviceAlloc+=e.Kind==D3D9_BRIDGE_TELEMETRY_ALLOC;
      deviceFree+=e.Kind==D3D9_BRIDGE_TELEMETRY_FREE;
    }
    if(e.Flags&D3D9_BRIDGE_TELEMETRY_FILE_BACKED) {
      fileMap |= e.Kind==D3D9_BRIDGE_TELEMETRY_MAP;
      fileUnmap |= e.Kind==D3D9_BRIDGE_TELEMETRY_UNMAP;
      if(e.Flags&D3D9_BRIDGE_TELEMETRY_OS_MAPPING) {
        osMap |= e.Kind==D3D9_BRIDGE_TELEMETRY_MAP;
        osUnmap |= e.Kind==D3D9_BRIDGE_TELEMETRY_UNMAP;
        if(e.Kind==D3D9_BRIDGE_TELEMETRY_UNMAP)
          for(UINT j=0;j<i;j++) if(events[j].Kind==D3D9_BRIDGE_TELEMETRY_MAP &&
             (events[j].Flags&D3D9_BRIDGE_TELEMETRY_OS_MAPPING) && events[j].Identity==e.Identity) matchedOs=true;
      }
    }
  }
  CHECK(kinds[D3D9_BRIDGE_TELEMETRY_ALLOC]>0);
  CHECK(kinds[D3D9_BRIDGE_TELEMETRY_FREE]>0);
  CHECK(kinds[D3D9_BRIDGE_TELEMETRY_CS_WAIT]>0);
  CHECK(fileMap && fileUnmap);
  CHECK(osMap && osUnmap && matchedOs);
  CHECK(resourceAlloc && resourceFree && deviceAlloc && deviceFree);
  // D3D9 never passes a local allocation cache; the sole frontend using it
  // is D3D11, which is disabled in this backend. Report it rather than claim
  // coverage of a path this fixture cannot reach.
  std::printf("events=%u dropped=%llu filtered=%llu alloc=%u free=%u map=%u unmap=%u cs=%u resource=%u gpu=%u chunks=%u masks=%u fileMap=%u fileUnmap=%u\n",
    r.Written,r.Dropped,r.Filtered,kinds[1],kinds[2],kinds[3],kinds[4],kinds[5],kinds[6],kinds[7],
    snap.CopiedChunks,snap.CopiedMasks,unsigned(fileMap),unsigned(fileUnmap));
  std::printf("resource_alloc=%u resource_free=%u device_alloc=%u device_free=%u os_map=%u os_unmap=%u matched_os=%u matched_cache=%u snapshot_used_before=%llu held=%llu released=%llu\n",
    resourceAlloc,resourceFree,deviceAlloc,deviceFree,unsigned(osMap),unsigned(osUnmap),unsigned(matchedOs),unsigned(matchedCache),
    base.TotalUsed,snap.TotalUsed,released.TotalUsed);
  CHECK(Read(t,101).Remaining==0);
  c=Config(102,2); CHECK(t->StartCapture(&c)==S_OK);
  CHECK(FAILED(t->ReadEvents(101,&invalid,nullptr,0))); Work(d,4);
  r=Read(t,102); CHECK(r.Remaining==2 && r.Dropped>0);
  CHECK(t->StopCapture(102)==S_OK);
  c=Config(103); c.MinBytes=~UINT64(0); c.MinDurationNs=~UINT64(0);
  CHECK(t->StartCapture(&c)==S_OK); Work(d,2); CHECK(t->StopCapture(103)==S_OK);
  r=Read(t,103); CHECK(r.Filtered>0);
  // Actual resource calls overlap Stop/Start; a completion from a prior
  // generation must never acquire the next generation's identity.
  std::atomic<bool> run{true};
  std::thread worker([&] { while(run.load()) Work(d,1); });
  for(UINT64 id=200;id<220;id++) {
    c=Config(id,4096); CHECK(t->StartCapture(&c)==S_OK); Sleep(2); CHECK(t->StopCapture(id)==S_OK);
    auto copy=Read(t,id,events.data(),4096);
    for(UINT i=0;i<copy.Written;i++) CHECK(events[i].CaptureId==id);
  }
  run.store(false); worker.join();
  Fence(d);
  c=Config(400); CHECK(t->StartCapture(&c)==S_OK);
  CHECK(SUCCEEDED(d->Reset(&pp))); Work(d,1);
  CHECK(t->StopCapture(400)==S_OK);
  IDirect3DDevice9* secondary=nullptr;
  CHECK(SUCCEEDED(factory->CreateDevice(0,D3DDEVTYPE_HAL,window,D3DCREATE_HARDWARE_VERTEXPROCESSING|D3DCREATE_MULTITHREADED,&pp,&secondary)));
  ID3D9BridgeTelemetry* other=nullptr;
  if(secondary) CHECK(secondary->QueryInterface(IID_D3D9BridgeTelemetry,reinterpret_cast<void**>(&other))==S_OK && other);
  if(other) {
    c=Config(500); CHECK(t->StartCapture(&c)==S_OK); CHECK(other->StartCapture(&c)==S_OK);
    Work(d,2); Work(secondary,2); CHECK(t->StopCapture(500)==S_OK); CHECK(other->StopCapture(500)==S_OK);
    auto primary=Read(t,500,events.data(),4096); std::vector<D3D9BridgeTelemetryEvent> first(events.begin(),events.begin()+primary.Written);
    auto second=Read(other,500,events.data(),4096);
    CHECK(primary.Written && second.Written);
    for(UINT i=0;i<second.Written;i++) if((events[i].Flags&D3D9_BRIDGE_TELEMETRY_FILE_BACKED) &&
        !(events[i].Flags&D3D9_BRIDGE_TELEMETRY_OS_MAPPING) && events[i].Identity)
      for(const auto& e:first) if((e.Flags&D3D9_BRIDGE_TELEMETRY_FILE_BACKED) &&
          !(e.Flags&D3D9_BRIDGE_TELEMETRY_OS_MAPPING)) CHECK(e.Identity!=events[i].Identity);
    other->Release();
  }
  if(secondary) secondary->Release();
}

int main(int argc,char** argv) {
  std::setvbuf(stdout,nullptr,_IONBF,0);
  if(argc<2 || argc>4) return 2;
  auto module=LoadLibraryA(argv[1]);
  if(!module) { std::printf("FAIL LoadLibrary %lu\n",GetLastError()); return 2; }
  auto create=reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(module,"Direct3DCreate9"));
  auto factory=create?create(D3D_SDK_VERSION):nullptr; if(!factory) return 2;
  auto window=CreateWindowA("STATIC","Telemetry fixture",WS_POPUP,0,0,64,64,nullptr,nullptr,GetModuleHandle(nullptr),nullptr);
  D3DPRESENT_PARAMETERS pp{}; pp.Windowed=TRUE; pp.SwapEffect=D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow=window; pp.BackBufferWidth=64; pp.BackBufferHeight=64;
  pp.BackBufferFormat=D3DFMT_X8R8G8B8; pp.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
  IDirect3DDevice9* device=nullptr;
  auto hr=factory->CreateDevice(0,D3DDEVTYPE_HAL,window,D3DCREATE_HARDWARE_VERTEXPROCESSING|D3DCREATE_MULTITHREADED,&pp,&device);
  if(FAILED(hr)) { std::printf("FAIL CreateDevice 0x%08lx\n",(unsigned long)hr); return 2; }
  ID3D9BridgeTelemetry* telemetry=nullptr;
  hr=device->QueryInterface(IID_D3D9BridgeTelemetry,reinterpret_cast<void**>(&telemetry));
  std::printf("telemetry QI hr=0x%08lx\n",(unsigned long)hr);
  if(argc>=3 && std::strcmp(argv[2],"--benchmark")==0) {
    if(argc!=4 || (std::strcmp(argv[3],"on") && std::strcmp(argv[3],"off") && std::strcmp(argv[3],"aa"))) return 2;
    bool active=std::strcmp(argv[3],"on")==0;
    if(active && !telemetry) { std::puts("benchmark unavailable=telemetry-interface"); return 3; }
    ShowWindow(window,SW_SHOWNOACTIVATE); UpdateWindow(window);
    for(unsigned i=0;i<4;i++) { Work(device,1,false); CHECK(device->Present(nullptr,nullptr,nullptr,nullptr)==S_OK); }
    Fence(device);
    if(active) { auto c=Config(9001,65536); CHECK(telemetry->StartCapture(&c)==S_OK); }
    LARGE_INTEGER f{},a{},b{},fenceEnd{}; ULONGLONG ca=0,cb=0;
    QueryPerformanceFrequency(&f); QueryThreadCycleTime(GetCurrentThread(),&ca); QueryPerformanceCounter(&a);
    constexpr unsigned iterations=300;
    unsigned successfulPresents=0;
    for(unsigned i=0;i<iterations;i++) {
      Work(device,1,false); auto result=device->Present(nullptr,nullptr,nullptr,nullptr);
      successfulPresents+=result==S_OK; CHECK(result==S_OK);
    }
    QueryPerformanceCounter(&b); QueryThreadCycleTime(GetCurrentThread(),&cb);
    Fence(device); QueryPerformanceCounter(&fenceEnd);
    D3D9BridgeTelemetryRead r{};
    if(active) { CHECK(telemetry->StopCapture(9001)==S_OK); r=Read(telemetry,9001); }
    std::printf("{\"variant\":\"%s\",\"supported\":%u,\"iterations\":%u,\"successful_presents\":%u,\"wall_ns_per_iteration\":%.3f,\"thread_cycles_per_iteration\":%.3f,\"separate_fence_ns\":%.3f,\"events\":%u,\"filtered\":%llu,\"dropped\":%llu,\"inflight\":%llu,\"failures\":%u}\n",
      argv[3],unsigned(telemetry!=nullptr),iterations,successfulPresents,double(b.QuadPart-a.QuadPart)*1e9/double(f.QuadPart)/iterations,
      double(cb-ca)/iterations,double(fenceEnd.QuadPart-b.QuadPart)*1e9/double(f.QuadPart),
      r.Remaining,r.Filtered,r.Dropped,r.InFlightAtStop,failures.load());
  } else if(argc==3 && std::strcmp(argv[2],"--present")==0) {
    CHECK(hr==S_OK && telemetry);
    if(telemetry) TestPresentLatency(module);
  } else {
    CHECK(hr==S_OK && telemetry);
    if(telemetry) { TestRecorderGenerations(); Test(device,telemetry,factory,window,pp); TestPresentLatency(module); }
  }
  if(telemetry) telemetry->Release(); device->Release(); factory->Release(); DestroyWindow(window); FreeLibrary(module);
  std::printf("checks=%u failures=%u\n",checks.load(),failures.load()); return failures.load()?1:0;
}
