#include <windows.h>
#include <tlhelp32.h>
#include <d3d9.h>
#include "../sdk/include/sa_renderstack/backend_api.h"
#include "../backend/dxvk/src/d3d9/d3d9_bridge_diagnostics.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

// Independent ABI consumer: do not include the implementation's private header.
static const GUID ThreadStatsIID={0xd0967703,0x3642,0x49d2,{0x93,0xa8,0x91,0xa2,0xd1,0xe0,0x31,0x50}};
struct ThreadSnapshot {
  UINT StructSize,Version,MainThreadId,CsThreadId,AvailableMask,Reserved;
  UINT64 WallUs,MainCpu100ns,CsCpu100ns,CsQueueIdleUs,OrderedDispatched,OrderedCompleted;
  UINT64 CsSyncCount,CsSyncUs,GpuSyncCount,GpuSyncUs,GpuIdleUs,Chunks,Submits;
};
struct ThreadStats : IUnknown { virtual HRESULT STDMETHODCALLTYPE GetThreadStats(ThreadSnapshot*)=0; };
static unsigned checks,failures;
static unsigned privateDestructions;
static const GUID LifetimeIID={0x64443822,0xc10e,0x4c92,{0x92,0x30,0x73,0x42,0x10,0xaa,0x48,0x50}};
struct Lifetime final : IUnknown {
  ULONG refs=1;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {if(!out)return E_POINTER;*out=nullptr;if(id!=__uuidof(IUnknown))return E_NOINTERFACE;*out=this;AddRef();return S_OK;}
  ULONG STDMETHODCALLTYPE AddRef() override{return ++refs;}
  ULONG STDMETHODCALLTYPE Release() override{const auto left=--refs;if(!left){++privateDestructions;delete this;}return left;}
};
#define CHECK(c) do {++checks;if(!(c)){++failures;std::printf("FAIL line=%u %s\n",__LINE__,#c);}}while(0)
static UINT64 Now(){LARGE_INTEGER q,f;QueryPerformanceCounter(&q);QueryPerformanceFrequency(&f);return UINT64(q.QuadPart)*1000000/UINT64(f.QuadPart);}
static ThreadSnapshot Snap(ThreadStats* stats){ThreadSnapshot s{};s.StructSize=sizeof(s);s.Version=1;CHECK(stats->GetThreadStats(&s)==S_OK);return s;}
static void Print(const char* name,const ThreadSnapshot& a,const ThreadSnapshot& b){
  std::printf("activity case=%s wallUs=%llu mainCpuUs=%llu csCpuUs=%llu csQueueIdleUs=%llu dispatched=%llu completed=%llu pendingStart=%llu pendingEnd=%llu csWaitUs=%llu resourceWaitUs=%llu chunks=%llu submits=%llu\n",name,
    b.WallUs-a.WallUs,(b.MainCpu100ns-a.MainCpu100ns)/10,(b.CsCpu100ns-a.CsCpu100ns)/10,b.CsQueueIdleUs-a.CsQueueIdleUs,
    b.OrderedDispatched-a.OrderedDispatched,b.OrderedCompleted-a.OrderedCompleted,a.OrderedDispatched-a.OrderedCompleted,b.OrderedDispatched-b.OrderedCompleted,
    b.CsSyncUs-a.CsSyncUs,b.GpuSyncUs-a.GpuSyncUs,b.Chunks-a.Chunks,b.Submits-a.Submits);
}
static void Modules(){
  auto h=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,GetCurrentProcessId());CHECK(h!=INVALID_HANDLE_VALUE);
  MODULEENTRY32 e{};e.dwSize=sizeof(e);if(Module32First(h,&e))do{
    std::string name=e.szModule;for(auto& c:name)c=char(tolower(c));
    CHECK(name.find("propershaders")==std::string::npos&&name.find("cleo")==std::string::npos&&name.find("flacompat")==std::string::npos);
    std::printf("module=%s\n",e.szExePath);
  }while(Module32Next(h,&e));CloseHandle(h);
}
struct Vertex{float x,y,z,rhw;DWORD color;};
static const Vertex Triangle[]={{0,0,.5f,1,0xff00ff00},{64,0,.5f,1,0xff00ff00},{0,64,.5f,1,0xff00ff00}};
static void Draw(IDirect3DDevice9* d){d->BeginScene();d->DrawPrimitiveUP(D3DPT_TRIANGLELIST,1,Triangle,sizeof(Vertex));d->EndScene();}
static void Flush(IDirect3DDevice9* d){
  IDirect3DQuery9* q=nullptr;const auto created=d->CreateQuery(D3DQUERYTYPE_EVENT,&q);CHECK(created==S_OK&&q);if(!q)return;
  const auto issued=q->Issue(D3DISSUE_END);CHECK(issued==S_OK);HRESULT result=issued;
  const auto deadline=GetTickCount64()+20000;
  if(SUCCEEDED(issued)){do{result=q->GetData(nullptr,0,D3DGETDATA_FLUSH);if(result==S_FALSE)Sleep(0);}while(result==S_FALSE&&GetTickCount64()<deadline);}
  CHECK(result==S_OK);q->Release();
}
static void Work(IDirect3DDevice9* d,ID3D9GtaSaCompatDevice3* j,const std::string& mode,unsigned n){
  IDirect3DStateBlock9* block=nullptr;
  const bool stateBlock=mode=="stateblock"||mode=="stateblock-tail";
  if(stateBlock) {d->BeginStateBlock();d->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);d->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_WRAP);if(mode=="stateblock-tail"){const float saved[4]={1,2,3,4};d->SetVertexShaderConstantF(255,saved,1);}d->EndStateBlock(&block);}
  float first[256*4],second[256*4];for(unsigned k=0;k<1024;k++){first[k]=float(10000+k);second[k]=float(20000+k);}
  for(unsigned i=0;i<n;i++){
    if(mode=="control"){d->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);continue;}
    if(mode=="draw"){Draw(d);continue;}
    if(stateBlock){d->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);if(mode=="stateblock-tail"){const float changed[4]={float(i+10),7,8,9};d->SetVertexShaderConstantF(255,changed,1);}block->Apply();continue;}
    if(mode=="capture-repeat"||mode=="capture-overlap"){
      if(j)j->BeginStateJournal();
      if(mode=="capture-repeat") {d->SetVertexShaderConstantF(0,first,256);d->SetVertexShaderConstantF(0,second,256);d->SetVertexShaderConstantF(0,first,256);}
      else {d->SetVertexShaderConstantF(31,first,128);d->SetVertexShaderConstantF(0,second,64);d->SetVertexShaderConstantF(127,first,129);}
      if(j)j->RestoreStateJournal();continue;
    }
    if(j)j->BeginStateJournal();
    if(mode!="empty"){
      d->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);
      d->SetSamplerState(15,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);
      if(mode=="tail"||mode=="dense"||mode=="tail-draw"){
        float values[256*4];for(unsigned k=0;k<1024;k++)values[k]=float(i+k+100);
        d->SetVertexShaderConstantF(mode!="dense"?255:0,values,mode!="dense"?1:256);
      }
      if(mode=="dense")for(unsigned s=0;s<16;s++)for(unsigned type=1;type<=13;type++)d->SetSamplerState(s,D3DSAMPLERSTATETYPE(type),1+(i&1));
    }
    if(mode=="sparse-draw"||mode=="tail-draw")Draw(d);
    if(j)j->RestoreStateJournal();
  }if(block)block->Release();
}
static void OverlappingConstants(IDirect3DDevice9* d,ID3D9GtaSaCompatDevice3* journal,bool software){
  if(!journal)return;
  const unsigned floats=software?8192:256, bools=software?2048:16;
  std::vector<float> initial(floats*4),changed(floats*4);for(unsigned i=0;i<initial.size();i++){initial[i]=float(3000+i);changed[i]=float(9000+i);}
  int integers[64],otherInts[64];BOOL boolean[2048],otherBools[2048];
  for(unsigned i=0;i<64;i++){integers[i]=int(100+i);otherInts[i]=-int(i+5);}
  for(unsigned i=0;i<2048;i++){boolean[i]=BOOL(i&1);otherBools[i]=!boolean[i];}
  for(bool vertex:{false,true}){
    const unsigned fcount=vertex?floats:224,bcount=vertex?bools:16;
    auto sf=[&](UINT s,const float* p,UINT n){return vertex?d->SetVertexShaderConstantF(s,p,n):d->SetPixelShaderConstantF(s,p,n);};
    auto si=[&](UINT s,const int* p,UINT n){return vertex?d->SetVertexShaderConstantI(s,p,n):d->SetPixelShaderConstantI(s,p,n);};
    auto sb=[&](UINT s,const BOOL* p,UINT n){return vertex?d->SetVertexShaderConstantB(s,p,n):d->SetPixelShaderConstantB(s,p,n);};
    CHECK(sf(0,initial.data(),fcount)==S_OK);CHECK(si(0,integers,16)==S_OK);CHECK(sb(0,boolean,bcount)==S_OK);
    for(unsigned cycle=0;cycle<8;cycle++){
      CHECK(journal->BeginStateJournal()==S_OK);
      // First write can be identical; later writes must still capture the original.
      CHECK(sf(0,initial.data(),fcount)==S_OK);
      for(auto range:{std::pair<UINT,UINT>{31,65},{0,33},{63,65},{fcount-2,2},{32,32},{0,fcount},{1,fcount-2},{0,fcount}})
        CHECK(sf(range.first,changed.data(),range.second)==S_OK);
      for(auto range:{std::pair<UINT,UINT>{7,9},{0,8},{1,14},{0,16}})CHECK(si(range.first,otherInts,range.second)==S_OK);
      if(bcount>32){CHECK(sb(31,otherBools,34)==S_OK);CHECK(sb(63,otherBools,65)==S_OK);}
      CHECK(sb(bcount-1,otherBools,1)==S_OK);CHECK(sb(0,otherBools,bcount)==S_OK);CHECK(sb(1,boolean,bcount-2)==S_OK);
      CHECK(journal->RestoreStateJournal()==S_OK);
      std::vector<float> result(fcount*4);int ri[64];std::vector<BOOL> rb(bcount);
      // This backend's public getters preserve their hardware-window clamp,
      // even on mixed devices. Exercise extended writes without changing it.
      const unsigned readableFloats=std::min(fcount,vertex?256u:224u),readableBools=16;
      CHECK((vertex?d->GetVertexShaderConstantF(0,result.data(),fcount):d->GetPixelShaderConstantF(0,result.data(),fcount))==S_OK&&!memcmp(result.data(),initial.data(),readableFloats*4*sizeof(float)));
      CHECK((vertex?d->GetVertexShaderConstantI(0,ri,16):d->GetPixelShaderConstantI(0,ri,16))==S_OK&&!memcmp(ri,integers,sizeof(ri)));
      CHECK((vertex?d->GetVertexShaderConstantB(0,rb.data(),bcount):d->GetPixelShaderConstantB(0,rb.data(),bcount))==S_OK&&!memcmp(rb.data(),boolean,readableBools*sizeof(BOOL)));
    }
  }
}
static void BindingPixels(IDirect3DDevice9* d,ID3D9GtaSaCompatDevice3* journal,bool verifyPropagation){
  IDirect3DTexture9* textures[2]={};
  for(unsigned i=0;i<2;i++){
    CHECK(d->CreateTexture(1,1,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&textures[i],nullptr)==S_OK);
    D3DLOCKED_RECT lock{};CHECK(textures[i]->LockRect(0,&lock,nullptr,0)==S_OK);
    *static_cast<DWORD*>(lock.pBits)=i?0xff202020:0xff808080;CHECK(textures[i]->UnlockRect(0)==S_OK);
  }
  struct TV{float x,y,z,rhw,u,v;};
  TV vertices[]={{0,0,.5f,1,2,2},{64,0,.5f,1,2,2},{0,64,.5f,1,2,2}};
  d->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1);d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);d->SetRenderState(D3DRS_ZENABLE,FALSE);
  d->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1);d->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE);
  d->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1);d->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE);
  d->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_POINT);d->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_POINT);
  d->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_WRAP);d->SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_WRAP);
  IDirect3DSurface9* target=nullptr,*read=nullptr;CHECK(d->GetRenderTarget(0,&target)==S_OK);
  CHECK(d->CreateOffscreenPlainSurface(64,64,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&read,nullptr)==S_OK);
  auto pixel=[&](DWORD expected){
    CHECK(d->BeginScene()==S_OK);CHECK(d->DrawPrimitiveUP(D3DPT_TRIANGLELIST,1,vertices,sizeof(TV))==S_OK);CHECK(d->EndScene()==S_OK);
    CHECK(d->GetRenderTargetData(target,read)==S_OK);D3DLOCKED_RECT lock{};CHECK(read->LockRect(&lock,nullptr,D3DLOCK_READONLY)==S_OK);
    const DWORD actual=*reinterpret_cast<DWORD*>(static_cast<char*>(lock.pBits)+8*lock.Pitch+8*4);
    for(unsigned shift:{0u,8u,16u,24u})CHECK(abs(int((actual>>shift)&255)-int((expected>>shift)&255))<=1);
    CHECK(read->UnlockRect()==S_OK);
  };
  d->SetTexture(0,textures[0]);
  d->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,0);pixel(0xff808080);
  ID3D9BridgeDiagnostics* diagnostics=nullptr;
  if(journal&&verifyPropagation){CHECK(d->QueryInterface(IID_D3D9BridgeDiagnostics,reinterpret_cast<void**>(&diagnostics))==S_OK);if(diagnostics){CHECK(diagnostics->SetCaptureOwner(TRUE)==S_OK);CHECK(diagnostics->StartAudit(0xdecaf,60000)==S_OK);}}
  for(auto srgb:{0u,1u,3u,2u}){CHECK(d->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,srgb)==S_OK);pixel(srgb&1?0xff373737:0xff808080);}
  for(auto state:{D3DSAMP_BORDERCOLOR,D3DSAMP_ELEMENTINDEX,D3DSAMP_DMAPOFFSET}){
    const DWORD value=state==D3DSAMP_BORDERCOLOR?0xff00ff00:7;
    CHECK(d->SetSamplerState(0,state,value)==S_OK);DWORD result=0;CHECK(d->GetSamplerState(0,state,&result)==S_OK&&result==value);pixel(0xff808080);
  }
  if(diagnostics){
    D3D9BridgeAuditSnapshot snapshot{};snapshot.StructSize=sizeof(snapshot);snapshot.Version=1;
    CHECK(diagnostics->GetAudit(&snapshot)==S_OK);CHECK(snapshot.PrepareDraws==7);
    CHECK(snapshot.Prepare[7].Units==0); // No actual sampler input changed.
    CHECK(snapshot.Prepare[8].Units==2); // Only bit zero selects an sRGB view.
    CHECK(diagnostics->StopAudit(0xdecaf)==S_OK);CHECK(diagnostics->SetCaptureOwner(FALSE)==S_OK);diagnostics->Release();
  }
  d->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_BORDER);d->SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_BORDER);pixel(0xff00ff00);
  // With border already active, an sRGB-only view change must also dirty the
  // sampler. Saturated pixels alone would not distinguish its view dependency.
  if(journal&&verifyPropagation){
    CHECK(d->QueryInterface(IID_D3D9BridgeDiagnostics,reinterpret_cast<void**>(&diagnostics))==S_OK);
    if(diagnostics){
      CHECK(diagnostics->SetCaptureOwner(TRUE)==S_OK);CHECK(diagnostics->StartAudit(0xdecaf,60000)==S_OK);
      CHECK(d->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,1)==S_OK);pixel(0xff00ff00);
      D3D9BridgeAuditSnapshot snapshot{};snapshot.StructSize=sizeof(snapshot);snapshot.Version=1;
      CHECK(diagnostics->GetAudit(&snapshot)==S_OK);CHECK(snapshot.Prepare[7].Units==1);CHECK(snapshot.Prepare[8].Units==1);
      CHECK(diagnostics->StopAudit(0xdecaf)==S_OK);CHECK(diagnostics->SetCaptureOwner(FALSE)==S_OK);diagnostics->Release();
    }
  }
  CHECK(d->BeginStateBlock()==S_OK);d->SetSamplerState(0,D3DSAMP_BORDERCOLOR,0xff0000ff);d->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,1);
  IDirect3DStateBlock9* block=nullptr;CHECK(d->EndStateBlock(&block)==S_OK);CHECK(block->Apply()==S_OK);pixel(0xff0000ff);
  if(journal){CHECK(journal->BeginStateJournal()==S_OK);d->SetTexture(0,textures[1]);d->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_WRAP);d->SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_WRAP);d->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,0);pixel(0xff202020);CHECK(journal->RestoreStateJournal()==S_OK);pixel(0xff0000ff);}
  block->Release();d->SetTexture(0,nullptr);textures[0]->Release();textures[1]->Release();read->Release();target->Release();
  // A device binding can be the last private reference to the old resource.
  for(unsigned cycle=0;cycle<8;cycle++){
    IDirect3DTexture9* old=nullptr,*next=nullptr;
    CHECK(d->CreateTexture(1,1,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&old,nullptr)==S_OK);
    CHECK(d->CreateTexture(1,1,1,0,D3DFMT_X8R8G8B8,D3DPOOL_MANAGED,&next,nullptr)==S_OK);
    const auto before=privateDestructions;auto* marker=new Lifetime;CHECK(old->SetPrivateData(LifetimeIID,marker,sizeof(IUnknown*),D3DSPD_IUNKNOWN)==S_OK);marker->Release();
    CHECK(d->SetTexture(0,old)==S_OK);old->Release();CHECK(privateDestructions==before);
    CHECK(d->SetTexture(0,next)==S_OK);CHECK(privateDestructions==before+1);d->SetTexture(0,nullptr);next->Release();
  }
  d->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_WRAP);d->SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_WRAP);d->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,0);
}
static void PrivateLifetime(IDirect3DDevice9* d,ID3D9GtaSaCompatDevice3* journal,D3DPRESENT_PARAMETERS pp){
  if(!journal)return;
  for(unsigned cycle=0;cycle<3;cycle++){
    IDirect3DTexture9* first=nullptr,*last=nullptr;IDirect3DVertexBuffer9* vb=nullptr;IDirect3DIndexBuffer9* ib=nullptr;
    CHECK(d->CreateTexture(1,1,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&first,nullptr)==S_OK);
    CHECK(d->CreateTexture(1,1,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&last,nullptr)==S_OK);
    CHECK(d->CreateVertexBuffer(64,0,0,D3DPOOL_MANAGED,&vb,nullptr)==S_OK);
    CHECK(d->CreateIndexBuffer(16,0,D3DFMT_INDEX16,D3DPOOL_MANAGED,&ib,nullptr)==S_OK);
    const auto destroyedBefore=privateDestructions;
    for(IDirect3DResource9* r:{static_cast<IDirect3DResource9*>(first),static_cast<IDirect3DResource9*>(last),static_cast<IDirect3DResource9*>(vb),static_cast<IDirect3DResource9*>(ib)}){
      auto* marker=new Lifetime;CHECK(r->SetPrivateData(LifetimeIID,marker,sizeof(IUnknown*),D3DSPD_IUNKNOWN)==S_OK);marker->Release();
    }
    d->SetTexture(0,first);d->SetTexture(15,last);d->SetStreamSource(15,vb,8,16);d->SetIndices(ib);
    CHECK(journal->BeginStateJournal()==S_OK);
    d->SetTexture(15,nullptr);
    if(cycle!=1){d->SetTexture(0,nullptr);d->SetStreamSource(15,nullptr,0,0);d->SetIndices(nullptr);}
    if(cycle==2){CHECK(d->Reset(&pp)==S_OK);CHECK(journal->RestoreStateJournal()==D3DERR_INVALIDCALL);}
    else{
      CHECK(journal->RestoreStateJournal()==S_OK);
      IDirect3DBaseTexture9* restored=nullptr;CHECK(d->GetTexture(15,&restored)==S_OK&&restored==last);if(restored)restored->Release();
    }
    d->SetTexture(0,nullptr);d->SetTexture(15,nullptr);d->SetStreamSource(15,nullptr,0,0);d->SetIndices(nullptr);
    first->Release();last->Release();vb->Release();ib->Release();Flush(d);
    CHECK(privateDestructions==destroyedBefore+4);
    Work(d,journal,"empty",2);
  }
}
static void Behavior(IDirect3DDevice9* d,ID3D9GtaSaCompatDevice3* journal){
  d->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);d->SetSamplerState(15,D3DSAMP_ADDRESSU,D3DTADDRESS_WRAP);
  // Recording identical values must still populate a normal custom state block.
  CHECK(d->BeginStateBlock()==S_OK);CHECK(d->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE)==S_OK);
  IDirect3DStateBlock9* b=nullptr;CHECK(d->EndStateBlock(&b)==S_OK);
  d->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);CHECK(b&&b->Apply()==S_OK);DWORD value=99;
  CHECK(d->GetRenderState(D3DRS_ALPHABLENDENABLE,&value)==S_OK&&value==FALSE);if(b)b->Release();
  // The same word/range restoration runs for ordinary D3D9 StateBlocks,
  // including when the GTA compatibility interface is disabled.
  CHECK(d->BeginStateBlock()==S_OK);
  const unsigned blockSlots[]={0,1,31,32,63,64,127,128,254,255};
  for(auto slot:blockSlots){const float saved[4]={float(slot+100),2,3,4};CHECK(d->SetVertexShaderConstantF(slot,saved,1)==S_OK);}
  CHECK(d->EndStateBlock(&b)==S_OK);
  for(auto slot:blockSlots){const float changed[4]={9,8,7,6};CHECK(d->SetVertexShaderConstantF(slot,changed,1)==S_OK);}
  const float untouched[4]={700,701,702,703};d->SetVertexShaderConstantF(200,untouched,1);
  CHECK(b->Apply()==S_OK);
  for(auto slot:blockSlots){float restored[4];const float saved[4]={float(slot+100),2,3,4};CHECK(d->GetVertexShaderConstantF(slot,restored,1)==S_OK&&!memcmp(restored,saved,sizeof(saved)));}
  float unaffected[4];CHECK(d->GetVertexShaderConstantF(200,unaffected,1)==S_OK&&!memcmp(unaffected,untouched,sizeof(untouched)));b->Release();
  IDirect3DTexture9* a=nullptr;CHECK(d->CreateTexture(1,1,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&a,nullptr)==S_OK);
  auto refs=a->AddRef();a->Release();
  if(journal){
    CHECK(journal->RestoreStateJournal()==D3DERR_INVALIDCALL);
    const float initial[4]={1,2,3,4};d->SetVertexShaderConstantF(255,initial,1);
    for(unsigned i=0;i<100;i++){
      CHECK(journal->BeginStateJournal()==S_OK);CHECK(journal->BeginStateJournal()==D3DERR_INVALIDCALL);
      CHECK(d->SetTexture(i&15,a)==S_OK);d->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);
      float other[4]={9,8,7,6};d->SetVertexShaderConstantF(255,other,1);
      CHECK(journal->RestoreStateJournal()==S_OK);
      CHECK(d->GetRenderState(D3DRS_ALPHABLENDENABLE,&value)==S_OK&&value==FALSE);
      float restored[4];CHECK(d->GetVertexShaderConstantF(255,restored,1)==S_OK&&!memcmp(restored,initial,sizeof(initial)));
      IDirect3DBaseTexture9* texture=nullptr;CHECK(d->GetTexture(i&15,&texture)==S_OK&&texture==nullptr);if(texture)texture->Release();
    }
    CHECK(a->AddRef()==refs);a->Release();
    IDirect3DVertexBuffer9* vertexBuffer=nullptr;IDirect3DIndexBuffer9* indexBuffer=nullptr;
    CHECK(d->CreateVertexBuffer(64,0,0,D3DPOOL_MANAGED,&vertexBuffer,nullptr)==S_OK);
    CHECK(d->CreateIndexBuffer(16,0,D3DFMT_INDEX16,D3DPOOL_MANAGED,&indexBuffer,nullptr)==S_OK);
    const auto vbRefs=vertexBuffer->AddRef();vertexBuffer->Release();
    const auto ibRefs=indexBuffer->AddRef();indexBuffer->Release();
    for(unsigned slot=0;slot<16;slot++){
      CHECK(journal->BeginStateJournal()==S_OK);
      CHECK(d->SetStreamSource(slot,vertexBuffer,8,16)==S_OK);CHECK(d->SetIndices(indexBuffer)==S_OK);
      CHECK(journal->RestoreStateJournal()==S_OK);
      IDirect3DVertexBuffer9* restored=nullptr;UINT offset=99,stride=99;
      CHECK(d->GetStreamSource(slot,&restored,&offset,&stride)==S_OK&&restored==nullptr);
      if(restored)restored->Release();IDirect3DIndexBuffer9* restoredIndex=nullptr;
      CHECK(d->GetIndices(&restoredIndex)==S_OK&&restoredIndex==nullptr);if(restoredIndex)restoredIndex->Release();
    }
    CHECK(vertexBuffer->AddRef()==vbRefs);vertexBuffer->Release();vertexBuffer->Release();
    CHECK(indexBuffer->AddRef()==ibRefs);indexBuffer->Release();indexBuffer->Release();
    // Cross-word, holes, tail and consecutive ranges exercise restoration of
    // exactly the captured registers rather than the enclosing interval.
    const unsigned slots[]={0,1,30,31,32,33,63,64,127,128,254,255};
    float initialRanges[256*4];for(unsigned k=0;k<1024;k++)initialRanges[k]=float(k+2000);
    CHECK(d->SetVertexShaderConstantF(0,initialRanges,256)==S_OK);
    for(unsigned cycle=0;cycle<12;cycle++){
      CHECK(journal->BeginStateJournal()==S_OK);
      for(auto slot:slots){float changed[4]={float(slot+cycle),0,0,0};CHECK(d->SetVertexShaderConstantF(slot,changed,1)==S_OK);}
      CHECK(journal->RestoreStateJournal()==S_OK);float restored[256*4];
      CHECK(d->GetVertexShaderConstantF(0,restored,256)==S_OK&&!memcmp(restored,initialRanges,sizeof(restored)));
    }
    Work(d,journal,"dense",2);Work(d,journal,"sparse",2);Work(d,journal,"empty",2);
    CHECK(d->GetSamplerState(15,D3DSAMP_ADDRESSU,&value)==S_OK&&value==D3DTADDRESS_WRAP);
  }
  a->Release();d->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1);d->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_DIFFUSE);
  d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);d->SetRenderState(D3DRS_ZENABLE,FALSE);d->SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE);
  d->Clear(0,nullptr,D3DCLEAR_TARGET,0xff000000,1,0);Draw(d);
  IDirect3DSurface9* rt=nullptr,*read=nullptr;CHECK(d->GetRenderTarget(0,&rt)==S_OK);
  CHECK(d->CreateOffscreenPlainSurface(64,64,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&read,nullptr)==S_OK);
  CHECK(d->GetRenderTargetData(rt,read)==S_OK);D3DLOCKED_RECT lock{};CHECK(read->LockRect(&lock,nullptr,D3DLOCK_READONLY)==S_OK);
  CHECK((*reinterpret_cast<DWORD*>(static_cast<char*>(lock.pBits)+8*lock.Pitch+8*4)&0xffffff)==0x00ff00);
  read->UnlockRect();read->Release();rt->Release();
}
int main(int argc,char** argv){
  if(argc<2)return 2;const std::string mode=argc>2?argv[2]:"check";const bool software=mode=="check-software";const bool check=mode=="check"||mode=="check-optional-stats"||software;auto module=LoadLibraryA(argv[1]);CHECK(module!=nullptr);if(!module)return 2;
  auto create=reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(module,"Direct3DCreate9"));CHECK(create);if(!create)return 2;
  WNDCLASSA wc{};wc.lpfnWndProc=DefWindowProcA;wc.hInstance=GetModuleHandleA(nullptr);wc.lpszClassName="ThreadJournalProbe";RegisterClassA(&wc);
  auto window=CreateWindowA(wc.lpszClassName,"Independent D3D9 fixture",WS_OVERLAPPEDWINDOW,0,0,64,64,nullptr,nullptr,wc.hInstance,nullptr);
  auto api=create(D3D_SDK_VERSION);CHECK(api);if(!api)return 2;D3DPRESENT_PARAMETERS pp{};
  pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=window;pp.BackBufferWidth=pp.BackBufferHeight=64;pp.BackBufferFormat=D3DFMT_A8R8G8B8;pp.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
  IDirect3DDevice9* d=nullptr;CHECK(api->CreateDevice(0,D3DDEVTYPE_HAL,window,software?D3DCREATE_MIXED_VERTEXPROCESSING:D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&d)==S_OK);if(!d)return 2;
  if(software)CHECK(d->SetSoftwareVertexProcessing(TRUE)==S_OK);
  ThreadStats* stats=nullptr;auto statResult=d->QueryInterface(ThreadStatsIID,reinterpret_cast<void**>(&stats));
  if(mode=="check"){CHECK(statResult==S_OK&&stats);}
  ID3D9GtaSaCompatDevice3* journal=nullptr;d->QueryInterface(__uuidof(ID3D9GtaSaCompatDevice3),reinterpret_cast<void**>(&journal));
  std::printf("fixture executable=thread-journal-probe compat=%u privateStats=%u\n",unsigned(journal!=nullptr),unsigned(stats!=nullptr));
  if(check){
    Modules();OverlappingConstants(d,journal,software);BindingPixels(d,journal,mode!="check-optional-stats");Behavior(d,journal);PrivateLifetime(d,journal,pp);Flush(d);Sleep(20);
    if(stats){
      CHECK(stats->GetThreadStats(nullptr)==D3DERR_INVALIDCALL);ThreadSnapshot invalid{};invalid.StructSize=sizeof(invalid)-1;invalid.Version=1;
      CHECK(stats->GetThreadStats(&invalid)==D3DERR_INVALIDCALL);invalid.StructSize=sizeof(invalid);invalid.Version=999;CHECK(stats->GetThreadStats(&invalid)==D3DERR_INVALIDCALL);
      auto a=Snap(stats);Sleep(100);auto b=Snap(stats);Print("idle",a,b);
      CHECK(b.MainThreadId==GetCurrentThreadId()&&b.CsThreadId&&b.CsThreadId!=b.MainThreadId&&b.AvailableMask==7);
      CHECK(b.WallUs-a.WallUs>=80000&&b.CsQueueIdleUs-a.CsQueueIdleUs>=70000);
      auto end=Now()+100000;while(Now()<end){d->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);}auto c=Snap(stats);Print("main-busy",b,c);
      CHECK(c.MainCpu100ns>b.MainCpu100ns+100000); // >=10ms scheduled CPU
      d->SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE);Work(d,journal,"draw",150000); IDirect3DSurface9* rt=nullptr,*read=nullptr; d->GetRenderTarget(0,&rt); d->CreateOffscreenPlainSurface(64,64,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&read,nullptr); CHECK(d->GetRenderTargetData(rt,read)==S_OK); D3DLOCKED_RECT waited{}; CHECK(read->LockRect(&waited,nullptr,D3DLOCK_READONLY)==S_OK); read->UnlockRect(); read->Release(); rt->Release(); Flush(d);auto e=Snap(stats);Print("draw",c,e);
      CHECK(e.OrderedDispatched>c.OrderedDispatched&&e.OrderedCompleted>c.OrderedCompleted&&e.CsCpu100ns>c.CsCpu100ns);
      CHECK(e.OrderedCompleted<=e.OrderedDispatched);
      CHECK(e.CsSyncCount>c.CsSyncCount&&e.CsSyncUs>c.CsSyncUs);
      CHECK(e.GpuSyncCount>c.GpuSyncCount&&e.GpuSyncUs>c.GpuSyncUs);
      const auto before=Snap(stats);for(unsigned i=0;i<100;i++)Snap(stats);const auto after=Snap(stats);
      CHECK(after.OrderedDispatched==before.OrderedDispatched&&after.Submits==before.Submits);
      CHECK(d->Reset(&pp)==S_OK);auto reset=Snap(stats);CHECK(reset.MainThreadId==e.MainThreadId&&reset.WallUs>=e.WallUs);
    }
  }else{
    d->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);d->SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE);Flush(d);
    std::vector<std::string> cases={"control","stateblock","draw"};if(journal)cases.insert(cases.end(),{"empty","sparse","tail","dense","sparse-draw","tail-draw"});
    if(argc>3)std::rotate(cases.begin(),cases.begin()+(std::strtoul(argv[3],nullptr,10)%cases.size()),cases.end());
    const bool observe=stats&&mode!="bench-off";
    if(argc>4&&std::string(argv[4])!="all"){
      const std::string selected=","+std::string(argv[4])+",";
      if(selected.find(",stateblock-tail,")!=std::string::npos)cases.push_back("stateblock-tail");
      for(auto extra:{"capture-repeat","capture-overlap"})if(selected.find(","+std::string(extra)+",")!=std::string::npos)cases.push_back(extra);
      cases.erase(std::remove_if(cases.begin(),cases.end(),[&](const auto& c){return selected.find(","+c+",")==std::string::npos;}),cases.end());
    }
    DWORD_PTR processMask=0,systemMask=0;GetProcessAffinityMask(GetCurrentProcess(),&processMask,&systemMask);
    const DWORD_PTR affinity=(processMask&4)?4:(processMask&(~processMask+1));
    CHECK(affinity&&SetThreadAffinityMask(GetCurrentThread(),affinity));std::printf("fixtureMainAffinityMask=%llu\n",UINT64(affinity));
    for(const auto& test:cases){Work(d,journal,test,200);Flush(d);
      for(unsigned round=0;round<5;round++){
        const auto start=Now();const auto a=observe?Snap(stats):ThreadSnapshot{};
        const unsigned n=test=="control"?4000000:test.find("draw")!=std::string::npos?50000:200000;
        Work(d,journal,test,n);const auto b=observe?Snap(stats):ThreadSnapshot{};const auto stop=Now();
        Flush(d);const auto completed=Now();
        std::printf("bench case=%s round=%u count=%u us=%llu observer=%u\n",test.c_str(),round,n,stop-start,unsigned(observe));
        std::printf("complete case=%s round=%u us=%llu\n",test.c_str(),round,completed-start);
        if(observe)Print(test.c_str(),a,b);
      }
    }
  }
  if(stats)stats->Release();if(journal)journal->Release();CHECK(d->Release()==0);api->Release();DestroyWindow(window);FreeLibrary(module);
  std::printf("checks=%u failures=%u\n",checks,failures);return failures?1:0;
}
