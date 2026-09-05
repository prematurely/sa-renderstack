#include <windows.h>
#include <d3d9.h>
#include <sa_renderstack/backend_api.h>
#include <array>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>
struct Module { HMODULE h{}; ~Module(){if(h)FreeLibrary(h);} };
using Create9=IDirect3D9*(WINAPI*)(UINT);
template<class T> class com_ptr{T*p_{};public:~com_ptr(){if(p_)p_->Release();} com_ptr()=default; explicit com_ptr(T*p):p_(p){} com_ptr(const com_ptr&)=delete; T**out(){return&p_;} T*get()const{return p_;} T*operator->()const{return p_;}};
struct Win{HWND h{};Win(){WNDCLASSW c{.lpfnWndProc=DefWindowProcW,.hInstance=GetModuleHandleW(nullptr),.lpszClassName=L"Api6Probe"};RegisterClassW(&c);h=CreateWindowW(c.lpszClassName,L"api6",0,0,0,64,64,nullptr,nullptr,c.hInstance,nullptr);}~Win(){if(h)DestroyWindow(h);}};
bool ck(bool v,const char*m){std::cout<<(v?"PASS ":"FAIL ")<<m<<'\n';return v;}
int wmain(int argc,wchar_t**argv){std::wstring path=argc>1?argv[1]:L"backend\\dxvk-gta\\d3d9.dll";HMODULE mod=LoadLibraryW(path.c_str()); Module module{mod};if(!ck(mod,"LoadLibrary backend"))return 2;auto f=(Create9)GetProcAddress(mod,"Direct3DCreate9");if(!ck(f,"Direct3DCreate9 export"))return 2;Win w;com_ptr<IDirect3D9>d(f(D3D_SDK_VERSION));if(!ck(d.get()!=nullptr,"Direct3DCreate9"))return 2;D3DPRESENT_PARAMETERS pp{};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=w.h;pp.BackBufferFormat=D3DFMT_X8R8G8B8;pp.BackBufferWidth=64;pp.BackBufferHeight=64;com_ptr<IDirect3DDevice9>dev;HRESULT hr=d->CreateDevice(0,D3DDEVTYPE_HAL,w.h,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,dev.out());if(!ck(SUCCEEDED(hr),"CreateDevice"))return 2;com_ptr<ID3D9GtaSaCompatDevice5>api;hr=dev->QueryInterface(__uuidof(ID3D9GtaSaCompatDevice5),reinterpret_cast<void**>(api.out()));if(!ck(SUCCEEDED(hr),"QI API6"))return 2;
  std::array<float,12> verts={0,0,0,1,0,0,0,1,0,0,0,1};com_ptr<IDirect3DVertexBuffer9>vb;hr=dev->CreateVertexBuffer(sizeof(verts),0,D3DFVF_XYZ,D3DPOOL_DEFAULT,vb.out(),nullptr);if(!ck(SUCCEEDED(hr),"CreateVertexBuffer"))return 2;void*p=nullptr;vb->Lock(0,0,&p,0);memcpy(p,verts.data(),sizeof(verts));vb->Unlock();com_ptr<IDirect3DIndexBuffer9>ib;std::array<WORD,3>idx={0,1,2};hr=dev->CreateIndexBuffer(sizeof(idx),0,D3DFMT_INDEX16,D3DPOOL_DEFAULT,ib.out(),nullptr);if(!ck(SUCCEEDED(hr),"CreateIndexBuffer"))return 2;ib->Lock(0,0,&p,0);memcpy(p,idx.data(),sizeof(idx));ib->Unlock();dev->SetStreamSource(0,vb.get(),0,3*sizeof(float));dev->SetIndices(ib.get());dev->SetFVF(D3DFVF_XYZ);
  float value[4]={2,0,0,0};D3D9GtaSaFloatConstantRange r{0,1,value};D3D9GtaSaStateBatch sb{};sb.StructSize=sizeof(sb);sb.ApiVersion=3;sb.VertexFloatRangeCount=1;sb.VertexFloatRanges=&r;D3D9GtaSaStateDrawBatch b{};b.StructSize=sizeof(b);b.ApiVersion=6;b.StateBatch=&sb;b.Draw={sizeof(D3D9GtaSaDrawDesc),D3D9_GTA_SA_DRAW_PRIMITIVE,D3DPT_TRIANGLELIST,0,0,0,0,0,1};
  bool ok=true;float before[4]={1,0,0,0};dev->SetVertexShaderConstantF(0,before,1);dev->BeginScene();hr=api->SubmitStateDrawBatch(&b);dev->EndScene();ok&=ck(SUCCEEDED(hr),"valid state+DrawPrimitive");float got[4]{};dev->GetVertexShaderConstantF(0,got,1);ok&=ck(got[0]==2.0f,"state batch applied");b.Draw={sizeof(D3D9GtaSaDrawDesc),D3D9_GTA_SA_DRAW_INDEXED_PRIMITIVE,D3DPT_TRIANGLELIST,0,0,0,3,0,1};dev->BeginScene();hr=api->SubmitStateDrawBatch(&b);dev->EndScene();ok&=ck(SUCCEEDED(hr),"valid state+DrawIndexedPrimitive");
  dev->SetVertexShaderConstantF(0,before,1);D3D9GtaSaStateDrawBatch bad=b;bad.Draw.Kind=99;hr=api->SubmitStateDrawBatch(&bad);ok&=ck(hr==D3DERR_INVALIDCALL,"invalid draw rejected");dev->GetVertexShaderConstantF(0,got,1);ok&=ck(got[0]==1.0f,"invalid draw has no partial state");return ok?0:1;}

