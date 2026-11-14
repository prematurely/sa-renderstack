#define main UploadMappingLegacyMain
#include "upload-mapping-probe.cpp"
#undef main
#include "lib/vulkan-dispatch-observer.h"
#include "../backend/dxvk/src/d3d9/d3d9_bridge_telemetry.h"
#include <atomic>

static std::atomic<bool> delayBegin{false};
static void DelayBegin(VkCommandBuffer) {if(delayBegin.exchange(false))Sleep(25);}
static void DelayAllocate() {if(delayBegin.exchange(false))Sleep(25);}
static void WaitCorrelation(IDirect3DDevice9* d,bool allocation) {
  IDirect3DSurface9 *rt=nullptr,*copy=nullptr;CHECK(d->GetRenderTarget(0,&rt)==S_OK);
  CHECK(d->CreateOffscreenPlainSurface(32,32,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&copy,nullptr)==S_OK);Flush(d);
  ID3D9BridgeTelemetry* t=nullptr;CHECK(d->QueryInterface(IID_D3D9BridgeTelemetry,reinterpret_cast<void**>(&t))==S_OK);if(!t)return;
  D3D9BridgeTelemetryConfig c{};c.StructSize=sizeof(c);c.Version=1;c.CaptureId=1;c.MaxEvents=65536;CHECK(t->StartCapture(&c)==S_OK);
  if(allocation)command_observer::beforeAllocate=&DelayAllocate;else command_observer::beforeBegin=&DelayBegin;delayBegin=true;
  CHECK(d->Clear(0,nullptr,D3DCLEAR_TARGET,0xff987654,1,0)==S_OK);CHECK(d->GetRenderTargetData(rt,copy)==S_OK);
  auto lockBegin=Milliseconds();D3DLOCKED_RECT a{};CHECK(copy->LockRect(&a,nullptr,D3DLOCK_READONLY)==S_OK);auto lockEnd=Milliseconds();
  CHECK(a.pBits && *static_cast<DWORD*>(a.pBits)==0xff987654);CHECK(copy->UnlockRect()==S_OK);CHECK(!delayBegin.load());
  Flush(d);command_observer::beforeBegin=nullptr;command_observer::beforeAllocate=nullptr;CHECK(t->StopCapture(1)==S_OK);
  D3D9BridgeTelemetryRead r{};r.StructSize=sizeof(r);r.Version=1;std::vector<D3D9BridgeTelemetryEvent> events(65536);
  CHECK(t->ReadEvents(1,&r,events.data(),unsigned(events.size()))==S_OK);CHECK(!r.Dropped && !r.LateDropped && !r.Remaining);events.resize(r.Written);
  bool correlated=false,driver=false,chunk=false;unsigned waits=0;UINT64 overlap=0;
  for(const auto& e:events){
    std::printf("telemetryEvent captureId=1 kind=%u beginNs=%llu endNs=%llu threadId=%u identity=%llu bytes=%llu result=%d flags=%u\n",e.Kind,e.BeginNs,e.EndNs,e.ThreadId,e.Identity,e.Bytes,e.Result,e.Flags);
    if(e.Kind==D3D9_BRIDGE_TELEMETRY_CS_WAIT)++waits;
    if(e.Kind!=(allocation?D3D9_BRIDGE_TELEMETRY_COMMAND_ALLOCATE:D3D9_BRIDGE_TELEMETRY_COMMAND_BEGIN) || e.EndNs-e.BeginNs<20000000)continue;
    driver=true;CHECK(e.Result==0 && e.ThreadId!=GetCurrentThreadId());
    for(const auto& work:events)if(work.Kind==D3D9_BRIDGE_TELEMETRY_CS_CHUNK && work.ThreadId==e.ThreadId && work.BeginNs<=e.BeginNs && work.EndNs>=e.EndNs){
      chunk=true;
      for(const auto& wait:events)if(wait.Kind==D3D9_BRIDGE_TELEMETRY_CS_WAIT && wait.ThreadId==GetCurrentThreadId() && !(wait.Flags&D3D9_BRIDGE_TELEMETRY_HIGH_PRIORITY) && !(work.Flags&D3D9_BRIDGE_TELEMETRY_HIGH_PRIORITY) && work.Identity && work.Identity<=wait.Identity){
        auto lo=std::max(e.BeginNs,wait.BeginNs),hi=std::min(e.EndNs,wait.EndNs);if(hi>lo){overlap=std::max(overlap,hi-lo);correlated|=hi-lo>=10000000;}
      }
    }
  }
  CHECK(driver && chunk && correlated && waits>0);
  std::printf("controlled-driver-delay lockMs=%.6f overlapNs=%llu driver=%u chunk=%u correlated=%u\n",lockEnd-lockBegin,overlap,unsigned(driver),unsigned(chunk),unsigned(correlated));
  t->Release();copy->Release();rt->Release();Flush(d);
}

