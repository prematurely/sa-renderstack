// Reuse the existing real upload/readback checks, rather than another pixel model.
#define main UploadMappingLegacyMain
#include "upload-mapping-probe.cpp"
#undef main
#include "../backend/dxvk/src/d3d9/d3d9_bridge_telemetry.h"
#include <unordered_map>
#include <atomic>
#include <thread>
#include <psapi.h>
#include <condition_variable>
#include <deque>
#include <mutex>

namespace budget_probe {
using U64 = unsigned long long;
static U64 frequency;
static U64 Now() { LARGE_INTEGER q; QueryPerformanceCounter(&q); return U64(q.QuadPart); }
static U64 Ns(U64 ticks) { return ticks / frequency * 1000000000ull + ticks % frequency * 1000000000ull / frequency; }
struct OsCounters { U64 maps=0, unmaps=0, mapTicks=0, unmapTicks=0, burst=0, burstTicks=0, maxBurst=0, maxBurstTicks=0; };
static OsCounters os;
static bool counting;
static std::atomic<bool> inspectRelease{false};
static std::atomic<bool> watchRetirement{false}, heldRetirement{false};
static HANDLE retirementReached, retirementGate, retirementFinished;
static std::atomic<unsigned> retiredOffThread{0};
struct ReleaseView { U64 bytes=0, pages=0, resident=0, queryNs=0, unmapNs=0; };
static std::vector<ReleaseView> releaseViews;
static std::atomic<unsigned> releaseQueryErrors{0};
static std::mutex observationMutex;
// Track a real file-close barrier so asynchronous cleanup remains inside the
// lifetime timer. The sentinel is released last, on the same pool as textures.
using CloseMapping=decltype(&CloseHandle);
static CloseMapping originalClose;
static std::atomic<HANDLE> barrierFile{nullptr};
static HANDLE barrierEvent;
static bool findSentinelFile;
static HANDLE sentinelFile;
static std::atomic<bool> countWorker{false};
static std::atomic<U64> workerCalls{0},workerTicks{0};
static BOOL WINAPI CloseHook(HANDLE h){auto result=originalClose(h);if(result && h==barrierFile.load())SetEvent(barrierEvent);return result;}
using WorkingSetQuery = BOOL (WINAPI*)(HANDLE,PVOID,DWORD);
static ReleaseView InspectView(LPCVOID p) {
  ReleaseView result;const auto begin=Now();
  static auto query=reinterpret_cast<WorkingSetQuery>(GetProcAddress(GetModuleHandleA("kernel32.dll"),"K32QueryWorkingSetEx"));
  MEMORY_BASIC_INFORMATION info{};
  if(!query || VirtualQuery(p,&info,sizeof(info))!=sizeof(info) || info.AllocationBase!=p){++releaseQueryErrors;return result;}
  result.bytes=info.RegionSize;result.pages=(info.RegionSize+4095)/4096;
  std::vector<PSAPI_WORKING_SET_EX_INFORMATION> entries(result.pages);
  for(size_t i=0;i<entries.size();++i)entries[i].VirtualAddress=const_cast<char*>(static_cast<const char*>(p))+i*4096;
  if(!query(GetCurrentProcess(),entries.data(),DWORD(entries.size()*sizeof(entries[0]))))++releaseQueryErrors;
  else for(const auto& page:entries)result.resident+=page.VirtualAttributes.Valid;
  result.queryNs=Ns(Now()-begin);return result;
}
using SetMemory = void* (__cdecl*)(void*, int, size_t);
static SetMemory originalSet;
static U64 zeroBytes;
static void* __cdecl SetHook(void* p, int value, size_t bytes) {
  if (GetCurrentThreadId()==ownerThread && counting && value==0 && bytes>=65536) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(p,&info,sizeof(info))==sizeof(info) && info.Type==MEM_MAPPED)
      zeroBytes+=bytes;
  }
  return originalSet(p,value,bytes);
}
static U64 nextCapture;
using CreateMapping = decltype(&CreateFileMappingA);
static CreateMapping originalCreateMapping;
static bool failNextCreateMapping;
static bool failNextMap;
using CreateWorker=decltype(&CreateThread);
static CreateWorker originalCreateWorker;
static bool failNextWorker;
static HANDLE WINAPI CreateWorkerHook(LPSECURITY_ATTRIBUTES s,SIZE_T n,LPTHREAD_START_ROUTINE p,LPVOID a,DWORD flags,LPDWORD id){
  if(GetCurrentThreadId()==ownerThread && failNextWorker){failNextWorker=false;SetLastError(ERROR_NOT_ENOUGH_MEMORY);return nullptr;}
  return originalCreateWorker(s,n,p,a,flags,id);
}
static HANDLE WINAPI CreateMappingHook(HANDLE f,LPSECURITY_ATTRIBUTES s,DWORD p,DWORD h,DWORD l,LPCSTR n) {
  if(GetCurrentThreadId()==ownerThread && failNextCreateMapping){failNextCreateMapping=false;SetLastError(ERROR_NOT_ENOUGH_MEMORY);return nullptr;}
  return originalCreateMapping(f,s,p,h,l,n);
}
static LPVOID WINAPI MapHook(HANDLE f,DWORD a,DWORD h,DWORD l,SIZE_T n) {
  if(GetCurrentThreadId()==ownerThread && failNextMap){failNextMap=false;SetLastError(ERROR_NOT_ENOUGH_MEMORY);return nullptr;}
  const auto begin=Now(); auto p=originalMap(f,a,h,l,n); const auto end=Now();
  if(p && GetCurrentThreadId()==ownerThread && findSentinelFile)sentinelFile=f;
  if(GetCurrentThreadId()==ownerThread && counting && p) { ++os.maps; os.mapTicks+=end-begin; }
  return p;
}
static BOOL WINAPI UnmapHook(LPCVOID p) {
  bool held=false;
  if(watchRetirement.load() && GetCurrentThreadId()!=ownerThread){++retiredOffThread;bool expected=false;if(heldRetirement.compare_exchange_strong(expected,true)){held=true;SetEvent(retirementReached);WaitForSingleObject(retirementGate,5000);}}
  const bool inspect=inspectRelease.load();
  ReleaseView view;if(inspect)view=InspectView(p);
  const auto begin=Now(); auto r=originalUnmap(p); const auto end=Now();
  if(inspect){view.unmapNs=Ns(end-begin);std::lock_guard<std::mutex> lock(observationMutex);releaseViews.push_back(view);}
  if(GetCurrentThreadId()==ownerThread && counting && r) {
    ++os.unmaps; os.unmapTicks+=end-begin; ++os.burst; os.burstTicks+=end-begin;
    os.maxBurst=std::max(os.maxBurst,os.burst); os.maxBurstTicks=std::max(os.maxBurstTicks,os.burstTicks);
  }
  if(r && GetCurrentThreadId()!=ownerThread && countWorker.load()){++workerCalls;workerTicks+=end-begin;}
  if(held)SetEvent(retirementFinished);
  return r;
}
static void Operation() { os.burst=os.burstTicks=0; }
// The observation pass consumes completed outer allocator spans. OS events are
// nested and must not contribute a second copy of logical mapped bytes.
struct Residency {
  ID3D9BridgeTelemetry* t=nullptr; U64 capture=0, mapped=0, peak=0; bool valid=true;
  bool requireSyncNesting=false;unsigned deferredLogicalUnmaps=0;
  struct Entry { U64 bytes; unsigned refs; };
  std::unordered_map<U64,Entry> live;
  std::vector<D3D9BridgeTelemetryEvent> events;
  explicit Residency(IDirect3DDevice9* d):events(65536) { CHECK(d->QueryInterface(IID_D3D9BridgeTelemetry,reinterpret_cast<void**>(&t))==S_OK); }
  void Start() {
    capture=++nextCapture; mapped=peak=0; valid=true; live.clear();
    D3D9BridgeTelemetryConfig c{}; c.StructSize=sizeof(c); c.Version=1; c.CaptureId=capture; c.MaxEvents=65536;
    CHECK(t && t->StartCapture(&c)==S_OK);
  }
  void Poll() {
    if(!t)return;
    D3D9BridgeTelemetryRead r{};r.StructSize=sizeof(r);r.Version=1;
    auto hr=t->ReadEvents(capture,&r,events.data(),unsigned(events.size()));
    if(hr!=S_OK || r.Dropped || r.Filtered || r.LateDropped || r.Remaining)valid=false;
    std::sort(events.begin(),events.begin()+r.Written,[](const auto& a,const auto& b){return a.EndNs<b.EndNs;});
    for(unsigned i=0;i<r.Written;++i) {
      const auto& e=events[i];
      if(e.Kind==D3D9_BRIDGE_TELEMETRY_UNMAP && (e.Flags&D3D9_BRIDGE_TELEMETRY_FILE_BACKED)) {
        if(e.Flags&D3D9_BRIDGE_TELEMETRY_OS_MAPPING){
          if(requireSyncNesting){bool contained=false;for(unsigned j=0;j<r.Written;++j){const auto& outer=events[j];contained|=outer.Kind==e.Kind && (outer.Flags&D3D9_BRIDGE_TELEMETRY_FILE_BACKED) && !(outer.Flags&D3D9_BRIDGE_TELEMETRY_OS_MAPPING) && outer.ThreadId==e.ThreadId && outer.BeginNs<=e.BeginNs && outer.EndNs>=e.EndNs;}CHECK(contained);}
        }else deferredLogicalUnmaps+=bool(e.Flags&D3D9_BRIDGE_TELEMETRY_DEFERRED);
      }
      if(!(e.Flags&D3D9_BRIDGE_TELEMETRY_FILE_BACKED) || (e.Flags&D3D9_BRIDGE_TELEMETRY_OS_MAPPING))continue;
      if(e.Flags&D3D9_BRIDGE_TELEMETRY_FAILED){valid=false;continue;}
      if(e.Kind==D3D9_BRIDGE_TELEMETRY_MAP) {
        auto& a=live[e.Identity];a.bytes=e.Bytes;if(!a.refs++)mapped+=e.Bytes;peak=std::max(peak,mapped);
      } else if(e.Kind==D3D9_BRIDGE_TELEMETRY_UNMAP) {
        auto p=live.find(e.Identity);if(p==live.end() || !p->second.refs){valid=false;continue;}
        if(!--p->second.refs){mapped-=p->second.bytes;live.erase(p);}
      }
    }
  }
  void Stop() { Poll();
    const auto deadline=GetTickCount64()+10000;
    for(;;){D3D9BridgeTelemetryRead pending{};pending.StructSize=sizeof(pending);pending.Version=1;
      CHECK(t->ReadEvents(capture,&pending,nullptr,0)==S_OK);if(!pending.InFlight)break;
      if(GetTickCount64()>=deadline){CHECK(false);break;}SwitchToThread();Poll();}
    CHECK(t->StopCapture(capture)==S_OK);Poll();
    D3D9BridgeTelemetryRead r{};r.StructSize=sizeof(r);r.Version=1;
    if(t->ReadEvents(capture,&r,nullptr,0)!=S_OK || r.InFlight || r.InFlightAtStop || r.LateDropped || r.Dropped)valid=false;
    CHECK(valid); }
  ~Residency(){if(t)t->Release();}
};
static IDirect3DTexture9* Create(IDirect3DDevice9* d,unsigned w=512,unsigned h=512,unsigned levels=1,D3DPOOL pool=D3DPOOL_SYSTEMMEM,void* data=nullptr) {
  IDirect3DTexture9* p=nullptr;HANDLE initial=data;
  CHECK(d->CreateTexture(w,h,levels,0,D3DFMT_A8R8G8B8,pool,&p,data?&initial:nullptr)==S_OK);return p;
}
static void Access(IDirect3DTexture9* p,DWORD value,bool write=true,unsigned level=0) {
  D3DLOCKED_RECT r{};CHECK(p && p->LockRect(level,&r,nullptr,write?0:D3DLOCK_READONLY)==S_OK);
  if(r.pBits){if(write)*static_cast<DWORD*>(r.pBits)=value;else CHECK(*static_cast<DWORD*>(r.pBits)==value);CHECK(p->UnlockRect(level)==S_OK);}
}
static void Release(std::vector<IDirect3DTexture9*>& list) { for(auto p:list)if(p)p->Release();list.clear(); }
static void Retirement(IDirect3DDevice9* d,unsigned budget) {
  if(!budget)return;
  retirementReached=CreateEventA(nullptr,TRUE,FALSE,nullptr);retirementGate=CreateEventA(nullptr,TRUE,FALSE,nullptr);
  retirementFinished=CreateEventA(nullptr,TRUE,FALSE,nullptr);
  auto keeper=Create(d,8,8);Access(keeper,0xff998877);
  std::vector<IDirect3DTexture9*> workingSet;for(unsigned i=0;i<7;++i)workingSet.push_back(Create(d));
  auto p=Create(d,512,512);Fill(p,2);D3DLOCKED_RECT a{};CHECK(p->LockRect(0,&a,nullptr,D3DLOCK_READONLY)==S_OK);auto old=a.pBits;CHECK(p->UnlockRect(0)==S_OK);
  heldRetirement=false;retiredOffThread=0;watchRetirement=true;p->Release();
  const auto reached=WaitForSingleObject(retirementReached,500);CHECK(reached==WAIT_OBJECT_0);
  if(reached==WAIT_OBJECT_0){MEMORY_BASIC_INFORMATION info{};CHECK(VirtualQuery(old,&info,sizeof(info))==sizeof(info) && info.State==MEM_COMMIT);Access(keeper,0xff998877,false);
    // The retired view may alias a recycled file range. It is unreachable by
    // the application; unmapping it must not invalidate the new mapping.
    auto replacement=Create(d,512,512);Fill(replacement,3);SetEvent(retirementGate);
    auto gpu=Create(d,512,512,1,D3DPOOL_DEFAULT);CHECK(d->UpdateTexture(replacement,gpu)==S_OK);Pixels(d,gpu,3);gpu->Release();replacement->Release();
  } else SetEvent(retirementGate);
  if(reached==WAIT_OBJECT_0)CHECK(WaitForSingleObject(retirementFinished,5000)==WAIT_OBJECT_0);
  watchRetirement=false;Release(workingSet);keeper->Release();Flush(d);
  CloseHandle(retirementGate);CloseHandle(retirementReached);CloseHandle(retirementFinished);
  std::printf("retirement offThread=%u held=%u\n",retiredOffThread.load(),unsigned(heldRetirement.load()));
}
static void RetirementFailure(IDirect3DDevice9* d) {
  Residency r(d);r.Start();r.requireSyncNesting=true;
  std::vector<IDirect3DTexture9*> workingSet;for(unsigned i=0;i<7;++i)workingSet.push_back(Create(d));
  auto p=Create(d);Fill(p,3);failNextWorker=true;os={};counting=true;p->Release();counting=false;
  CHECK(!failNextWorker);CHECK(os.unmaps==1);failNextWorker=false;
  Release(workingSet);
  p=Create(d);Fill(p,4);auto gpu=Create(d,512,512,1,D3DPOOL_DEFAULT);CHECK(d->UpdateTexture(p,gpu)==S_OK);Pixels(d,gpu,4);gpu->Release();p->Release();Flush(d);r.Stop();CHECK(r.deferredLogicalUnmaps==0);
}
static void RetirementPressure(IDirect3DDevice9* d,bool retry) {
  Residency r(d);r.Start();
  retirementReached=CreateEventA(nullptr,TRUE,FALSE,nullptr);retirementGate=CreateEventA(nullptr,TRUE,FALSE,nullptr);retirementFinished=CreateEventA(nullptr,TRUE,FALSE,nullptr);
  std::vector<IDirect3DTexture9*> hot;for(unsigned i=0;i<7;++i){auto p=Create(d);Access(p,0xff000000|i);hot.push_back(p);}
  auto victim=Create(d);Fill(victim,2);heldRetirement=false;watchRetirement=true;victim->Release();
  const auto reached=WaitForSingleObject(retirementReached,500);CHECK(reached==WAIT_OBJECT_0);
  if(reached==WAIT_OBJECT_0){
    D3D9BridgeTelemetryRead pending{};pending.StructSize=sizeof(pending);pending.Version=1;CHECK(r.t->ReadEvents(r.capture,&pending,nullptr,0)==S_OK);CHECK(pending.InFlight>0);
    // Pending view + new two-MiB view crosses the ten-MiB budget. The pending
    // view must be drained before any still useful hot mapping is evicted.
    failNextMap=retry;
    std::thread unblock([]{Sleep(60);SetEvent(retirementGate);});auto begin=Now();auto incoming=Create(d,1024,512);auto elapsed=Ns(Now()-begin);unblock.join();CHECK(!failNextMap);failNextMap=false;
    CHECK(elapsed>=30000000);CHECK(WaitForSingleObject(retirementFinished,5000)==WAIT_OBJECT_0);
    os={};counting=true;for(unsigned i=0;i<hot.size();++i)Access(hot[i],0xff000000|i,false);counting=false;CHECK(os.maps==0);
    if(incoming){Fill(incoming,3);auto gpu=Create(d,1024,512,1,D3DPOOL_DEFAULT);CHECK(d->UpdateTexture(incoming,gpu)==S_OK);Pixels(d,gpu,3);gpu->Release();incoming->Release();}
  }else SetEvent(retirementGate);
  watchRetirement=false;Release(hot);r.Stop();CHECK(r.mapped==0);Flush(d);
  CloseHandle(retirementGate);CloseHandle(retirementReached);CloseHandle(retirementFinished);
}
static void RetirementTeardown(IDirect3DDevice9*& d,bool capacity) {
  retirementReached=CreateEventA(nullptr,TRUE,FALSE,nullptr);retirementGate=CreateEventA(nullptr,TRUE,FALSE,nullptr);retirementFinished=CreateEventA(nullptr,TRUE,FALSE,nullptr);
  std::vector<IDirect3DTexture9*> textures;const unsigned count=capacity?128:8;
  for(unsigned i=0;i<count;++i){auto p=Create(d);Access(p,0xff000000|i);textures.push_back(p);}
  heldRetirement=false;watchRetirement=true;textures.front()->Release();textures[0]=nullptr;
  CHECK(WaitForSingleObject(retirementReached,500)==WAIT_OBJECT_0);
  std::atomic<unsigned> released{1},beforeSignal{0};
  std::thread unblock([&]{Sleep(60);beforeSignal=released.load();SetEvent(retirementGate);});
  auto begin=Now();for(unsigned i=1;i<count;++i){textures[i]->Release();++released;}auto releaseEnd=Now();
  CHECK(d->Release()==0);auto destroyed=Now();d=nullptr;unblock.join();
  CHECK(WaitForSingleObject(retirementFinished,5000)==WAIT_OBJECT_0);watchRetirement=false;
  if(capacity){CHECK(beforeSignal>1 && beforeSignal<count);CHECK(Ns(releaseEnd-begin)>=30000000);}
  else {CHECK(beforeSignal==count);CHECK(Ns(destroyed-begin)>=30000000);}
  std::printf("retirement-teardown capacity=%u releasedBeforeSignal=%u total=%u\n",unsigned(capacity),beforeSignal.load(),count);
  CloseHandle(retirementGate);CloseHandle(retirementReached);CloseHandle(retirementFinished);
}
static void BudgetCheck(Residency& r,unsigned budget) {r.Poll();if(budget)CHECK(r.mapped<=U64(budget)*1048576);}

