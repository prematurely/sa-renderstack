#include <windows.h>
#include <d3d9.h>
#include <sa_renderstack/backend_api.h>
#include <array>
#include <cmath>
#include <iostream>
#include <string>
#include <utility>

struct Module { HMODULE h{}; ~Module(){if(h)FreeLibrary(h);} };
using Create9 = IDirect3D9* (WINAPI*)(UINT);
template<class T> class com_ptr { T* p_{}; public: com_ptr()=default; explicit com_ptr(T* p):p_(p){} ~com_ptr(){if(p_)p_->Release();} com_ptr(const com_ptr&)=delete; com_ptr& operator=(const com_ptr&)=delete; com_ptr(com_ptr&&o)noexcept:p_(std::exchange(o.p_,nullptr)){} T** out(){return &p_;} T* get()const{return p_;} T* operator->()const{return p_;} };
struct Window { HWND h{}; Window(){ WNDCLASSW wc{.style=0,.lpfnWndProc=DefWindowProcW,.hInstance=GetModuleHandleW(nullptr),.lpszClassName=L"Api5Probe"}; RegisterClassW(&wc); h=CreateWindowW(wc.lpszClassName,L"api5",0,0,0,64,64,nullptr,nullptr,wc.hInstance,nullptr);} ~Window(){if(h)DestroyWindow(h);} };
bool check(bool v,const char* m){std::cout<<(v?"PASS ":"FAIL ")<<m<<'\n';return v;}
int wmain(int argc,wchar_t** argv){
  std::wstring path=argc>1?argv[1]:L"backend\\dxvk-gta\\d3d9.dll"; HMODULE mod=LoadLibraryW(path.c_str()); Module module{mod}; if(!check(mod,"LoadLibrary backend")) return 2;
  auto create=reinterpret_cast<Create9>(GetProcAddress(mod,"Direct3DCreate9")); if(!check(create!=nullptr,"Direct3DCreate9 export")) return 2;
  Window win; com_ptr<IDirect3D9> d3d(create(D3D_SDK_VERSION)); if(!check(d3d.get()!=nullptr,"Direct3DCreate9")) return 2;
  D3DPRESENT_PARAMETERS pp{}; pp.Windowed=TRUE; pp.SwapEffect=D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow=win.h; pp.BackBufferFormat=D3DFMT_X8R8G8B8; pp.BackBufferWidth=64; pp.BackBufferHeight=64;
  com_ptr<IDirect3DDevice9> dev; HRESULT hr=d3d->CreateDevice(D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,win.h,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,dev.out()); if(!check(SUCCEEDED(hr),"CreateDevice")) return 2;
  com_ptr<ID3D9GtaSaCompatDevice4> api5; hr=dev->QueryInterface(__uuidof(ID3D9GtaSaCompatDevice4),reinterpret_cast<void**>(api5.out())); if(!check(SUCCEEDED(hr),"QI API5")) return 2;
  D3D9GtaSaRenderStateBinding rs{D3DRS_CULLMODE,D3DCULL_CW}; D3DMATRIX identity{}; identity._11=identity._22=identity._33=identity._44=1.0f; D3D9GtaSaTransformBinding tr{D3DTS_WORLD,identity}; float vf[4]={1,2,3,4}; D3D9GtaSaFloatConstantRange vr{0,1,vf};
  D3D9GtaSaLightBinding light{}; light.Index=0; light.Light.Type=D3DLIGHT_DIRECTIONAL; light.Light.Direction={0,0,1}; light.Light.Diffuse={1,1,1,1}; D3D9GtaSaLightEnableBinding enable{0,TRUE}; D3D9GtaSaTextureBinding texture{0,nullptr}; D3D9GtaSaTextureStageStateBinding stage{0,D3DTSS_COLOROP,D3DTOP_SELECTARG1}; D3D9GtaSaSamplerStateBinding sampler{0,D3DSAMP_MINFILTER,D3DTEXF_POINT}; int vi[4]={1,2,3,4}; BOOL vb[1]={TRUE}; D3D9GtaSaIntConstantRange ir{0,1,vi}; D3D9GtaSaBoolConstantRange br{0,1,vb};
  D3D9GtaSaEffectStateBatch b{}; b.StructSize=sizeof(b); b.ApiVersion=5; b.Flags=D3D9_GTA_SA_EFFECT_STATE_HAS_MATERIAL|D3D9_GTA_SA_EFFECT_STATE_HAS_NPATCH_MODE|D3D9_GTA_SA_EFFECT_STATE_HAS_FVF|D3D9_GTA_SA_EFFECT_STATE_HAS_VERTEX_SHADER|D3D9_GTA_SA_EFFECT_STATE_HAS_PIXEL_SHADER; b.TransformCount=1; b.Transforms=&tr; b.LightCount=1; b.Lights=&light; b.LightEnableCount=1; b.LightEnables=&enable; b.RenderStateCount=1; b.RenderStates=&rs; b.TextureBindingCount=1; b.TextureBindings=&texture; b.TextureStageStateCount=1; b.TextureStageStates=&stage; b.SamplerStateCount=1; b.SamplerStates=&sampler; b.Material.Diffuse={1,1,1,1}; b.NPatchMode=0; b.FVF=D3DFVF_XYZ; b.VertexFloatRangeCount=1; b.VertexFloatRanges=&vr; b.PixelFloatRangeCount=1; b.PixelFloatRanges=&vr; b.VertexIntRangeCount=1; b.VertexIntRanges=&ir; b.PixelIntRangeCount=1; b.PixelIntRanges=&ir; b.VertexBoolRangeCount=1; b.VertexBoolRanges=&br; b.PixelBoolRangeCount=1; b.PixelBoolRanges=&br;
  bool ok=true; dev->SetRenderState(D3DRS_CULLMODE,D3DCULL_CCW); ok&=check(api5->BeginStateJournal()==D3D_OK,"Begin journal"); DWORD state=0; dev->GetRenderState(D3DRS_CULLMODE,&state); ok&=check(state==D3DCULL_CCW,"baseline state");
  ok&=check(api5->SubmitEffectStateBatch(&b)==D3D_OK,"valid effect batch"); dev->GetRenderState(D3DRS_CULLMODE,&state); ok&=check(state==D3DCULL_CW,"batch applied");
  ok&=check(api5->RestoreStateJournal()==D3D_OK,"restore journal"); dev->GetRenderState(D3DRS_CULLMODE,&state); ok&=check(state==D3DCULL_CCW,"state restored");
  D3D9GtaSaRenderStateBinding bad[2]={{D3DRS_CULLMODE,D3DCULL_CW},{static_cast<D3DRENDERSTATETYPE>(1),0}}; b.RenderStateCount=2; b.RenderStates=bad; hr=api5->SubmitEffectStateBatch(&b); ok&=check(hr==D3DERR_INVALIDCALL,"invalid batch rejected"); dev->GetRenderState(D3DRS_CULLMODE,&state); ok&=check(state==D3DCULL_CCW,"invalid batch has no partial state");
   return ok?0:1;
}