static void Work(IDirect3DDevice9* d,unsigned iterations,bool timed,bool upload) {
  const unsigned before=failures;
  IDirect3DTexture9* src[8]{};IDirect3DTexture9* gpu[8]{};
  IDirect3DSurface9 *rt=nullptr,*copy=nullptr;
  CHECK(d->GetRenderTarget(0,&rt)==S_OK);
  CHECK(d->CreateOffscreenPlainSurface(32,32,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&copy,nullptr)==S_OK);
  std::vector<double> locks;locks.reserve(iterations);
  ULONG64 c0=0,c1=0;QueryThreadCycleTime(GetCurrentThread(),&c0);
  auto start=Milliseconds();
  if(upload)for(unsigned j=0;j<8;++j){CHECK(d->CreateTexture(256,256,1,0,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&src[j],nullptr)==S_OK);CHECK(d->CreateTexture(256,256,1,0,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&gpu[j],nullptr)==S_OK);}
  for(unsigned i=0;i<iterations;++i) {
    if(upload)for(unsigned j=0;j<8;++j){Fill(src[j],i%6);CHECK(d->UpdateTexture(src[j],gpu[j])==S_OK);}
    const auto color=Color(0,i%6);
    CHECK(d->Clear(0,nullptr,D3DCLEAR_TARGET,color,1,0)==S_OK);
    CHECK(d->GetRenderTargetData(rt,copy)==S_OK);
    // A later GPU write must not change the result of the queued readback.
    CHECK(d->Clear(0,nullptr,D3DCLEAR_TARGET,color^0x0000ffff,1,0)==S_OK);
    D3DLOCKED_RECT a{};auto begin=Milliseconds();
    CHECK(copy->LockRect(&a,nullptr,D3DLOCK_READONLY)==S_OK);locks.push_back(Milliseconds()-begin);
    bool exact=a.pBits!=nullptr;
    if(a.pBits){for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x)exact &= reinterpret_cast<DWORD*>(static_cast<char*>(a.pBits)+y*a.Pitch)[x]==color;CHECK(copy->UnlockRect()==S_OK);}
    CHECK(exact);
  }
  for(auto p:src)if(p)p->Release();for(auto p:gpu)if(p)p->Release();copy->Release();rt->Release();Flush(d);
  const auto total=Milliseconds()-start;QueryThreadCycleTime(GetCurrentThread(),&c1);
  if(timed){std::sort(locks.begin(),locks.end());double sum=0;for(auto n:locks)sum+=n;
    std::printf("{\"type\":\"lockrect-pool-benchmark\",\"workload\":\"%s\",\"iterations\":%u,\"valid\":%s,\"totalMs\":%.6f,\"mainThreadCycles\":%llu,\"lockSumMs\":%.6f,\"lockP95Ms\":%.6f,\"lockMaxMs\":%.6f,\"observer\":false}\n",upload?"upload-readback":"readback",iterations,failures==before?"true":"false",total,static_cast<unsigned long long>(c1-c0),sum,locks[(locks.size()-1)*95/100],locks.back());}
}
int main(int argc,char** argv) {
  std::setvbuf(stdout,nullptr,_IONBF,0);if(argc<3)return 2;
  std::string mode=argv[2];bool timed=mode=="bench";bool upload=argc<4 || std::strcmp(argv[3],"readback");
  unsigned iterations=argc>4?std::strtoul(argv[4],nullptr,10):48;
  if(!iterations || iterations>4096 || (mode!="observe" && mode!="regression" && mode!="correlation" && mode!="correlation-allocate" && !timed))return 2;
  SetEnvironmentVariableA("DXVK_CONFIG","d3d9.textureMemory=100;d3d9.presentInterval=0;d3d9.gtaSaCompat=False;dxvk.hud=");
  SetEnvironmentVariableA("DXVK_CONFIG_FILE","lockrect-no-external.conf");SetEnvironmentVariableA("DXVK_LOG_LEVEL","error");SetEnvironmentVariableA("DXVK_HUD","0");
  auto lib=LoadLibraryA(argv[1]);CHECK(lib);if(!lib)return 1;
  {
    ImportHook hook;if(!timed){CHECK(hook.install(lib,"GetProcAddress",reinterpret_cast<void*>(&command_observer::GetProc)));command_observer::getProc=reinterpret_cast<decltype(&GetProcAddress)>(hook.original);}
    using Factory=IDirect3D9* (WINAPI*)(UINT);auto make=reinterpret_cast<Factory>(GetProcAddress(lib,"Direct3DCreate9"));CHECK(make);auto factory=make(D3D_SDK_VERSION);CHECK(factory);
    HWND w=CreateWindowA("STATIC","lockrect-pool",WS_POPUP,0,0,64,64,nullptr,nullptr,GetModuleHandleA(nullptr),nullptr);CHECK(w);
    D3DPRESENT_PARAMETERS pp{};pp.BackBufferWidth=pp.BackBufferHeight=32;pp.BackBufferFormat=D3DFMT_A8R8G8B8;pp.BackBufferCount=1;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.Windowed=TRUE;pp.hDeviceWindow=w;pp.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* d=nullptr;CHECK(factory->CreateDevice(0,D3DDEVTYPE_HAL,w,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&d)==S_OK);
    if(d){if(timed){Work(d,16,false,upload);BenchmarkThreadAffinity affinity;CHECK(affinity.valid());Work(d,iterations,true,upload);}else if(mode=="correlation" || mode=="correlation-allocate"){WaitCorrelation(d,mode=="correlation-allocate");CHECK(d->Reset(&pp)==S_OK);}else{Work(d,iterations,false,true);CHECK(d->Reset(&pp)==S_OK);Work(d,12,false,false);}CHECK(d->Release()==0);}
    factory->Release();DestroyWindow(w);
    if(!timed){using namespace command_observer;std::printf("driver primaryCalls=%u buffers=%u batches=%u resets=%u destroyed=%u maxSpare=%u errors=%u livePools=%u\n",calls,buffers,batches,resets,destroyed,maxSpare,errors,unsigned(pools.size()));CHECK(calls>0 && resets>0 && destroyed>0);CHECK(errors==0 && pools.empty() && owners.empty());}
  }
  FreeLibrary(lib);std::printf("checks=%u failures=%u\n",checks,failures);return failures?1:0;
}