static void Edge(IDirect3DDevice9* d,unsigned budget) {
  Residency r(d);r.Start();std::vector<IDirect3DTexture9*> sources;
  // Every 512-square allocation is 1 MiB + 64 bytes. A final skinny
  // texture fills the exact logical remainder, including padding.
  if(!budget || budget>200){auto p=Create(d);Access(p,0xff123456);p->Release();r.Stop();CHECK(r.mapped==0);return;}
  for(unsigned i=0;i<budget-1;++i){auto p=Create(d);sources.push_back(p);Access(p,0xff000000|i);BudgetCheck(r,budget);}
  auto exact=Create(d,16,16384-budget);sources.push_back(exact);Access(exact,0xff123456);r.Poll();
  std::printf("edge stage=exact mapped=%llu budget=%llu\n",r.mapped,U64(budget)*1048576);
  CHECK(r.mapped==U64(budget)*1048576);
  os={};counting=true;Access(exact,0xff123456,false);counting=false;CHECK(os.maps==0);
  if(budget>1) {
    auto incoming=Create(d);sources.push_back(incoming);Access(incoming,0xffabcdef);r.Poll();
    CHECK(r.mapped<=U64(budget)*1048576);CHECK(r.mapped>U64(budget)*1048576*3/4);
    os={};counting=true;Access(sources[1],0xff000001,false);counting=false;
    std::printf("edge stage=survivor maps=%llu mapped=%llu\n",os.maps,r.mapped);CHECK(os.maps==0);
    Fill(exact,3);auto gpu=Create(d,16,16384-budget,1,D3DPOOL_DEFAULT);CHECK(d->UpdateTexture(exact,gpu)==S_OK);Pixels(d,gpu,3);gpu->Release();
  }
  Release(sources);r.Stop();CHECK(r.mapped==0);Flush(d);
}
static void LockedAndPages(IDirect3DDevice9* d,unsigned budget) {
  Residency r(d);r.Start();
  const unsigned side=budget==1?1024:512;
  auto old=Create(d,side,side,0);Fill(old,1);
  D3DLOCKED_RECT a{},b{};CHECK(old->LockRect(0,&a,nullptr,0)==S_OK);CHECK(old->LockRect(1,&b,nullptr,0)==S_OK);
  r.Poll();const auto pinned=r.mapped;
  std::vector<IDirect3DTexture9*> pressure;
  for(unsigned i=0;i<(budget?budget+3:3);++i){auto p=Create(d);pressure.push_back(p);Access(p,i);*static_cast<DWORD*>(a.pBits)=Color(0,1);*static_cast<DWORD*>(b.pBits)=Color(1,1);r.Poll();CHECK(r.mapped>=pinned);}
  CHECK(old->UnlockRect(0)==S_OK);r.Poll();CHECK(r.mapped>=pinned);*static_cast<DWORD*>(b.pBits)=Color(1,1);CHECK(old->UnlockRect(1)==S_OK);BudgetCheck(r,budget);
  auto gpu=Create(d,side,side,0,D3DPOOL_DEFAULT);CHECK(d->UpdateTexture(old,gpu)==S_OK);Pixels(d,gpu,1);gpu->Release();old->Release();Release(pressure);r.Poll();CHECK(r.mapped==0);
  auto x=Create(d,8,8), y=Create(d,8,8);D3DLOCKED_RECT lx{},ly{};
  CHECK(x->LockRect(0,&lx,nullptr,0)==S_OK);CHECK(y->LockRect(0,&ly,nullptr,0)==S_OK);
  MEMORY_BASIC_INFORMATION mx{},my{};CHECK(VirtualQuery(lx.pBits,&mx,sizeof(mx))==sizeof(mx));CHECK(VirtualQuery(ly.pBits,&my,sizeof(my))==sizeof(my));
  if(budget)CHECK(mx.AllocationBase==my.AllocationBase);
  *static_cast<DWORD*>(lx.pBits)=0xff112233;*static_cast<DWORD*>(ly.pBits)=0xff223344;
  CHECK(x->UnlockRect(0)==S_OK);CHECK(y->UnlockRect(0)==S_OK);
  os={};counting=true;x->Release();counting=false;if(budget)CHECK(os.unmaps==0);Access(y,0xff223344,false);y->Release();
  auto cross=Create(d,257,128);Fill(cross,2);gpu=Create(d,257,128,1,D3DPOOL_DEFAULT);CHECK(d->UpdateTexture(cross,gpu)==S_OK);Pixels(d,gpu,2);gpu->Release();cross->Release();
  if(budget) {
    std::vector<IDirect3DTexture9*> allPinned;std::vector<D3DLOCKED_RECT> locks;
    for(unsigned i=0;i<budget+2;++i){auto p=Create(d);Access(p,i);D3DLOCKED_RECT a{};CHECK(p->LockRect(0,&a,nullptr,0)==S_OK);allPinned.push_back(p);locks.push_back(a);}
    r.Poll();CHECK(r.mapped>U64(budget)*1048576);
    // Every mapped allocation is pinned; a normal reclamation entry must
    // return with these pointers intact, even though the budget is exceeded.
    for(unsigned i=0;i<allPinned.size();++i)*static_cast<DWORD*>(locks[i].pBits)=0xffa00000|i;
    for(unsigned i=0;i<allPinned.size();++i){CHECK(*static_cast<DWORD*>(locks[i].pBits)==(0xffa00000|i));CHECK(allPinned[i]->UnlockRect(0)==S_OK);r.Poll();}
    BudgetCheck(r,budget);Release(allPinned);
  }
  r.Stop();CHECK(r.mapped==0);Flush(d);
}
static void Draw(IDirect3DDevice9* d,IDirect3DTexture9* texture);
static void SolidPixels(IDirect3DDevice9* d,IDirect3DTexture9* texture,DWORD expected) {
  Draw(d,texture);IDirect3DSurface9 *rt=nullptr,*copy=nullptr;
  CHECK(d->GetRenderTarget(0,&rt)==S_OK);CHECK(d->CreateOffscreenPlainSurface(32,32,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&copy,nullptr)==S_OK);CHECK(d->GetRenderTargetData(rt,copy)==S_OK);
  D3DLOCKED_RECT a{};CHECK(copy->LockRect(&a,nullptr,D3DLOCK_READONLY)==S_OK);if(a.pBits){CHECK(*reinterpret_cast<DWORD*>(static_cast<char*>(a.pBits)+8*a.Pitch+32)==expected);CHECK(copy->UnlockRect()==S_OK);}copy->Release();rt->Release();
}
static void RowStride(IDirect3DDevice9* d) {
  IDirect3DTexture9 *p=nullptr,*gpu=nullptr;
  CHECK(d->CreateTexture(3,7,1,0,D3DFMT_R5G6B5,D3DPOOL_SYSTEMMEM,&p,nullptr)==S_OK);
  if(!p)return;
  D3DLOCKED_RECT a{};CHECK(p->LockRect(0,&a,nullptr,0)==S_OK);
  if(a.pBits){CHECK(a.Pitch>=6);for(unsigned y=0;y<7;++y){auto row=reinterpret_cast<unsigned short*>(static_cast<char*>(a.pBits)+y*a.Pitch);std::fill_n(row,3,0xf81f);if(a.Pitch>=8)row[3]=0x07e0;}CHECK(p->UnlockRect(0)==S_OK);}
  CHECK(p->LockRect(0,&a,nullptr,D3DLOCK_READONLY)==S_OK);
  if(a.pBits){for(unsigned y=0;y<7;++y)for(unsigned x=0;x<3;++x)CHECK(reinterpret_cast<const unsigned short*>(static_cast<const char*>(a.pBits)+y*a.Pitch)[x]==0xf81f);CHECK(p->UnlockRect(0)==S_OK);}
  CHECK(d->CreateTexture(3,7,1,0,D3DFMT_R5G6B5,D3DPOOL_DEFAULT,&gpu,nullptr)==S_OK);CHECK(d->UpdateTexture(p,gpu)==S_OK);Draw(d,gpu);
  IDirect3DSurface9 *rt=nullptr,*copy=nullptr;CHECK(d->GetRenderTarget(0,&rt)==S_OK);CHECK(d->CreateOffscreenPlainSurface(32,32,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&copy,nullptr)==S_OK);CHECK(d->GetRenderTargetData(rt,copy)==S_OK);a={};CHECK(copy->LockRect(&a,nullptr,D3DLOCK_READONLY)==S_OK);if(a.pBits){CHECK(*reinterpret_cast<DWORD*>(static_cast<char*>(a.pBits)+8*a.Pitch+32)==0xffff00ff);CHECK(copy->UnlockRect()==S_OK);}copy->Release();rt->Release();gpu->Release();p->Release();
}
static void Initializer(IDirect3DDevice9* d,unsigned budget) {
  RowStride(d);
  Residency r(d);r.Start();
  for(auto pool:{D3DPOOL_SYSTEMMEM,D3DPOOL_MANAGED,D3DPOOL_SCRATCH}) {
    os={};counting=true;auto p=Create(d,256,256,0,pool);counting=false;
    std::printf("initializer pool=%u createMaps=%llu createUnmaps=%llu\n",unsigned(pool),os.maps,os.unmaps);
    if(budget){CHECK(os.maps>=1);CHECK(os.unmaps==0);}
    os={};counting=true;
    for(unsigned level=0;level<p->GetLevelCount();++level){D3DLOCKED_RECT a{};CHECK(p->LockRect(level,&a,nullptr,D3DLOCK_READONLY)==S_OK);D3DSURFACE_DESC desc{};p->GetLevelDesc(level,&desc);if(a.pBits){for(unsigned y=0;y<desc.Height;++y)for(unsigned x=0;x<desc.Width;++x)CHECK(reinterpret_cast<const DWORD*>(static_cast<const char*>(a.pBits)+a.Pitch*y)[x]==0);CHECK(p->UnlockRect(level)==S_OK);}}
    counting=false;if(budget)CHECK(os.maps==0);
    Fill(p,2);if(pool!=D3DPOOL_SCRATCH){auto gpu=Create(d,256,256,0,D3DPOOL_DEFAULT);if(pool==D3DPOOL_MANAGED)Pixels(d,p,2);else{CHECK(d->UpdateTexture(p,gpu)==S_OK);Pixels(d,gpu,2);}gpu->Release();}p->Release();r.Poll();CHECK(r.mapped==0);
  }
  IDirect3DCubeTexture9* cube=nullptr;os={};counting=true;
  CHECK(d->CreateCubeTexture(64,0,0,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&cube,nullptr)==S_OK);counting=false;
  if(budget)CHECK(os.maps>=1 && !os.unmaps);os={};counting=true;
  if(cube){for(unsigned face=0;face<6;++face)for(unsigned mip=0;mip<cube->GetLevelCount();++mip){D3DSURFACE_DESC desc{};CHECK(cube->GetLevelDesc(mip,&desc)==S_OK);D3DLOCKED_RECT a{};CHECK(cube->LockRect(D3DCUBEMAP_FACES(face),mip,&a,nullptr,D3DLOCK_READONLY)==S_OK);if(a.pBits){for(unsigned y=0;y<desc.Height;++y)for(unsigned x=0;x<desc.Width;++x)CHECK(reinterpret_cast<const DWORD*>(static_cast<const char*>(a.pBits)+y*a.Pitch)[x]==0);CHECK(cube->UnlockRect(D3DCUBEMAP_FACES(face),mip)==S_OK);}}counting=false;if(budget)CHECK(!os.maps);cube->Release();}counting=false;r.Poll();CHECK(r.mapped==0);
  IDirect3DVolumeTexture9* volume=nullptr;
  CHECK(d->CreateVolumeTexture(64,33,7,0,0,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&volume,nullptr)==S_OK);
  if(volume){for(unsigned mip=0;mip<volume->GetLevelCount();++mip){D3DVOLUME_DESC desc{};CHECK(volume->GetLevelDesc(mip,&desc)==S_OK);D3DLOCKED_BOX a{};CHECK(volume->LockBox(mip,&a,nullptr,D3DLOCK_READONLY)==S_OK);bool zero=a.pBits!=nullptr;if(a.pBits){for(unsigned z=0;z<desc.Depth;++z)for(unsigned y=0;y<desc.Height;++y)for(unsigned x=0;x<desc.Width;++x)zero &= reinterpret_cast<const DWORD*>(static_cast<const char*>(a.pBits)+z*a.SlicePitch+y*a.RowPitch)[x]==0;CHECK(volume->UnlockBox(mip)==S_OK);}CHECK(zero);}volume->Release();}r.Poll();CHECK(r.mapped==0);
  std::vector<DWORD> data(17*19,Color(0,4));auto p=Create(d,17,19,1,D3DPOOL_SYSTEMMEM,data.data());Access(p,Color(0,4),false);auto gpu=Create(d,17,19,1,D3DPOOL_DEFAULT);CHECK(d->UpdateTexture(p,gpu)==S_OK);Pixels(d,gpu,4);gpu->Release();p->Release();
  HANDLE initial=nullptr;
  IDirect3DSurface9 *surface=nullptr,*gpuSurface=nullptr;initial=data.data();
  CHECK(d->CreateOffscreenPlainSurface(17,19,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&surface,&initial)==S_OK);
  if(surface){D3DLOCKED_RECT a{};CHECK(surface->LockRect(&a,nullptr,D3DLOCK_READONLY)==S_OK);if(a.pBits){for(unsigned y=0;y<19;++y)for(unsigned x=0;x<17;++x)CHECK(reinterpret_cast<DWORD*>(static_cast<char*>(a.pBits)+y*a.Pitch)[x]==Color(0,4));CHECK(surface->UnlockRect()==S_OK);}
    gpu=Create(d,17,19,1,D3DPOOL_DEFAULT);CHECK(gpu->GetSurfaceLevel(0,&gpuSurface)==S_OK);CHECK(d->UpdateSurface(surface,nullptr,gpuSurface,nullptr)==S_OK);Pixels(d,gpu,4);gpuSurface->Release();gpu->Release();surface->Release();}
  // Create-only and old locked source: no untracked initialized mappings.
  auto old=Create(d,256,256);D3DLOCKED_RECT locked{};CHECK(old->LockRect(0,&locked,nullptr,0)==S_OK);
  std::vector<IDirect3DTexture9*> unused;
  for(unsigned i=0;i<(budget?budget+8:8);++i){unused.push_back(Create(d));*static_cast<DWORD*>(locked.pBits)=0xff987654;BudgetCheck(r,budget);}
  CHECK(old->UnlockRect(0)==S_OK);Access(old,0xff987654,false);Release(unused);old->Release();r.Poll();
  if(budget>=4){std::vector<IDirect3DTexture9*> hot,cold;
    for(unsigned i=0;i<budget*88/100;++i){auto q=Create(d);Access(q,0xff555555);hot.push_back(q);r.Poll();}
    for(unsigned i=0;i<budget/4+4;++i){cold.push_back(Create(d));BudgetCheck(r,budget);}
    os={};counting=true;Access(hot.front(),0xff555555,false);counting=false;
    std::printf("initializer unused-admission survivorMaps=%llu\n",os.maps);CHECK(os.maps==0);
    Fill(hot.front(),5);gpu=Create(d,512,512,1,D3DPOOL_DEFAULT);CHECK(d->UpdateTexture(hot.front(),gpu)==S_OK);Pixels(d,gpu,5);gpu->Release();BudgetCheck(r,budget);Release(cold);Release(hot);}
  r.Stop();CHECK(r.mapped==0);Flush(d);
}
static void Concurrent(IDirect3DDevice9* d,unsigned budget) {
  std::atomic<unsigned> errors{0};std::atomic<bool> go{false};
  IDirect3DTexture9* last[2]{};
  auto worker=[&](unsigned slot){const unsigned tag=slot?0x2200:0x1100;while(!go.load())std::this_thread::yield();for(unsigned i=0;i<48;++i){IDirect3DTexture9 *s=nullptr,*g=nullptr;unsigned side=budget==1?768:512;
    if(d->CreateTexture(side,side,1,0,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&s,nullptr)!=S_OK){++errors;continue;}
    D3DLOCKED_RECT a{};if(s->LockRect(0,&a,nullptr,0)==S_OK){for(unsigned y=0;y<side;++y)std::fill_n(reinterpret_cast<DWORD*>(static_cast<char*>(a.pBits)+y*a.Pitch),side,0xff000000|tag|i);s->UnlockRect(0);}else ++errors;
    if(s->LockRect(0,&a,nullptr,D3DLOCK_READONLY)==S_OK){if(*static_cast<DWORD*>(a.pBits)!=(0xff000000|tag|i))++errors;s->UnlockRect(0);}else ++errors;
    if(d->CreateTexture(side,side,1,0,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&g,nullptr)==S_OK){if(d->UpdateTexture(s,g)!=S_OK)++errors;if(i==47)last[slot]=g;else g->Release();}else ++errors;s->Release();}};
  std::thread a(worker,0),b(worker,1);go=true;a.join();b.join();CHECK(errors==0);Flush(d);
  for(unsigned slot=0;slot<2;++slot){CHECK(last[slot]);if(last[slot]){SolidPixels(d,last[slot],0xff000000|(slot?0x2200:0x1100)|47);last[slot]->Release();}}Flush(d);
}
static void AllocationFailure(IDirect3DDevice9* d,unsigned budget) {
  if(!budget)return;
  // All earlier file allocations have been released, so the next source must
  // acquire a fresh chunk at the existing MemoryFile creation boundary.
  IDirect3DTexture9* p=nullptr;failNextCreateMapping=true;
  auto hr=d->CreateTexture(512,512,1,0,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&p,nullptr);
  std::printf("allocation-failure hr=0x%08lx injected=%u null=%u\n",static_cast<unsigned long>(hr),unsigned(!failNextCreateMapping),unsigned(p==nullptr));
  CHECK(!failNextCreateMapping);CHECK(hr==D3DERR_OUTOFVIDEOMEMORY && !p);failNextCreateMapping=false;if(p)p->Release();
  p=Create(d);Access(p,0xff123456);Access(p,0xff123456,false);if(p)p->Release();Flush(d);
}
static void FreshZero(IDirect3DDevice9* d,unsigned budget) {
  if (!budget) return;
  // Keep the backing chunk alive, then force reuse of a previously written
  // range. Fresh storage and recycled storage must have identical visible bytes.
  auto keeper=Create(d,8,8);Access(keeper,0xff123456);
  zeroBytes=0;counting=true;auto p=Create(d,256,256);counting=false;
  const auto freshBytes=zeroBytes;
  Access(p,0,false);
  D3DLOCKED_RECT a{};CHECK(p->LockRect(0,&a,nullptr,0)==S_OK);
  if(a.pBits){for(unsigned y=0;y<256;++y)std::fill_n(reinterpret_cast<DWORD*>(static_cast<char*>(a.pBits)+y*a.Pitch),256,0xffabcdef);CHECK(p->UnlockRect(0)==S_OK);}
  p->Release();
  zeroBytes=0;counting=true;p=Create(d,256,256);counting=false;
  const auto recycledBytes=zeroBytes;
  CHECK(p->LockRect(0,&a,nullptr,D3DLOCK_READONLY)==S_OK);
  bool zero=true;if(a.pBits){for(unsigned y=0;y<256;++y)for(unsigned x=0;x<256;++x)zero &= reinterpret_cast<DWORD*>(static_cast<char*>(a.pBits)+y*a.Pitch)[x]==0;CHECK(p->UnlockRect(0)==S_OK);}else zero=false;
  CHECK(zero);Access(keeper,0xff123456,false);
  // A partial first write must leave all untouched bytes at zero.
  RECT rect{7,9,13,15};CHECK(p->LockRect(0,&a,&rect,0)==S_OK);
  if(a.pBits){for(unsigned y=0;y<6;++y)std::fill_n(reinterpret_cast<DWORD*>(static_cast<char*>(a.pBits)+y*a.Pitch),6,0xff987654);CHECK(p->UnlockRect(0)==S_OK);}
  CHECK(p->LockRect(0,&a,nullptr,D3DLOCK_READONLY)==S_OK);bool partial=true;
  if(a.pBits){for(unsigned y=0;y<256;++y)for(unsigned x=0;x<256;++x)partial &= reinterpret_cast<DWORD*>(static_cast<char*>(a.pBits)+y*a.Pitch)[x]==(x>=7 && x<13 && y>=9 && y<15?0xff987654u:0u);CHECK(p->UnlockRect(0)==S_OK);}else partial=false;
  CHECK(partial);Fill(p,3);auto gpu=Create(d,256,256,1,D3DPOOL_DEFAULT);CHECK(d->UpdateTexture(p,gpu)==S_OK);Pixels(d,gpu,3);gpu->Release();p->Release();keeper->Release();Flush(d);
  std::printf("fresh-zero freshBytes=%llu recycledBytes=%llu zero=%u partial=%u\n",freshBytes,recycledBytes,unsigned(zero),unsigned(partial));
  CHECK(freshBytes==0);CHECK(recycledBytes>=256*256*4);
}
static void UnusedAdmission(IDirect3DDevice9* d,unsigned budget) {
  if(budget<4)return;
  Residency r(d);r.Start();std::vector<IDirect3DTexture9*> hot,unused;
  for(unsigned i=0;i<budget-3;++i){auto p=Create(d);Access(p,0xff000000|i);hot.push_back(p);r.Poll();}
  // Fill the remaining cache with created-but-never-locked resources. Incoming
  // first-use textures should replace these without evicting the hot sources.
  for(unsigned i=0;i<4;++i){unused.push_back(Create(d));BudgetCheck(r,budget);}
  auto incoming=Create(d);BudgetCheck(r,budget);
  os={};counting=true;Access(incoming,0,false);counting=false;
  const auto firstUseMaps=os.maps;
  os={};counting=true;Access(hot.front(),0xff000000,false);counting=false;
  std::printf("unused-admission firstUseMaps=%llu hotMaps=%llu\n",firstUseMaps,os.maps);
  CHECK(firstUseMaps==0);CHECK(os.maps==0);
  // Reclaiming unused mappings must not change their zero bytes on later use.
  for(auto p:unused)Access(p,0,false);
  Fill(incoming,4);auto gpu=Create(d,512,512,1,D3DPOOL_DEFAULT);CHECK(d->UpdateTexture(incoming,gpu)==S_OK);Pixels(d,gpu,4);gpu->Release();incoming->Release();Release(unused);Release(hot);r.Stop();CHECK(r.mapped==0);Flush(d);
}
static void MappingFailure(IDirect3DDevice9* d,unsigned budget) {
  if(budget<4)return;
  IDirect3DTexture9* p=nullptr;failNextMap=true;
  auto hr=d->CreateTexture(512,512,1,0,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&p,nullptr);
  std::printf("mapping-failure hr=0x%08lx injected=%u null=%u\n",static_cast<unsigned long>(hr),unsigned(!failNextMap),unsigned(p==nullptr));
  CHECK(!failNextMap);CHECK(hr==D3DERR_OUTOFVIDEOMEMORY && !p);failNextMap=false;if(p)p->Release();
  // Failed views must not consume budget after their texture is destroyed.
  Edge(d,budget);
  auto keeper=Create(d,8,8);Access(keeper,0xff123456);
  std::vector<IDirect3DTexture9*> pressure;
  for(unsigned i=0;i<budget+2;++i){auto q=Create(d);Access(q,i);pressure.push_back(q);}
  // The shared page is now unmapped. This allocation starts at a non-zero
  // offset, so a failed base map must not become an apparently valid pointer.
  failNextMap=true;p=nullptr;
  hr=d->CreateTexture(8,8,1,0,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&p,nullptr);
  CHECK(!failNextMap);CHECK(hr==D3DERR_OUTOFVIDEOMEMORY && !p);failNextMap=false;if(p)p->Release();
  Access(keeper,0xff123456,false);keeper->Release();Release(pressure);Edge(d,budget);
}
struct BenchResult {U64 releaseSubmit=0,total=0,cycles=0,present=0,drain=0,release=0,end=0,after=0,peak=0,operationBurst=0,operationBurstTicks=0,cleanupCalls=0,cleanupTicks=0;OsCounters calls;std::vector<U64> samples;unsigned failedPresents=0;bool valid=true,pixels=true,queryValid=true;};
static void Draw(IDirect3DDevice9* d,IDirect3DTexture9* texture) {
  CHECK(d->SetTexture(0,texture)==S_OK);CHECK(d->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1)==S_OK);
  CHECK(d->SetRenderState(D3DRS_ZENABLE,FALSE)==S_OK);CHECK(d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE)==S_OK);
  CHECK(d->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1)==S_OK);CHECK(d->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE)==S_OK);
  const Vertex v[]={{0,0,.5f,1,0,0},{64,0,.5f,1,1,0},{0,64,.5f,1,0,1}};
  CHECK(d->BeginScene()==S_OK);CHECK(d->DrawPrimitiveUP(D3DPT_TRIANGLELIST,1,v,sizeof(Vertex))==S_OK);CHECK(d->EndScene()==S_OK);CHECK(d->SetTexture(0,nullptr)==S_OK);
}
static BenchResult Batch(IDirect3DDevice9* d,unsigned budget,const std::string& workload,unsigned iterations,Residency* observation,bool measured) {
  const unsigned before=failures;BenchResult result; result.samples.reserve(iterations);
  // Validation infrastructure is outside the measured resource lifetime. Draw
  // into an explicit target so Present's discard swap effect cannot destroy the
  // final pixels. The target itself is released inside the lifetime timer.
  IDirect3DSurface9 *back=nullptr,*target=nullptr,*readback=nullptr;
  CHECK(d->GetRenderTarget(0,&back)==S_OK);
  CHECK(d->CreateRenderTarget(32,32,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&target,nullptr)==S_OK);
  CHECK(d->CreateOffscreenPlainSurface(32,32,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&readback,nullptr)==S_OK);
  // The readback resource is mapped before capture and never enters the file
  // allocator after priming GetRenderTargetData's Vulkan readback buffer path.
  CHECK(d->GetRenderTargetData(target,readback)==S_OK);Flush(d);
  auto poll=[&]{if(observation)observation->Poll();};
  if(observation)observation->Start();
  // Shared setup is untimed for reuse workloads, but its complete release is
  // inside the lifetime interval. Create/unused also create incoming inside it.
  const unsigned setSize=workload=="below"?48:workload=="over"?116:workload=="admission"?97:88;
  std::vector<IDirect3DTexture9*> hot,incoming;
  auto gpu=Create(d,512,512,1,D3DPOOL_DEFAULT);
  const bool creating=workload=="create" || workload=="admission";
  if(workload!="create" && workload!="cold")for(unsigned i=0;i<setSize;++i){auto p=Create(d);hot.push_back(p);Fill(p,i%4+1);poll();}
  if(workload=="admission")for(unsigned i=0;i<3;++i){incoming.push_back(Create(d));poll();}
  if(workload=="cold") {Release(hot);poll();for(unsigned i=0;i<iterations;++i){auto p=Create(d);hot.push_back(p);Fill(p,i%4+1);poll();}}
  Flush(d);CHECK(d->SetRenderTarget(0,target)==S_OK);poll();
  ULONG64 cyclesBegin=0,cyclesEnd=0;os={};workerCalls=0;workerTicks=0;counting=measured;countWorker=measured;CHECK(QueryThreadCycleTime(GetCurrentThread(),&cyclesBegin));const auto begin=Now();
  DWORD expectedColor=Color(0,1);
  for(unsigned i=0;i<iterations;++i) {
    const unsigned version=(creating?i:i%hot.size())%4+1;expectedColor=Color(0,version);
    Operation();const auto op=Now();IDirect3DTexture9* p=nullptr;
    if(creating){
      if(workload=="admission") {
        incoming.push_back(Create(d));incoming.front()->Release();incoming.erase(incoming.begin());
        Access(hot[i%hot.size()],Color(0,(i%hot.size())%4+1),false);
      }
      p=Create(d);Fill(p,version);
    }
    else {
      // Hot workload retains at most twelve concurrent cold mappings, then reuses
      // all hot entries in order. Unused deliberately admits never-used input.
      if(workload=="unused" || (workload=="hot" && i%16==0)) {
        auto cold=Create(d);incoming.push_back(cold);if(workload=="hot")Access(cold,0xff334455);
        if(incoming.size()>12){incoming.front()->Release();incoming.erase(incoming.begin());}
      }
      p=hot[i%hot.size()];Access(p,expectedColor);
    }
    CHECK(d->UpdateTexture(p,gpu)==S_OK);Draw(d,gpu);
    const auto presentBegin=Now();auto hr=d->Present(nullptr,nullptr,nullptr,nullptr);result.present+=Now()-presentBegin;if(hr!=S_OK)++result.failedPresents;
    if(creating)p->Release();
    if(workload=="cold"){p->Release();hot[i]=nullptr;}
    result.samples.push_back(Ns(Now()-op));poll();
  }
  result.operationBurst=os.maxBurst;result.operationBurstTicks=os.maxBurstTicks;
  // Establish a final real file-close barrier after the measured operations, so
  // the sentinel cannot change chunk reuse or zero-initialization in the loop.
  findSentinelFile=true;sentinelFile=nullptr;auto sentinel=Create(d,2,2);Access(sentinel,0xff567890);findSentinelFile=false;
  CHECK(sentinelFile);ResetEvent(barrierEvent);barrierFile=sentinelFile;poll();
  // Copy the final actual workload draw before releasing its target. The copy
  // and GPU synchronization remain in totalNs; CPU pixel inspection is outside.
  CHECK(d->GetRenderTargetData(target,readback)==S_OK);
  poll();if(observation)result.end=observation->mapped;
  inspectRelease=observation && std::getenv("RS_RELEASE_RESIDENCY");
  if(inspectRelease){releaseViews.clear();releaseQueryErrors=0;}
  Operation();const auto cleanupBeginCounters=os;const auto cleanupWorkerCalls=workerCalls.load(),cleanupWorkerTicks=workerTicks.load();const auto releaseBegin=Now();Release(incoming);Release(hot);gpu->Release();CHECK(d->SetRenderTarget(0,back)==S_OK);target->Release();
  sentinel->Release();result.releaseSubmit=Ns(Now()-releaseBegin);
  CHECK(WaitForSingleObject(barrierEvent,10000)==WAIT_OBJECT_0);barrierFile=nullptr;
  result.release=Now()-releaseBegin;
  if(inspectRelease){U64 bytes=0,pages=0,resident=0,unmap=0,query=0;for(const auto& v:releaseViews){bytes+=v.bytes;pages+=v.pages;resident+=v.resident;unmap+=v.unmapNs;query+=v.queryNs;}
    std::printf("{\"type\":\"release-residency-observation\",\"timedBenchmark\":false,\"views\":%u,\"bytes\":%llu,\"pages\":%llu,\"residentPages\":%llu,\"unmapNs\":%llu,\"queryNs\":%llu,\"queryErrors\":%u}\n",unsigned(releaseViews.size()),bytes,pages,resident,unmap,query,releaseQueryErrors.load());CHECK(releaseQueryErrors==0);}
  inspectRelease=false;
  const auto drainBegin=Now();const auto drainFailures=failures;Flush(d);result.queryValid=failures==drainFailures;result.drain=Now()-drainBegin;const auto finish=Now();CHECK(QueryThreadCycleTime(GetCurrentThread(),&cyclesEnd));counting=false;countWorker=false;
  os.unmaps+=workerCalls.load();os.unmapTicks+=workerTicks.load();
  result.total=Ns(finish-begin);result.cycles=cyclesEnd-cyclesBegin;result.present=Ns(result.present);result.release=Ns(result.release);result.drain=Ns(result.drain);result.calls=os;result.cleanupCalls=os.unmaps-cleanupBeginCounters.unmaps-cleanupWorkerCalls;result.cleanupTicks=os.unmapTicks-cleanupBeginCounters.unmapTicks-cleanupWorkerTicks;
  if(observation){observation->Stop();result.peak=observation->peak;result.after=observation->mapped;result.valid=observation->valid && !result.after;}
  D3DLOCKED_RECT locked{};CHECK(readback->LockRect(&locked,nullptr,D3DLOCK_READONLY)==S_OK);
  result.pixels=locked.pBits && *reinterpret_cast<const DWORD*>(static_cast<const char*>(locked.pBits)+8*locked.Pitch+8*4)==expectedColor;
  CHECK(result.pixels);if(locked.pBits)CHECK(readback->UnlockRect()==S_OK);readback->Release();back->Release();Flush(d);
  result.valid=result.valid && failures==before && !result.failedPresents;
  return result;
}
static void Bench(IDirect3DDevice9* d,unsigned budget,const std::string& workload,unsigned iterations) {
  Batch(d,budget,workload,16,nullptr,false);Flush(d);
  BenchResult time;
  {BenchmarkThreadAffinity affinity;CHECK(affinity.valid());time=Batch(d,budget,workload,iterations,nullptr,true);}
  Residency observation(d);auto resident=Batch(d,budget,workload,iterations,&observation,false);
  time.pixels=time.pixels && resident.pixels;time.valid=time.valid && resident.valid && time.pixels;
  std::sort(time.samples.begin(),time.samples.end());auto quantile=[&](unsigned n){return time.samples.empty()?0:time.samples[(time.samples.size()-1)*n/100];};
  std::printf("{\"type\":\"mapping-budget-benchmark\",\"affinityScope\":\"main-thread-only-lowest-allowed\",\"residencyPass\":\"separate-untimed\",\"queryValid\":%s,\"workload\":\"%s\",\"iterations\":%u,\"budgetMiB\":%u,\"valid\":%s,\"pixelsValid\":%s,\"failedPresents\":%u,\"totalNs\":%llu,\"mainThreadCycles\":%llu,\"p95Ns\":%llu,\"p99Ns\":%llu,\"maxNs\":%llu,\"presentNs\":%llu,\"drainNs\":%llu,\"releaseNs\":%llu,\"releaseSubmitNs\":%llu,\"mapCalls\":%llu,\"unmapCalls\":%llu,\"mapNs\":%llu,\"unmapNs\":%llu,\"maxUnmapBurst\":%llu,\"maxUnmapBurstNs\":%llu,\"operationMaxUnmapBurst\":%llu,\"operationMaxUnmapBurstNs\":%llu,\"cleanupUnmapCalls\":%llu,\"cleanupUnmapNs\":%llu,\"mappedPeakBytes\":%llu,\"mappedEndBytes\":%llu,\"mappedAfterReleaseBytes\":%llu}\n",
    time.queryValid && resident.queryValid?"true":"false",workload.c_str(),iterations,budget,time.valid?"true":"false",time.pixels?"true":"false",time.failedPresents,time.total,time.cycles,quantile(95),quantile(99),quantile(100),time.present,time.drain,time.release,time.releaseSubmit,time.calls.maps,time.calls.unmaps,Ns(time.calls.mapTicks),Ns(time.calls.unmapTicks),time.calls.maxBurst,Ns(time.calls.maxBurstTicks),time.operationBurst,Ns(time.operationBurstTicks),time.cleanupCalls,Ns(time.cleanupTicks),resident.peak,resident.end,resident.after);
  CHECK(time.valid);
}
}
int main(int argc,char** argv) {
  using namespace budget_probe;
  std::setvbuf(stdout,nullptr,_IONBF,0);
  if(argc<4)return 2;const std::string mode=argv[2];const unsigned budget=std::strtoul(argv[3],nullptr,10);
  if(mode=="bench" && argc!=6)return 2;
  if(mode!="bench" && mode!="retirement" && mode!="retirement-failure" && mode!="retirement-pressure" && mode!="retirement-teardown" && mode!="retirement-cap" && mode!="retirement-retry" && mode!="regression" && mode!="initializer" && mode!="allocation-failure" && mode!="row-stride" && mode!="fresh-zero" && mode!="unused-admission" && mode!="mapping-failure")return 2;
  if(mode=="bench") {const std::string w=argv[4];const unsigned n=std::strtoul(argv[5],nullptr,10);if(!n || n>1024 || (w!="below" && w!="hot" && w!="cold" && w!="over" && w!="create" && w!="unused" && w!="admission"))return 2;}
  ownerThread=GetCurrentThreadId();LARGE_INTEGER f;QueryPerformanceFrequency(&f);frequency=f.QuadPart;
  const std::string config="d3d9.textureMemory="+std::to_string(budget)+";d3d9.presentInterval=0;d3d9.gtaSaCompat=False;dxvk.hud=";
  SetEnvironmentVariableA("DXVK_CONFIG",config.c_str());SetEnvironmentVariableA("DXVK_CONFIG_FILE","mapping-budget-no-external.conf");SetEnvironmentVariableA("DXVK_LOG_LEVEL","error");SetEnvironmentVariableA("DXVK_HUD","0");
  barrierEvent=CreateEventA(nullptr,TRUE,FALSE,nullptr);CHECK(barrierEvent);
  HMODULE library=LoadLibraryA(argv[1]);CHECK(library);if(!library)return 1;
  {
    ImportHook mh,uh,ch,sh,clh,th;CHECK(mh.install(library,"MapViewOfFile",reinterpret_cast<void*>(&MapHook)));CHECK(uh.install(library,"UnmapViewOfFile",reinterpret_cast<void*>(&UnmapHook)));originalMap=reinterpret_cast<MapView>(mh.original);originalUnmap=reinterpret_cast<UnmapView>(uh.original);
    if(mode=="retirement-failure"){CHECK(th.install(library,"CreateThread",reinterpret_cast<void*>(&CreateWorkerHook)));originalCreateWorker=reinterpret_cast<CreateWorker>(th.original);}
    CHECK(clh.install(library,"CloseHandle",reinterpret_cast<void*>(&CloseHook)));originalClose=reinterpret_cast<CloseMapping>(clh.original);
    if(mode=="fresh-zero"){CHECK(sh.install(library,"memset",reinterpret_cast<void*>(&SetHook)));originalSet=reinterpret_cast<SetMemory>(sh.original);}
    CHECK(ch.install(library,"CreateFileMappingA",reinterpret_cast<void*>(&CreateMappingHook)));originalCreateMapping=reinterpret_cast<CreateMapping>(ch.original);
    using Factory=IDirect3D9* (WINAPI*)(UINT);auto make=reinterpret_cast<Factory>(GetProcAddress(library,"Direct3DCreate9"));CHECK(make);auto factory=make(D3D_SDK_VERSION);CHECK(factory);
    HWND w=CreateWindowA("STATIC","mapping-budget",WS_POPUP,0,0,64,64,nullptr,nullptr,GetModuleHandleA(nullptr),nullptr);CHECK(w);ShowWindow(w,SW_SHOWNOACTIVATE);UpdateWindow(w);
    D3DPRESENT_PARAMETERS pp{};pp.BackBufferWidth=pp.BackBufferHeight=32;pp.BackBufferFormat=D3DFMT_A8R8G8B8;pp.BackBufferCount=1;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.Windowed=TRUE;pp.hDeviceWindow=w;pp.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* d=nullptr;CHECK(factory->CreateDevice(0,D3DDEVTYPE_HAL,w,D3DCREATE_HARDWARE_VERTEXPROCESSING|D3DCREATE_MULTITHREADED,&pp,&d)==S_OK);
    if(d){if(mode=="bench")Bench(d,budget,argv[4],std::strtoul(argv[5],nullptr,10));else if(mode=="retirement")Retirement(d,budget);else if(mode=="retirement-failure")RetirementFailure(d);else if(mode=="retirement-teardown" || mode=="retirement-cap")RetirementTeardown(d,mode=="retirement-cap");else if(mode=="retirement-pressure" || mode=="retirement-retry")RetirementPressure(d,mode=="retirement-retry");else if(mode=="initializer"){Initializer(d,budget);Concurrent(d,budget);}else if(mode=="allocation-failure")AllocationFailure(d,budget);else if(mode=="mapping-failure")MappingFailure(d,budget);else if(mode=="fresh-zero")FreshZero(d,budget);else if(mode=="unused-admission")UnusedAdmission(d,budget);else if(mode=="row-stride")RowStride(d);else{Edge(d,budget);LockedAndPages(d,budget);
      IDirect3DDevice9* other=nullptr;CHECK(factory->CreateDevice(0,D3DDEVTYPE_HAL,w,D3DCREATE_HARDWARE_VERTEXPROCESSING|D3DCREATE_MULTITHREADED,&pp,&other)==S_OK);if(other){auto x=Create(d),y=Create(other);Access(x,1);Access(y,2);x->Release();Access(y,2,false);y->Release();Flush(other);CHECK(other->Release()==0);}}
      if(d){CHECK(d->Reset(&pp)==S_OK);CHECK(d->Release()==0);}}
    factory->Release();DestroyWindow(w);
  }
  FreeLibrary(library);CloseHandle(barrierEvent);std::printf("checks=%u failures=%u\n",checks,failures);return failures?1:0;
}
