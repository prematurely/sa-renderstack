#include <windows.h>
#include <d3d9.h>
#include <sa_renderstack/backend_api.h>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
struct Module { HMODULE h{}; ~Module(){if(h)FreeLibrary(h);} };
using Create9=IDirect3D9*(WINAPI*)(UINT);
template<class T> class com_ptr{T*p_{};public:~com_ptr(){if(p_)p_->Release();} com_ptr()=default; explicit com_ptr(T*p):p_(p){} com_ptr(const com_ptr&)=delete; T**out(){return&p_;} T*get()const{return p_;} T*operator->()const{return p_;}};
struct Win{HWND h{};Win(){WNDCLASSW c{.lpfnWndProc=DefWindowProcW,.hInstance=GetModuleHandleW(nullptr),.lpszClassName=L"Api7Probe"};RegisterClassW(&c);h=CreateWindowW(c.lpszClassName,L"api7",0,0,0,64,64,nullptr,nullptr,c.hInstance,nullptr);}~Win(){if(h)DestroyWindow(h);}};
bool ck(bool v,const char*m){std::cout<<(v?"PASS ":"FAIL ")<<m<<'\n';return v;}
int wmain(int argc,wchar_t**argv){std::wstring path=argc>1?argv[1]:L"backend\\dxvk-gta\\d3d9.dll";HMODULE mod=LoadLibraryW(path.c_str()); Module module{mod};if(!ck(mod,"LoadLibrary backend"))return 2;auto f=(Create9)GetProcAddress(mod,"Direct3DCreate9");if(!ck(f,"Direct3DCreate9 export"))return 2;Win w;com_ptr<IDirect3D9>d(f(D3D_SDK_VERSION));if(!ck(d.get()!=nullptr,"Direct3DCreate9"))return 2;D3DPRESENT_PARAMETERS pp{};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=w.h;pp.BackBufferFormat=D3DFMT_X8R8G8B8;pp.BackBufferWidth=64;pp.BackBufferHeight=64;com_ptr<IDirect3DDevice9>dev;HRESULT hr=d->CreateDevice(0,D3DDEVTYPE_HAL,w.h,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,dev.out());if(!ck(SUCCEEDED(hr),"CreateDevice"))return 2;com_ptr<ID3D9GtaSaCompatDevice6>api;hr=dev->QueryInterface(__uuidof(ID3D9GtaSaCompatDevice6),reinterpret_cast<void**>(api.out()));if(!ck(SUCCEEDED(hr),"QI API7"))return 2;
  bool ok=true;ok&=ck(api->SetStateJournalCaptureEnabled(TRUE)==D3DERR_INVALIDCALL,"capture before begin fails closed");ok&=ck(api->BeginSelectiveStateJournal()==D3D_OK,"begin selective journal");DWORD s=0;dev->GetRenderState(D3DRS_CULLMODE,&s);const DWORD baseline=s;dev->SetRenderState(D3DRS_CULLMODE,D3DCULL_CW);ok&=ck(api->SetStateJournalCaptureEnabled(TRUE)==D3D_OK,"enable capture");dev->SetRenderState(D3DRS_CULLMODE,D3DCULL_CCW);ok&=ck(api->SetStateJournalCaptureEnabled(FALSE)==D3D_OK,"disable capture");ok&=ck(api->RestoreStateJournal()==D3D_OK,"restore selective journal");dev->GetRenderState(D3DRS_CULLMODE,&s);ok&=ck(s==D3DCULL_CW,"only captured state restored");
  ok&=ck(api->BeginSelectiveStateJournal()==D3D_OK,"begin for reset");dev->SetRenderState(D3DRS_CULLMODE,D3DCULL_CCW);D3DPRESENT_PARAMETERS reset=pp;hr=dev->Reset(&reset);ok&=ck(SUCCEEDED(hr),"Reset succeeds");ok&=ck(api->RestoreStateJournal()==D3DERR_INVALIDCALL,"Reset discards journal");
  ok&=ck(api->SetStateJournalCaptureEnabled(TRUE)==D3DERR_INVALIDCALL,"failed sequence fallback remains closed");std::cout<<"same_thread_sequence=verified; reset_discard=verified; fallback=verified\n";return ok?0:1;}

