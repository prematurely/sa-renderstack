#include <windows.h>
#include <d3d9.h>
#include "../backend/dxvk/src/d3d9/d3d9_bridge_diagnostics.h"
#include "../backend/dxvk/src/d3d9/d3d9_bridge_lifecycle.h"
#include "../sdk/include/sa_renderstack/backend_api.h"
#include <cstdio>
#include <cstring>
#include <thread>
#include <atomic>
#include <vector>

static unsigned checks, failures;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; std::printf("FAIL line=%u %s\n", __LINE__, #c); } } while (0)
// The fallback lets the pre-fix header/DLL fail behaviorally for absent timing
// coverage, rather than making the RED a compile failure.
template<typename Counter> static auto Timed(const Counter& c, int) -> decltype(c.TimedCalls) { return c.TimedCalls; }
template<typename Counter> static UINT64 Timed(const Counter&, ...) { return 0; }
static D3D9BridgeAuditSnapshot Audit(ID3D9BridgeDiagnostics* d) {
  D3D9BridgeAuditSnapshot s{}; s.StructSize=sizeof(s); s.Version=1;
  CHECK(d->GetAudit(&s)==S_OK); return s;
}
static void TestOrdinaryTiming(IDirect3DDevice9* device, ID3D9BridgeDiagnostics* diagnostics) {
  const DWORD vertexCode[]={0xfffe0101,0x00000001,0xc00f0000,0xa0e40000,0x0000ffff};
  const DWORD pixelCode[]={0xffff0101,0x00000001,0x800f0000,0xa0e40000,0x0000ffff};
  IDirect3DVertexShader9* vertex=nullptr;IDirect3DPixelShader9* pixel=nullptr;
  CHECK(SUCCEEDED(device->CreateVertexShader(vertexCode,&vertex)));
  CHECK(SUCCEEDED(device->CreatePixelShader(pixelCode,&pixel)));
  CHECK(diagnostics->StartAudit(76,3000)==S_OK);
  for(unsigned i=0;i<32;i++) {
    const float f[4]={float(i+10),float(i+20),float(i+30),float(i+40)};
    const int integers[4]={int(i+10),int(i+20),int(i+30),int(i+40)};
    const BOOL boolean=(i&1)?FALSE:TRUE;
    for(unsigned duplicate=0;duplicate<2;duplicate++) {
      CHECK(SUCCEEDED(device->SetRenderState(D3DRS_ALPHABLENDENABLE,boolean)));
      CHECK(SUCCEEDED(device->SetSamplerState(0,D3DSAMP_ADDRESSV,(i&1)?D3DTADDRESS_CLAMP:D3DTADDRESS_WRAP)));
      CHECK(SUCCEEDED(device->SetTextureStageState(0,D3DTSS_COLOROP,(i&1)?D3DTOP_ADD:D3DTOP_SELECTARG1)));
      CHECK(SUCCEEDED(device->SetTexture(0,nullptr)));
      CHECK(SUCCEEDED(device->SetVertexShader((i&1)?nullptr:vertex)));
      CHECK(SUCCEEDED(device->SetPixelShader((i&1)?nullptr:pixel)));
      CHECK(SUCCEEDED(device->SetVertexShaderConstantF(0,f,1)));
      CHECK(SUCCEEDED(device->SetVertexShaderConstantI(0,integers,1)));
      CHECK(SUCCEEDED(device->SetVertexShaderConstantB(0,&boolean,1)));
      CHECK(SUCCEEDED(device->SetPixelShaderConstantF(0,f,1)));
      CHECK(SUCCEEDED(device->SetPixelShaderConstantI(0,integers,1)));
      CHECK(SUCCEEDED(device->SetPixelShaderConstantB(0,&boolean,1)));
    }
  }
  CHECK(device->SetVertexShaderConstantF(0,nullptr,1)==D3DERR_INVALIDCALL);
  CHECK(device->SetVertexShaderConstantF(0,nullptr,0)==S_OK);
  CHECK(device->SetPixelShaderConstantI(100000,nullptr,1)==D3DERR_INVALIDCALL);
  CHECK(diagnostics->StopAudit(76)==S_OK);
  const auto snapshot=Audit(diagnostics);
  UINT64 sumCalls=0,sumTicks=0;
  for(unsigned i=0;i<12;i++) {
    const auto& counter=snapshot.Counters[i];
    CHECK(counter.Calls==64 && Timed(counter,0)==64);
    CHECK(counter.QpcTicks>0 && counter.MaxQpcTicks>0 && counter.MaxQpcTicks<=counter.QpcTicks);
    // BOOL uploads deliberately preserve upstream unconditional upload
    // semantics; timing must not change that existing admission policy.
    CHECK((i==8 || i==11) ? counter.RedundantCalls==0 : counter.RedundantCalls>=32);
    sumCalls+=counter.Calls;sumTicks+=counter.QpcTicks;
    std::printf("ordinary category=%u calls=%llu redundant=%llu timed=%llu qpc=%llu max=%llu\n",
      i,counter.Calls,counter.RedundantCalls,Timed(counter,0),counter.QpcTicks,counter.MaxQpcTicks);
  }
  CHECK(snapshot.Total.Calls==768 && Timed(snapshot.Total,0)==768 && sumCalls==768);
  CHECK(snapshot.Total.QpcTicks==sumTicks);
  // Completed snapshots remain fixed when further ordinary setters run idle.
  CHECK(SUCCEEDED(device->SetVertexShader(nullptr)));CHECK(SUCCEEDED(device->SetPixelShader(nullptr)));
  CHECK(SUCCEEDED(device->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE)));
  const auto stopped=Audit(diagnostics);
  CHECK(stopped.Total.Calls==768 && Timed(stopped.Total,0)==768 && stopped.Total.QpcTicks==sumTicks);
  if(vertex)vertex->Release();if(pixel)pixel->Release();
}
static void TestConstantBatchTiming(IDirect3DDevice9* device, ID3D9BridgeDiagnostics* diagnostics) {
  ID3D9GtaSaCompatDevice2* compat=nullptr;
  CHECK(SUCCEEDED(device->QueryInterface(__uuidof(ID3D9GtaSaCompatDevice2),reinterpret_cast<void**>(&compat))));
  if(!compat)return;
  CHECK(diagnostics->StartAudit(77,3000)==S_OK);
  for(unsigned i=0;i<32;i++) {
    const float values[4]={float(i+100),2,3,4};
    D3D9GtaSaFloatConstantRange range{0,1,values};
    D3D9GtaSaStateBatch batch{};batch.StructSize=sizeof(batch);batch.ApiVersion=7;
    batch.VertexFloatRangeCount=1;batch.VertexFloatRanges=&range;
    CHECK(compat->SubmitStateBatch(&batch)==S_OK);
    CHECK(compat->SubmitStateBatch(&batch)==S_OK);
  }
  CHECK(diagnostics->StopAudit(77)==S_OK);
  const auto snapshot=Audit(diagnostics);
  CHECK(snapshot.Counters[6].Calls==64 && snapshot.Counters[6].RedundantCalls==32);
  CHECK(Timed(snapshot.Counters[6],0)==64 && snapshot.Counters[6].QpcTicks>0);
  std::printf("batch constant timing calls=%llu redundant=%llu timed=%llu qpc=%llu\n",
    snapshot.Counters[6].Calls,snapshot.Counters[6].RedundantCalls,
    Timed(snapshot.Counters[6],0),snapshot.Counters[6].QpcTicks);
  compat->Release();
  ID3D9GtaSaCompatDevice4* effect=nullptr;
  CHECK(SUCCEEDED(device->QueryInterface(__uuidof(ID3D9GtaSaCompatDevice4),reinterpret_cast<void**>(&effect))));
  if(!effect)return;
  CHECK(diagnostics->StartAudit(78,3000)==S_OK);
  for(unsigned i=0;i<32;i++) {
    const float floats[4]={float(i+200),3,4,5};const int integers[4]={int(i+200),3,4,5};
    const BOOL boolean=(i&1)?FALSE:TRUE;
    D3D9GtaSaFloatConstantRange f{0,1,floats};D3D9GtaSaIntConstantRange integersRange{0,1,integers};
    D3D9GtaSaBoolConstantRange b{0,1,&boolean};
    D3D9GtaSaEffectStateBatch batch{};batch.StructSize=sizeof(batch);batch.ApiVersion=7;
    batch.VertexFloatRangeCount=batch.PixelFloatRangeCount=1;
    batch.VertexFloatRanges=batch.PixelFloatRanges=&f;
    batch.VertexIntRangeCount=batch.PixelIntRangeCount=1;
    batch.VertexIntRanges=batch.PixelIntRanges=&integersRange;
    batch.VertexBoolRangeCount=batch.PixelBoolRangeCount=1;
    batch.VertexBoolRanges=batch.PixelBoolRanges=&b;
    CHECK(effect->SubmitEffectStateBatch(&batch)==S_OK);
    CHECK(effect->SubmitEffectStateBatch(&batch)==S_OK);
  }
  CHECK(diagnostics->StopAudit(78)==S_OK);
  const auto effectSnapshot=Audit(diagnostics);
  for(unsigned i=6;i<12;i++) {
    const auto& counter=effectSnapshot.Counters[i];
    CHECK(counter.Calls==64 && Timed(counter,0)==64 && counter.QpcTicks>0);
    CHECK((i==8 || i==11) ? counter.RedundantCalls==0 : counter.RedundantCalls==32);
  }
  CHECK(effectSnapshot.Total.Calls==384 && Timed(effectSnapshot.Total,0)==384);
  std::printf("effect batch timing calls=%llu timed=%llu qpc=%llu\n",effectSnapshot.Total.Calls,
    Timed(effectSnapshot.Total,0),effectSnapshot.Total.QpcTicks);
  effect->Release();
}
static void TestAuditAdmission(IDirect3DDevice9* device, ID3D9BridgeDiagnostics* diagnostics) {
  DWORD oldRender=0,oldSampler=0;
  CHECK(device->GetRenderState(D3DRS_ALPHABLENDENABLE,&oldRender)==S_OK);
  CHECK(device->GetSamplerState(0,D3DSAMP_MAGFILTER,&oldSampler)==S_OK);
  CHECK(diagnostics->StartAudit(79,3000)==S_OK);
  CHECK(device->BeginStateBlock()==S_OK);
  for(unsigned i=0;i<64;i++) {
    const float floats[4]={float(i+500),2,3,4};const int integers[4]={int(i+500),2,3,4};
    const BOOL boolean=(i&1)?FALSE:TRUE;
    CHECK(device->SetRenderState(D3DRS_ALPHABLENDENABLE,boolean)==S_OK);
    CHECK(device->SetSamplerState(0,D3DSAMP_MAGFILTER,(i&1)?D3DTEXF_POINT:D3DTEXF_LINEAR)==S_OK);
    CHECK(device->SetTextureStageState(0,D3DTSS_COLOROP,(i&1)?D3DTOP_ADD:D3DTOP_SELECTARG1)==S_OK);
    CHECK(device->SetTexture(0,nullptr)==S_OK);
    CHECK(device->SetVertexShader(nullptr)==S_OK);
    CHECK(device->SetPixelShader(nullptr)==S_OK);
    CHECK(device->SetVertexShaderConstantF(0,floats,1)==S_OK);
    CHECK(device->SetVertexShaderConstantI(0,integers,1)==S_OK);
    CHECK(device->SetVertexShaderConstantB(0,&boolean,1)==S_OK);
    CHECK(device->SetPixelShaderConstantF(0,floats,1)==S_OK);
    CHECK(device->SetPixelShaderConstantI(0,integers,1)==S_OK);
    CHECK(device->SetPixelShaderConstantB(0,&boolean,1)==S_OK);
  }
  IDirect3DStateBlock9* recorded=nullptr;CHECK(device->EndStateBlock(&recorded)==S_OK && recorded);
  auto recording=Audit(diagnostics);
  for(unsigned i=0;i<12;i++) {
    const auto& counter=recording.Counters[i];
    CHECK(counter.Calls==0 && Timed(counter,0)==0 && counter.QpcTicks==0 && counter.MaxQpcTicks==0);
    std::printf("record-only category=%u attempts=64 calls=%llu timed=%llu qpc=%llu\n",
      i,counter.Calls,Timed(counter,0),counter.QpcTicks);
  }
  CHECK(recording.Total.Calls==2 && Timed(recording.Total,0)==0 && recording.Total.QpcTicks==0);
  CHECK(recording.Counters[13].Calls==1 && recording.Counters[14].Calls==1);
  DWORD currentRender=0,currentSampler=0;
  CHECK(device->GetRenderState(D3DRS_ALPHABLENDENABLE,&currentRender)==S_OK && currentRender==oldRender);
  CHECK(device->GetSamplerState(0,D3DSAMP_MAGFILTER,&currentSampler)==S_OK && currentSampler==oldSampler);
  for(unsigned i=0;i<32;i++) {
    CHECK(device->SetVertexShaderConstantF(0,nullptr,1)==D3DERR_INVALIDCALL);
    CHECK(device->SetVertexShaderConstantI(0,nullptr,1)==D3DERR_INVALIDCALL);
    CHECK(device->SetVertexShaderConstantB(0,nullptr,1)==D3DERR_INVALIDCALL);
    CHECK(device->SetPixelShaderConstantF(0,nullptr,1)==D3DERR_INVALIDCALL);
    CHECK(device->SetPixelShaderConstantI(0,nullptr,1)==D3DERR_INVALIDCALL);
    CHECK(device->SetPixelShaderConstantB(0,nullptr,1)==D3DERR_INVALIDCALL);
    CHECK(device->SetVertexShaderConstantF(0,nullptr,0)==S_OK);
    CHECK(device->SetVertexShaderConstantI(0,nullptr,0)==S_OK);
    CHECK(device->SetVertexShaderConstantB(0,nullptr,0)==S_OK);
    CHECK(device->SetPixelShaderConstantF(0,nullptr,0)==S_OK);
    CHECK(device->SetPixelShaderConstantI(0,nullptr,0)==S_OK);
    CHECK(device->SetPixelShaderConstantB(0,nullptr,0)==S_OK);
    CHECK(device->SetRenderState(static_cast<D3DRENDERSTATETYPE>(256),0)==S_OK);
    CHECK(device->SetSamplerState(999,D3DSAMP_MAGFILTER,D3DTEXF_POINT)==S_OK);
    CHECK(device->SetTexture(999,nullptr)==S_OK);
  }
  CHECK(diagnostics->StopAudit(79)==S_OK);
  auto rejected=Audit(diagnostics);
  for(unsigned i=0;i<12;i++) {
    const auto& counter=rejected.Counters[i];
    CHECK(counter.Calls==0 && Timed(counter,0)==0 && counter.QpcTicks==0 && counter.MaxQpcTicks==0);
  }
  CHECK(rejected.Total.Calls==2 && Timed(rejected.Total,0)==0 && rejected.Total.QpcTicks==0);
  std::printf("recording and rejected/zero-count total calls=%llu timed=%llu qpc=%llu\n",
    rejected.Total.Calls,Timed(rejected.Total,0),rejected.Total.QpcTicks);
  if(recorded)recorded->Release();
  // Positive control: after recording ends, the same admitted native path
  // still records one elapsed span per counted setter.
  CHECK(diagnostics->StartAudit(80,3000)==S_OK);
  for(unsigned i=0;i<64;i++)
    CHECK(device->SetSamplerState(0,D3DSAMP_MAGFILTER,(i&1)?D3DTEXF_POINT:D3DTEXF_LINEAR)==S_OK);
  CHECK(diagnostics->StopAudit(80)==S_OK);
  const auto positive=Audit(diagnostics);
  CHECK(positive.Counters[1].Calls==64 && Timed(positive.Counters[1],0)==64 && positive.Counters[1].QpcTicks>0);
  std::printf("post-record native sampler calls=%llu timed=%llu qpc=%llu\n",
    positive.Counters[1].Calls,Timed(positive.Counters[1],0),positive.Counters[1].QpcTicks);
}
static unsigned CaptureHud(HWND window, const char* filename) {
  RECT rect{};GetClientRect(window,&rect);
  const int width=rect.right,height=rect.bottom;
  BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;
  info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
  HDC dc=GetDC(window),copy=CreateCompatibleDC(dc);void* pixels=nullptr;
  HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
  auto old=SelectObject(copy,bitmap);
  CHECK(BitBlt(copy,0,0,width,height,dc,0,0,SRCCOPY));
  unsigned textPixels=0;
  auto data=static_cast<unsigned char*>(pixels);
  for(int y=40;y<180 && y<height;y++) for(int x=4;x<width;x++) {
    const auto p=data+4*(y*width+x);
    if (p[0]>100 && p[1]>150 && p[2]<230) ++textPixels;
  }
  BITMAPFILEHEADER file{};file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(info.bmiHeader);
  file.bfSize=file.bfOffBits+width*height*4;
  if(auto output=std::fopen(filename,"wb")) {
    std::fwrite(&file,sizeof(file),1,output);std::fwrite(&info.bmiHeader,sizeof(info.bmiHeader),1,output);
    std::fwrite(pixels,width*height*4,1,output);std::fclose(output);
  } else CHECK(false);
  SelectObject(copy,old);DeleteObject(bitmap);DeleteDC(copy);ReleaseDC(window,dc);
  return textPixels;
}
static void PresentHud(IDirect3DDevice9* device, HWND window) {
  for(unsigned i=0;i<8;i++) {
    MSG message{};while(PeekMessage(&message,nullptr,0,0,PM_REMOVE)) {TranslateMessage(&message);DispatchMessage(&message);}
    CHECK(SUCCEEDED(device->Clear(0,nullptr,D3DCLEAR_TARGET,0xff123456,1,0)));
    CHECK(SUCCEEDED(device->Present(nullptr,nullptr,nullptr,nullptr)));
    Sleep(60);
  }
  Sleep(100);
}
struct ReleaseResult { ID3D9BridgeDiagnostics* Diagnostics; D3D9BridgeAuditSnapshot Snapshot; bool Called; };
static void STDMETHODCALLTYPE Observe(void* context, IDirect3DDevice9*,
  D3D9BridgeEvent event, HRESULT, D3DPRESENT_PARAMETERS*, const D3D9BridgePresentArgs*) {
  if (event != D3D9BridgeEvent::FinalRelease) return;
  auto& result=*static_cast<ReleaseResult*>(context);
  result.Snapshot=Audit(result.Diagnostics);result.Called=true;
}
int main(int argc,char**argv) {
  if (argc!=2 && argc!=3) return 2;
  const bool disabled=argc==3 && std::strcmp(argv[2],"--disabled")==0;
  const bool render=argc==3 && std::strcmp(argv[2],"--render")==0;
  auto module=LoadLibraryA(argv[1]);
  if (!module) {std::printf("FAIL LoadLibrary %lu\n",GetLastError());return 2;}
  auto create=reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(module,"Direct3DCreate9"));
  auto factory=create?create(D3D_SDK_VERSION):nullptr;
  if (!factory) return 2;
  auto window=CreateWindowA("STATIC","Measurement backend fixture",WS_OVERLAPPEDWINDOW,0,0,640,480,nullptr,nullptr,GetModuleHandle(nullptr),nullptr);
  D3DPRESENT_PARAMETERS pp{};
  pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=window;
  pp.BackBufferWidth=640;pp.BackBufferHeight=480;pp.BackBufferFormat=D3DFMT_X8R8G8B8;
  pp.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
  IDirect3DDevice9* device=nullptr;
  auto hr=factory->CreateDevice(0,D3DDEVTYPE_HAL,window,D3DCREATE_HARDWARE_VERTEXPROCESSING|D3DCREATE_MULTITHREADED,&pp,&device);
  if (FAILED(hr)) { std::printf("FAIL CreateDevice 0x%08lx\n",(unsigned long)hr);return 2; }
  ID3D9BridgeDiagnostics* d=nullptr;
  hr=device->QueryInterface(IID_D3D9BridgeDiagnostics,reinterpret_cast<void**>(&d));
  std::printf("diagnostics QI hr=0x%08lx\n",(unsigned long)hr);
  CHECK(hr==S_OK && d);
  if (!d) return 1;
  if (render) {
    ShowWindow(window,SW_SHOWNORMAL);UpdateWindow(window);
    PresentHud(device,window);
    const auto idlePixels=CaptureHud(window,"hud-unowned.bmp");
    CHECK(idlePixels==0);
    CHECK(d->SetCaptureOwner(TRUE)==S_OK);
    D3D9BridgeHudSnapshot visible{};visible.StructSize=sizeof(visible);visible.Version=1;
    visible.Visible=1;visible.LineCount=8;visible.CaptureId=5001;
    const char* lines[]={"RenderStack capture 5001: state attribution","Elapsed 1.0 / 3.0 sec; progress 33%",
      "Baseline: 100 FPS, avg 10 ms, max 12 ms","CPU: 85 FPS; samples 2100, missed 2",
      "Hotspot: verified function @ module+RVA","State API: 300 calls, 200 redundant",
      "Journal: inclusive spans; backend available","Complete report: out/perf/measurement"};
    for(unsigned i=0;i<8;i++)std::strcpy(visible.Lines[i],lines[i]);
    CHECK(d->PublishHud(&visible)==S_OK);PresentHud(device,window);
    const auto visiblePixels=CaptureHud(window,"hud-published.bmp");
    CHECK(visiblePixels>100);
    visible.Visible=0;CHECK(d->PublishHud(&visible)==S_OK);
    CHECK(d->SetCaptureOwner(FALSE)==S_OK);PresentHud(device,window);
    const auto hiddenPixels=CaptureHud(window,"hud-hidden.bmp");CHECK(hiddenPixels==0);
    std::printf("post-HUD client pixels unowned=%u visible=%u hidden=%u\n",idlePixels,visiblePixels,hiddenPixels);
    d->Release();device->Release();factory->Release();DestroyWindow(window);
    std::printf("measurement backend HUD checks=%u failures=%u\n",checks,failures);
    return failures?1:0;
  }
  IUnknown* identity=nullptr;CHECK(d->QueryInterface(IID_IUnknown,reinterpret_cast<void**>(&identity))==S_OK);
  CHECK(identity==static_cast<IUnknown*>(device));if(identity)identity->Release();
  const auto initial=Audit(d);
  if (disabled) {
    CHECK(initial.Status==D3D9_BRIDGE_AUDIT_UNAVAILABLE && !initial.Available);
    CHECK(d->SetCaptureOwner(TRUE)==S_OK);
    CHECK(d->StartAudit(77,3000)==D3DERR_NOTAVAILABLE);
    const auto unavailable=Audit(d);
    CHECK(unavailable.CaptureId==77 && unavailable.Status==D3D9_BRIDGE_AUDIT_UNAVAILABLE && !unavailable.Active);
    d->Release();device->Release();factory->Release();DestroyWindow(window);
    std::printf("measurement backend unavailable checks=%u failures=%u\n",checks,failures);
    return failures?1:0;
  }
  CHECK(initial.Status==D3D9_BRIDGE_AUDIT_IDLE && initial.CaptureId==0 && !initial.Active);
  D3D9BridgeHudSnapshot idle{};idle.StructSize=sizeof(idle);idle.Version=1;
  CHECK(d->GetHud(&idle)==S_OK && !idle.Visible);
  CHECK(d->SetCaptureOwner(TRUE)==S_OK);
  CHECK(d->GetHud(&idle)==S_OK && idle.Visible && idle.LineCount==1);
  CHECK(d->StartAudit(0,3000)==D3DERR_INVALIDCALL);
  CHECK(d->StartAudit(71,0)==D3DERR_INVALIDCALL);
  CHECK(d->StartAudit(71,3000)==S_OK);
  CHECK(d->StartAudit(72,3000)==D3DERR_INVALIDCALL);
  CHECK(d->StopAudit(72)==D3DERR_INVALIDCALL);
  auto s=Audit(d);CHECK(s.Active && s.CaptureId==71);
  DWORD old=0;CHECK(SUCCEEDED(device->GetRenderState(D3DRS_ZENABLE,&old)));
  CHECK(SUCCEEDED(device->SetRenderState(D3DRS_ZENABLE,old)));
  CHECK(SUCCEEDED(device->SetRenderState(D3DRS_ZENABLE,old==D3DZB_FALSE?D3DZB_TRUE:D3DZB_FALSE)));
  CHECK(SUCCEEDED(device->SetRenderState(D3DRS_ZENABLE,old==D3DZB_FALSE?D3DZB_TRUE:D3DZB_FALSE)));
  const float constants[4]={1,2,3,4};
  CHECK(SUCCEEDED(device->SetVertexShaderConstantF(0,constants,1)));
  CHECK(SUCCEEDED(device->SetVertexShaderConstantF(0,constants,1)));
  IDirect3DStateBlock9* block=nullptr;
  CHECK(SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL,&block)));
  CHECK(block && SUCCEEDED(block->Capture()));if(block)block->Release();
  device->Clear(0,nullptr,D3DCLEAR_TARGET,0xff123456,1,0);
  auto present=device->Present(nullptr,nullptr,nullptr,nullptr);
  CHECK(SUCCEEDED(present));
  CHECK(d->StopAudit(71)==S_OK);
  s=Audit(d);
  CHECK(s.CaptureId==71 && s.Available && !s.Active && s.Status==D3D9_BRIDGE_AUDIT_COMPLETE);
  CHECK(s.Counters[0].Calls==3 && s.Counters[0].RedundantCalls==2);
  CHECK(Timed(s.Counters[0],0)==3 && s.Counters[0].MaxQpcTicks<=s.Counters[0].QpcTicks);
  CHECK(s.Counters[6].Calls==2 && s.Counters[6].RedundantCalls==1 && s.Counters[6].Units==2);
  CHECK(Timed(s.Counters[6],0)==2);
  CHECK(s.Counters[15].Calls>=1 && s.Counters[15].QpcTicks>0 && s.QpcFrequency>0);
  CHECK(Timed(s.Counters[15],0)==s.Counters[15].Calls);
  CHECK(s.Counters[12].Calls==1 && Timed(s.Counters[12],0)==0 && s.Counters[12].QpcTicks==0);
  CHECK(s.Total.Calls==7 && Timed(s.Total,0)==6);
  std::printf("ordinary timing renderCalls=%llu renderTimed=%llu renderQpc=%llu constantCalls=%llu constantTimed=%llu constantQpc=%llu stateblockTimed=%llu\n",
    s.Counters[0].Calls,Timed(s.Counters[0],0),s.Counters[0].QpcTicks,
    s.Counters[6].Calls,Timed(s.Counters[6],0),s.Counters[6].QpcTicks,Timed(s.Counters[15],0));
  CHECK(s.ObservedPresentBoundaries==1);
  CHECK(d->StopAudit(71)==S_FALSE);
  CHECK(Audit(d).Total.Calls==s.Total.Calls);
  // Existing sampler/texture/prepare-draw paths, with true duplicates and
  // changed bindings; removing any CopyStateAudit category must fail here.
  DWORD address=0;CHECK(SUCCEEDED(device->GetSamplerState(0,D3DSAMP_ADDRESSU,&address)));
  IDirect3DTexture9* texture=nullptr;
  CHECK(SUCCEEDED(device->CreateTexture(1,1,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&texture,nullptr)));
  CHECK(d->StartAudit(75,3000)==S_OK);
  CHECK(SUCCEEDED(device->SetSamplerState(0,D3DSAMP_ADDRESSU,address)));
  CHECK(SUCCEEDED(device->SetSamplerState(0,D3DSAMP_ADDRESSU,address==D3DTADDRESS_WRAP?D3DTADDRESS_CLAMP:D3DTADDRESS_WRAP)));
  CHECK(SUCCEEDED(device->SetSamplerState(0,D3DSAMP_ADDRESSU,address==D3DTADDRESS_WRAP?D3DTADDRESS_CLAMP:D3DTADDRESS_WRAP)));
  CHECK(SUCCEEDED(device->SetTexture(0,nullptr)));
  CHECK(SUCCEEDED(device->SetTexture(0,texture)));
  CHECK(SUCCEEDED(device->SetTexture(0,texture)));
  struct Vertex {float x,y,z,w;DWORD color;} vertices[]={{30,30,0,1,0xffffffff},{40,30,0,1,0xffffffff},{30,40,0,1,0xffffffff}};
  CHECK(SUCCEEDED(device->SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE)));
  CHECK(SUCCEEDED(device->BeginScene()));
  CHECK(SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST,1,vertices,sizeof(Vertex))));
  CHECK(SUCCEEDED(device->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,2)));
  // Change consumed inputs, then restore them before the next draw. This
  // genuinely dirties bindings while retaining the submitted sampler/view.
  CHECK(SUCCEEDED(device->SetSamplerState(0,D3DSAMP_ADDRESSU,address)));
  CHECK(SUCCEEDED(device->SetSamplerState(0,D3DSAMP_ADDRESSU,address==D3DTADDRESS_WRAP?D3DTADDRESS_CLAMP:D3DTADDRESS_WRAP)));
  CHECK(SUCCEEDED(device->SetTexture(0,nullptr)));
  CHECK(SUCCEEDED(device->SetTexture(0,texture)));
  CHECK(SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST,1,vertices,sizeof(Vertex))));
  CHECK(SUCCEEDED(device->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,4)));
  CHECK(SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST,1,vertices,sizeof(Vertex))));
  CHECK(SUCCEEDED(device->EndScene()));
  CHECK(d->StopAudit(75)==S_OK);
  const auto bindings=Audit(d);
  CHECK(bindings.Counters[1].Calls==7 && bindings.Counters[1].RedundantCalls==2);
  CHECK(Timed(bindings.Counters[1],0)==7);
  CHECK(bindings.Counters[3].Calls==5 && bindings.Counters[3].RedundantCalls==2);
  CHECK(Timed(bindings.Counters[3],0)==5);
  CHECK(bindings.PrepareDraws==3 && bindings.PrepareCategoryEvents>0 && bindings.PrepareMaxCategoriesPerDraw>0);
  CHECK(bindings.Prepare[7].Draws>=1 && bindings.Prepare[8].Draws>=1);
  // The final high-bit-only sRGB change must not dirty either binding.
  CHECK(bindings.Prepare[7].Draws==2 && bindings.Prepare[8].Draws==2);
  CHECK(bindings.SamplerBindingCacheHits>0 && bindings.TextureBindingCacheHits>0);
  CHECK(bindings.ResourceBindingCacheInvalidations==0);
  std::printf("native counters samplerCalls=%llu textureCalls=%llu prepareDraws=%llu samplerHits=%llu textureHits=%llu invalidations=%llu\n",
    bindings.Counters[1].Calls,bindings.Counters[3].Calls,bindings.PrepareDraws,
    bindings.SamplerBindingCacheHits,bindings.TextureBindingCacheHits,bindings.ResourceBindingCacheInvalidations);
  CHECK(SUCCEEDED(device->SetTexture(0,nullptr)));if(texture)texture->Release();
  TestOrdinaryTiming(device,d);
  TestConstantBatchTiming(device,d);
  TestAuditAdmission(device,d);
  CHECK(d->GetAudit(nullptr)==E_POINTER);
  D3D9BridgeAuditSnapshot invalid{};CHECK(d->GetAudit(&invalid)==D3DERR_INVALIDCALL);
  D3D9BridgeHudSnapshot hud{};hud.StructSize=sizeof(hud);hud.Version=1;hud.CaptureId=71;hud.Visible=1;hud.LineCount=2;
  std::strcpy(hud.Lines[0],"RenderStack capture 71");std::strcpy(hud.Lines[1],"Backend native API counters verified");
  CHECK(d->PublishHud(nullptr)==E_POINTER);
  CHECK(d->PublishHud(&hud)==S_OK);
  D3D9BridgeHudSnapshot copy{};copy.StructSize=sizeof(copy);copy.Version=1;CHECK(d->GetHud(&copy)==S_OK);
  CHECK(std::strcmp(copy.Lines[0],hud.Lines[0])==0 && copy.CaptureId==71);
  hud.Version=2;CHECK(d->PublishHud(&hud)==D3DERR_INVALIDCALL);hud.Version=1;
  hud.StructSize=sizeof(hud)-1;CHECK(d->PublishHud(&hud)==D3DERR_INVALIDCALL);hud.StructSize=sizeof(hud);
  hud.LineCount=9;CHECK(d->PublishHud(&hud)==D3DERR_INVALIDCALL);hud.LineCount=2;
  hud.ProgressPermille=1001;CHECK(d->PublishHud(&hud)==D3DERR_INVALIDCALL);hud.ProgressPermille=0;
  std::memset(hud.Lines[0],'x',sizeof(hud.Lines[0]));CHECK(d->PublishHud(&hud)==D3DERR_INVALIDCALL);
  CHECK(d->GetHud(nullptr)==E_POINTER);
  // Fixed snapshots are copied coherently while a publisher updates them.
  std::atomic<bool> copying{true}, coherent{true};
  std::thread reader([&] {while(copying) {D3D9BridgeHudSnapshot c{};c.StructSize=sizeof(c);c.Version=1;
    if(d->GetHud(&c)!=S_OK || (c.CaptureId!=71 && c.CaptureId!=91))coherent=false;}});
  std::memset(hud.Lines,0,sizeof(hud.Lines));hud.CaptureId=91;
  for(unsigned i=0;i<1000;++i) CHECK(d->PublishHud(&hud)==S_OK);
  copying=false;reader.join();CHECK(coherent);
  CHECK(d->StartAudit(81,3000)==S_OK);
  CHECK(SUCCEEDED(device->SetRenderState(D3DRS_ZENABLE,D3DZB_FALSE)));
  CHECK(SUCCEEDED(device->Reset(&pp)));
  auto reset=Audit(d);CHECK(reset.CaptureId==81 && !reset.Active && reset.Status==D3D9_BRIDGE_AUDIT_INTERRUPTED);
  CHECK(reset.Total.Calls==1);
  CHECK(d->SetCaptureOwner(FALSE)==S_OK);
  CHECK(Audit(d).CaptureId==81);
  CHECK(d->StartAudit(85,3000)==D3DERR_INVALIDCALL);
  CHECK(d->SetCaptureOwner(TRUE)==S_OK);
  CHECK(d->StartAudit(85,3000)==S_OK);
  CHECK(d->SetCaptureOwner(FALSE)==S_OK);
  const auto abandoned=Audit(d);CHECK(abandoned.CaptureId==85 && !abandoned.Active && abandoned.Status==D3D9_BRIDGE_AUDIT_INTERRUPTED);
  CHECK(d->SetCaptureOwner(TRUE)==S_OK);
  CHECK(d->StartAudit(91,3000)==S_OK);
  CHECK(SUCCEEDED(device->SetRenderState(D3DRS_ZENABLE,D3DZB_TRUE)));
  ID3D9BridgeLifecycle* lifecycle=nullptr;
  CHECK(device->QueryInterface(IID_D3D9BridgeLifecycle,reinterpret_cast<void**>(&lifecycle))==S_OK);
  ReleaseResult release{d,{ },false};
  D3D9BridgeObserver observer{sizeof(observer),&release,&Observe};
  CHECK(lifecycle && lifecycle->SetObserver(&observer)==S_OK);
  if(lifecycle)lifecycle->Release();
  d->Release();device->Release();
  CHECK(release.Called && release.Snapshot.CaptureId==91 && !release.Snapshot.Active &&
    release.Snapshot.Status==D3D9_BRIDGE_AUDIT_INTERRUPTED && release.Snapshot.Total.Calls==1);
  factory->Release();DestroyWindow(window);
  std::printf("measurement backend checks=%u failures=%u\n",checks,failures);
  return failures?1:0;
}
