// Combined control: a real first bulk release races the CS readback consumed by
// LockRect. Every OS retirement and the GPU drain remain inside totalNs.
#define main UploadMappingLegacyMain
#include "upload-mapping-probe.cpp"
#undef main
#include <atomic>

static std::atomic<HANDLE> completionFile{nullptr};
static HANDLE completionEvent;
static bool captureFile;
static HANDLE capturedFile;
static decltype(&CloseHandle) realClose;
static LPVOID WINAPI ObserveMap(HANDLE f,DWORD a,DWORD h,DWORD l,SIZE_T size) {
  auto p=originalMap(f,a,h,l,size);
  if(GetCurrentThreadId()==ownerThread && captureFile && p)capturedFile=f;
  return p;
}
static BOOL WINAPI ObserveClose(HANDLE f) {
  auto result=realClose(f);
  if(result && f==completionFile.load())SetEvent(completionEvent);
  return result;
}
static unsigned long long Now() {LARGE_INTEGER q{};QueryPerformanceCounter(&q);return q.QuadPart;}
static unsigned long long frequency;
static unsigned long long Ns(unsigned long long ticks) {return ticks/frequency*1000000000ull+ticks%frequency*1000000000ull/frequency;}

static void Run(IDirect3DDevice9* d) {
  IDirect3DSurface9 *rt=nullptr,*copy=nullptr;IDirect3DTexture9 *marker=nullptr,*gpu=nullptr;
  CHECK(d->GetRenderTarget(0,&rt)==S_OK);
  CHECK(d->CreateOffscreenPlainSurface(32,32,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&copy,nullptr)==S_OK);
  // Prime the readback conversion before installing the final pool-file marker.
  CHECK(d->GetRenderTargetData(rt,copy)==S_OK);Flush(d);
  captureFile=true;capturedFile=nullptr;
  CHECK(d->CreateTexture(2,2,1,0,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&marker,nullptr)==S_OK);
  captureFile=false;CHECK(capturedFile);completionFile=capturedFile;ResetEvent(completionEvent);
  D3DLOCKED_RECT mark{};CHECK(marker->LockRect(0,&mark,nullptr,0)==S_OK);if(mark.pBits)*static_cast<DWORD*>(mark.pBits)=0xff123456;CHECK(marker->UnlockRect(0)==S_OK);
  std::vector<IDirect3DTexture9*> sources;
  BenchmarkThreadAffinity affinity;CHECK(affinity.valid());ULONG64 c0=0,c1=0;CHECK(QueryThreadCycleTime(GetCurrentThread(),&c0));auto start=Now();
  for(unsigned i=0;i<96;++i){IDirect3DTexture9* p=nullptr;CHECK(d->CreateTexture(512,512,1,0,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&p,nullptr)==S_OK);if(p){Fill(p,3);sources.push_back(p);}}
  CHECK(sources.size()==96);CHECK(d->CreateTexture(512,512,1,0,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&gpu,nullptr)==S_OK);
  if(gpu && !sources.empty()){CHECK(d->UpdateTexture(sources.back(),gpu)==S_OK);Pixels(d,gpu,3);}
  CHECK(d->Clear(0,nullptr,D3DCLEAR_TARGET,0xff987654,1,0)==S_OK);
  CHECK(d->GetRenderTargetData(rt,copy)==S_OK);
  CHECK(d->Clear(0,nullptr,D3DCLEAR_TARGET,0xff112233,1,0)==S_OK);
  auto release=Now();for(auto p:sources)p->Release();sources.clear();if(gpu)gpu->Release();auto released=Now();
  D3DLOCKED_RECT locked{};CHECK(copy->LockRect(&locked,nullptr,D3DLOCK_READONLY)==S_OK);auto lockedAt=Now();
  bool pixels=locked.pBits!=nullptr;
  if(locked.pBits){for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x)pixels&=reinterpret_cast<DWORD*>(static_cast<char*>(locked.pBits)+y*locked.Pitch)[x]==0xff987654;CHECK(copy->UnlockRect()==S_OK);}
  CHECK(pixels);copy->Release();rt->Release();marker->Release();
  CHECK(WaitForSingleObject(completionEvent,10000)==WAIT_OBJECT_0);completionFile=nullptr;Flush(d);auto end=Now();CHECK(QueryThreadCycleTime(GetCurrentThread(),&c1));
  std::printf("{\"type\":\"retirement-cs-benchmark\",\"valid\":%s,\"pixelsValid\":%s,\"textures\":96,\"totalNs\":%llu,\"mainThreadCycles\":%llu,\"releaseSubmitNs\":%llu,\"lockNs\":%llu,\"releaseAndLockNs\":%llu,\"completeReleaseAndDrainNs\":%llu,\"completion\":\"actual-last-file-close-and-gpu-query\"}\n",failures?"false":"true",pixels?"true":"false",Ns(end-start),c1-c0,Ns(released-release),Ns(lockedAt-released),Ns(lockedAt-release),Ns(end-release));
}
int main(int argc,char** argv) {
  std::setvbuf(stdout,nullptr,_IONBF,0);if(argc!=2)return 2;ownerThread=GetCurrentThreadId();LARGE_INTEGER f{};QueryPerformanceFrequency(&f);frequency=f.QuadPart;
  SetEnvironmentVariableA("DXVK_CONFIG","d3d9.textureMemory=100;d3d9.presentInterval=0;d3d9.gtaSaCompat=False;dxvk.hud=");SetEnvironmentVariableA("DXVK_CONFIG_FILE","retirement-cs-no-external.conf");SetEnvironmentVariableA("DXVK_HUD","0");SetEnvironmentVariableA("DXVK_LOG_LEVEL","error");
  completionEvent=CreateEventA(nullptr,TRUE,FALSE,nullptr);CHECK(completionEvent);auto lib=LoadLibraryA(argv[1]);CHECK(lib);if(!lib)return 1;
  {
    ImportHook mh,ch;CHECK(mh.install(lib,"MapViewOfFile",reinterpret_cast<void*>(&ObserveMap)));originalMap=reinterpret_cast<MapView>(mh.original);CHECK(ch.install(lib,"CloseHandle",reinterpret_cast<void*>(&ObserveClose)));realClose=reinterpret_cast<decltype(&CloseHandle)>(ch.original);
    using Factory=IDirect3D9* (WINAPI*)(UINT);auto make=reinterpret_cast<Factory>(GetProcAddress(lib,"Direct3DCreate9"));CHECK(make);auto factory=make(D3D_SDK_VERSION);CHECK(factory);
    HWND w=CreateWindowA("STATIC","retirement-cs",WS_POPUP,0,0,64,64,nullptr,nullptr,GetModuleHandleA(nullptr),nullptr);CHECK(w);
    D3DPRESENT_PARAMETERS pp{};pp.BackBufferWidth=pp.BackBufferHeight=32;pp.BackBufferFormat=D3DFMT_A8R8G8B8;pp.BackBufferCount=1;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.Windowed=TRUE;pp.hDeviceWindow=w;pp.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* d=nullptr;CHECK(factory->CreateDevice(0,D3DDEVTYPE_HAL,w,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&d)==S_OK);
    if(d){Run(d);CHECK(d->Reset(&pp)==S_OK);CHECK(d->Release()==0);}factory->Release();DestroyWindow(w);
  }
  FreeLibrary(lib);CloseHandle(completionEvent);std::printf("checks=%u failures=%u\n",checks,failures);return failures?1:0;
}
