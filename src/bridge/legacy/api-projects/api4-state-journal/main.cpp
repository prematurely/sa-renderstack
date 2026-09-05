#include <windows.h>
#include <d3d9.h>
#include <sa_renderstack/backend_api.h>
#include <cstdio>

using Direct3DCreate9Fn = IDirect3D9* (WINAPI*)(UINT);
LRESULT CALLBACK Api4WindowProc(HWND w, UINT m, WPARAM p, LPARAM l) { return DefWindowProcW(w,m,p,l); }
static void show(const char* label, HRESULT hr, bool success) {
  std::printf("%-36s hr=0x%08lX %s\n", label, static_cast<unsigned long>(hr),
    (success ? (SUCCEEDED(hr)?"PASS":"FAIL") : (FAILED(hr)?"PASS":"FAIL")));
}

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) { std::puts("SKIP: pass path to backend d3d9.dll"); return 0; }
  HMODULE dll = LoadLibraryW(argv[1]);
  if (!dll) { std::printf("SKIP: LoadLibrary failed (%lu)\n", GetLastError()); return 0; }
  auto create9 = reinterpret_cast<Direct3DCreate9Fn>(GetProcAddress(dll,"Direct3DCreate9"));
  if (!create9) { std::puts("SKIP: Direct3DCreate9 export missing"); FreeLibrary(dll); return 0; }
  WNDCLASSW wc{}; wc.lpfnWndProc=Api4WindowProc; wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"RenderStackApi4Probe"; RegisterClassW(&wc);
  HWND window=CreateWindowW(wc.lpszClassName,L"api4",WS_OVERLAPPEDWINDOW,0,0,64,64,nullptr,nullptr,wc.hInstance,nullptr);
  if (!window) { std::puts("SKIP: window creation failed"); FreeLibrary(dll); return 0; }
  IDirect3D9* d3d=create9(D3D_SDK_VERSION);
  if (!d3d) { std::puts("SKIP: Direct3DCreate9 returned null"); DestroyWindow(window); FreeLibrary(dll); return 0; }
  D3DPRESENT_PARAMETERS pp{}; pp.Windowed=TRUE; pp.SwapEffect=D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow=window;
  IDirect3DDevice9* device=nullptr; HRESULT hr=d3d->CreateDevice(0,D3DDEVTYPE_HAL,window,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,&device);
  if (FAILED(hr)||!device) { std::printf("SKIP: CreateDevice hr=0x%08lX\n",static_cast<unsigned long>(hr)); d3d->Release(); DestroyWindow(window); FreeLibrary(dll); return 0; }
  ID3D9GtaSaCompatDevice3* api4=nullptr; hr=device->QueryInterface(__uuidof(ID3D9GtaSaCompatDevice3),reinterpret_cast<void**>(&api4)); show("QueryInterface API4",hr,true);
  if (FAILED(hr)) { device->Release(); d3d->Release(); DestroyWindow(window); FreeLibrary(dll); return 1; }

  hr=api4->RestoreStateJournal(); show("restore without begin",hr,false);
  DWORD baseline=0; hr=device->GetRenderState(D3DRS_ZENABLE,&baseline); show("read baseline ZENABLE",hr,true);
  hr=api4->BeginStateJournal(); show("begin journal",hr,true);
  HRESULT nested=api4->BeginStateJournal(); show("nested begin rejected",nested,false);
  DWORD changed = baseline == D3DZB_FALSE ? D3DZB_TRUE : D3DZB_FALSE;
  hr=device->SetRenderState(D3DRS_ZENABLE,changed); show("mutate ZENABLE",hr,true);
  DWORD observed=0; device->GetRenderState(D3DRS_ZENABLE,&observed);
  std::printf("%-36s %s\n", "mutation observed", observed==changed?"PASS":"FAIL");
  hr=api4->RestoreStateJournal(); show("restore journal",hr,true);
  device->GetRenderState(D3DRS_ZENABLE,&observed);
  bool restored=(observed==baseline); std::printf("%-36s %s\n", "state restored exactly",restored?"PASS":"FAIL");
  hr=api4->RestoreStateJournal(); show("double restore rejected",hr,false);

  hr=api4->BeginStateJournal(); show("begin before reset",hr,true);
  hr=device->Reset(&pp); show("device reset",hr,true);
  HRESULT afterReset=api4->RestoreStateJournal(); show("restore after reset rejected",afterReset,false);
  bool ok=restored && SUCCEEDED(hr) && FAILED(afterReset);
  api4->Release(); device->Release(); d3d->Release(); DestroyWindow(window); UnregisterClassW(wc.lpszClassName,wc.hInstance); FreeLibrary(dll);
  return ok?0:1;
}
