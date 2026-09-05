#include <windows.h>
#include <d3d9.h>
#include <sa_api1/status_client.hpp>

#include <filesystem>
#include <iostream>

namespace {
using Create9 = IDirect3D9* (WINAPI*)(UINT);
LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  return m == WM_DESTROY ? (PostQuitMessage(0), 0) : DefWindowProcW(h,m,w,l);
}
HWND ProbeWindow() {
  constexpr wchar_t kName[] = L"SaApi1StatusProbe";
  WNDCLASSW wc{}; wc.hInstance = GetModuleHandleW(nullptr); wc.lpfnWndProc = WndProc; wc.lpszClassName = kName;
  if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
  return CreateWindowExW(0,kName,L"SA API1",WS_OVERLAPPEDWINDOW,0,0,64,64,nullptr,nullptr,wc.hInstance,nullptr);
}
void hr(const char* name, HRESULT x) { std::cout << name << "=0x" << std::hex << static_cast<unsigned long>(x) << std::dec << '\n'; }
}

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) { std::wcerr << L"usage: sa-api1-status.exe <d3d9.dll>\n"; return 2; }
  HMODULE dll = LoadLibraryW(std::filesystem::path(argv[1]).c_str());
  if (!dll) { std::wcerr << L"[FAIL] LoadLibrary error=" << GetLastError() << L"\n"; return 1; }
  int result = 1;
  { // all COM objects are destroyed before FreeLibrary.
    auto create = reinterpret_cast<Create9>(GetProcAddress(dll,"Direct3DCreate9"));
    if (!create) { std::cout << "[FAIL] Direct3DCreate9 export missing\n"; }
    else if (IDirect3D9* d3d = create(D3D_SDK_VERSION)) {
      HWND window = ProbeWindow();
      if (window) {
        D3DPRESENT_PARAMETERS pp{}; pp.Windowed=TRUE; pp.SwapEffect=D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow=window; pp.BackBufferFormat=D3DFMT_UNKNOWN;
        IDirect3DDevice9* device = nullptr;
        HRESULT createHr = d3d->CreateDevice(D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,window,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,&device);
        if (SUCCEEDED(createHr) && device) {
          auto client = sa::api1::StatusClient::from_device(device);
          if (!client) hr("GetCompat", client.error());
          else if (auto status = client->status(); status) {
            hr("GetStatus", S_OK); std::cout << "api=" << status->ApiVersion << " flags=0x" << std::hex << status->Flags << std::dec << " presents=" << status->PresentCount << '\n';
            auto valid = sa::api1::StatusClient::validate(*status);
            if (!valid) hr("Validate", valid.error());
            auto interop = client->vulkan_interop();
            if (!interop) hr("GetVulkanInterop", interop.error());
            else { hr("GetVulkanInterop", S_OK); result = valid ? 0 : 1; }
          } else hr("GetStatus", status.error());
          device->Release();
        } else hr("CreateDevice", createHr);
        DestroyWindow(window);
      }
      d3d->Release();
    } else { std::cout << "[FAIL] Direct3DCreate9 returned null\n"; }
  }
  FreeLibrary(dll);
  return result;
}
