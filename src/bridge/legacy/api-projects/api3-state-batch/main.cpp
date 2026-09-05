#include <windows.h>
#include <d3d9.h>
#include <sa_renderstack/backend_api.h>
#include "api3_state_batch.hpp"

#include <cstdio>
#include <cstring>
#include <string>

using Direct3DCreate9Fn = IDirect3D9* (WINAPI*)(UINT);

namespace {
LRESULT CALLBACK WindowProc(HWND w, UINT m, WPARAM p, LPARAM l) { return DefWindowProcW(w,m,p,l); }

struct ComRelease { template<class T> void operator()(T* p) const { if (p) p->Release(); } };

void report(const char* name, HRESULT hr, bool expected = true) {
  std::printf("%-36s hr=0x%08lX %s\n", name, static_cast<unsigned long>(hr),
    (expected ? (SUCCEEDED(hr) ? "PASS" : "FAIL") : (FAILED(hr) ? "PASS" : "FAIL")));
}
}

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) { std::puts("SKIP: pass path to backend d3d9.dll"); return 0; }
  HMODULE module = LoadLibraryW(argv[1]);
  if (!module) { std::printf("SKIP: LoadLibrary failed (%lu)\n", GetLastError()); return 0; }
  auto create9 = reinterpret_cast<Direct3DCreate9Fn>(GetProcAddress(module, "Direct3DCreate9"));
  if (!create9) { std::puts("SKIP: Direct3DCreate9 export missing"); FreeLibrary(module); return 0; }

  WNDCLASSW wc{}; wc.lpfnWndProc=WindowProc; wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"RenderStackApi3Probe";
  RegisterClassW(&wc);
  HWND window = CreateWindowW(wc.lpszClassName, L"api3", WS_OVERLAPPEDWINDOW, 0,0,64,64,nullptr,nullptr,wc.hInstance,nullptr);
  if (!window) { std::puts("SKIP: window creation failed"); FreeLibrary(module); return 0; }

  IDirect3D9* d3d = create9(D3D_SDK_VERSION);
  if (!d3d) { std::puts("SKIP: Direct3DCreate9 returned null"); DestroyWindow(window); FreeLibrary(module); return 0; }
  D3DPRESENT_PARAMETERS pp{}; pp.Windowed=TRUE; pp.SwapEffect=D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow=window;
  IDirect3DDevice9* device = nullptr;
  HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
    D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device);
  if (FAILED(hr) || !device) { std::printf("SKIP: CreateDevice hr=0x%08lX\n", static_cast<unsigned long>(hr)); d3d->Release(); DestroyWindow(window); FreeLibrary(module); return 0; }

  ID3D9GtaSaCompatDevice2* api3 = nullptr;
  hr = device->QueryInterface(__uuidof(ID3D9GtaSaCompatDevice2), reinterpret_cast<void**>(&api3));
  report("QueryInterface API3", hr);
  if (FAILED(hr)) { device->Release(); d3d->Release(); DestroyWindow(window); FreeLibrary(module); return 1; }

  float baseline[4] = { 1.f, 2.f, 3.f, 4.f };
  hr = device->SetVertexShaderConstantF(0, baseline, 1); report("baseline SetVertexShaderConstantF", hr);
  const float legalData[8] = { 5.f, 6.f, 7.f, 8.f, 9.f, 10.f, 11.f, 12.f };
  D3D9GtaSaFloatConstantRange legalRange{0, 2, legalData};
  const float pixelData[4] = { 13.f, 14.f, 15.f, 16.f };
  D3D9GtaSaFloatConstantRange pixelRange{2, 1, pixelData};
  D3D9GtaSaStateBatch legal{}; legal.StructSize=sizeof(legal); legal.ApiVersion=3; legal.VertexFloatRangeCount=1; legal.VertexFloatRanges=&legalRange; legal.PixelFloatRangeCount=1; legal.PixelFloatRanges=&pixelRange;
  api3::StateBatchBuilder builder;
  const auto vertexAdded = builder.add_vertex_float(0, legalData);
  const auto pixelAdded = builder.add_pixel_float(2, pixelData);
  if (!vertexAdded || !pixelAdded) {
    std::puts("FAIL builder rejected legal ranges");
    api3->Release(); device->Release(); d3d->Release();
    DestroyWindow(window); UnregisterClassW(wc.lpszClassName, wc.hInstance);
    FreeLibrary(module); return 1;
  }
  hr = builder.submit(api3); report("legal two-register batch", hr);
  float observed[8]{}; HRESULT readHr = device->GetVertexShaderConstantF(0, observed, 2);
  bool applied = SUCCEEDED(readHr) && std::memcmp(observed, legalData, sizeof(observed)) == 0;
  std::printf("%-36s %s\n", "legal state observed", applied ? "PASS" : "FAIL");
  float pixelObserved[4]{}; readHr = device->GetPixelShaderConstantF(2, pixelObserved, 1);
  bool pixelApplied = SUCCEEDED(readHr) && std::memcmp(pixelObserved, pixelData, sizeof(pixelData)) == 0;
  std::printf("%-36s %s\n", "legal pixel range observed", pixelApplied ? "PASS" : "FAIL");

  const float invalidData[4] = { 42.f, 43.f, 44.f, 45.f };
  D3D9GtaSaFloatConstantRange ranges[2] = { {0,1,invalidData}, {0xffffffffu,1,invalidData} };
  D3D9GtaSaStateBatch invalid = legal; invalid.VertexFloatRangeCount=2; invalid.VertexFloatRanges=ranges;
  hr = api3->SubmitStateBatch(&invalid); report("invalid range rejected", hr, false);
  float after[4]{}; readHr = device->GetVertexShaderConstantF(0, after, 1);
  bool unchanged = SUCCEEDED(readHr) && std::memcmp(after, legalData, sizeof(after)) == 0;
  std::printf("%-36s %s\n", "failed batch had no partial state", unchanged ? "PASS" : "FAIL");

  D3D9GtaSaStateBatch nullData = legal; nullData.VertexFloatRanges=nullptr;
  hr = api3->SubmitStateBatch(&nullData); report("null range pointer rejected", hr, false);
  bool ok = applied && pixelApplied && unchanged;
  api3->Release(); device->Release(); d3d->Release(); DestroyWindow(window); UnregisterClassW(wc.lpszClassName, wc.hInstance); FreeLibrary(module);
  return ok ? 0 : 1;
}
