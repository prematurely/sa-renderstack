#include "test_check.hpp"
#include "../../../../../backend/dxvk/src/d3d9/d3d9_bridge_lifecycle.h"
#include <vector>
#include <thread>
#include <atomic>
#include <cstring>
#include <string>

namespace {
struct Events {
  std::vector<D3D9BridgeEvent> events;
  std::vector<HRESULT> resets;
  DWORD owner = GetCurrentThreadId();
  HANDLE entered{}, resume{};
  static void STDMETHODCALLTYPE notify(void* context, IDirect3DDevice9* device,
      D3D9BridgeEvent event, HRESULT result, D3DPRESENT_PARAMETERS*, const D3D9BridgePresentArgs* present) {
    auto& e = *static_cast<Events*>(context);
    CHECK(device != nullptr);
    e.events.push_back(event);
    if (event == D3D9BridgeEvent::PresentBefore || event == D3D9BridgeEvent::PresentAfter)
      CHECK(present != nullptr);
    if (event == D3D9BridgeEvent::ResetAfter) e.resets.push_back(result);
    if (event == D3D9BridgeEvent::PresentBefore && e.entered) {
      SetEvent(e.entered);
      CHECK(WaitForSingleObject(e.resume, 5000) == WAIT_OBJECT_0);
    }
  }
};
struct Fixture {
  HMODULE library{};
  HWND window{};
  IDirect3D9* factory{};
  D3DPRESENT_PARAMETERS params{};
  ~Fixture() {
    if (factory) factory->Release();
    if (window) DestroyWindow(window);
    if (library) FreeLibrary(library);
  }
  bool open(const char* dll, bool extended = false) {
    SetEnvironmentVariableA("DXVK_CONFIG", "d3d9.gtaSaCompat=True;d3d9.presentInterval=0;dxvk.hud=");
    SetEnvironmentVariableA("DXVK_CONFIG_FILE", "lifecycle-no-external.conf");
    SetEnvironmentVariableA("DXVK_LOG_LEVEL", "error");
    library = LoadLibraryA(dll);
    if (!library) return false;
    using Create = IDirect3D9* (WINAPI*)(UINT);
    auto create = reinterpret_cast<Create>(GetProcAddress(library, "Direct3DCreate9"));
    if (!create) return false;
    if (extended) {
      using CreateEx = HRESULT (WINAPI*)(UINT, IDirect3D9Ex**);
      auto ex = reinterpret_cast<CreateEx>(GetProcAddress(library, "Direct3DCreate9Ex"));
      IDirect3D9Ex* result{};
      if (!ex || FAILED(ex(D3D_SDK_VERSION, &result))) return false;
      factory = result;
    } else factory = create(D3D_SDK_VERSION);
    WNDCLASSA wc{}; wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "RenderStackLifecycle";
    RegisterClassA(&wc);
    window = CreateWindowA(wc.lpszClassName, "lifecycle", WS_OVERLAPPEDWINDOW, 0,0,128,128,nullptr,nullptr,wc.hInstance,nullptr);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    UpdateWindow(window);
    params.BackBufferWidth = params.BackBufferHeight = 64;
    params.BackBufferFormat = D3DFMT_A8R8G8B8; params.BackBufferCount = 1;
    params.SwapEffect = D3DSWAPEFFECT_DISCARD; params.hDeviceWindow = window;
    params.Windowed = TRUE; params.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    return factory && window;
  }
  IDirect3DDevice9* create() {
    IDirect3DDevice9* d{};
    CHECK(SUCCEEDED(factory->CreateDevice(0, D3DDEVTYPE_HAL, window,
      D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED, &params, &d)));
    return d;
  }
  void finish_present(IDirect3DDevice9* device) {
    // Present queues CS work. Wait for a real GPU event before Reset/Release
    // may unregister queued pass callbacks; no timing-based observation.
    IDirect3DQuery9* fence{};
    CHECK(SUCCEEDED(device->CreateQuery(D3DQUERYTYPE_EVENT, &fence)));
    if (!fence) return;
    CHECK(SUCCEEDED(fence->Issue(D3DISSUE_END)));
    HRESULT ready = S_FALSE;
    const auto deadline = GetTickCount64() + 5000;
    while (ready == S_FALSE && GetTickCount64() < deadline)
      ready = fence->GetData(nullptr,0,D3DGETDATA_FLUSH);
    CHECK(ready == S_OK);
    fence->Release();
  }
};

void backend(Fixture& f) {
  auto* device = f.create(); if (!device) return;
  ID3D9BridgeLifecycle* lifecycle{};
  const HRESULT qi = device->QueryInterface(IID_D3D9BridgeLifecycle, reinterpret_cast<void**>(&lifecycle));
  CHECK(qi == S_OK);
  if (FAILED(qi)) { device->Release(); return; }
  Events e;
  D3D9BridgeObserver desc{sizeof(desc), &e, &Events::notify};
  CHECK(lifecycle->SetObserver(&desc) == S_OK);
  CHECK(lifecycle->SetObserver(&desc) == D3DERR_INVALIDCALL);
  auto invalid = desc; --invalid.StructSize;
  CHECK(lifecycle->SetObserver(&invalid) == D3DERR_INVALIDCALL);
  CHECK(SUCCEEDED(device->Present(nullptr,nullptr,nullptr,nullptr)));
  auto bad = f.params; bad.SwapEffect = static_cast<D3DSWAPEFFECT>(0);
  CHECK(FAILED(device->Reset(&bad)));
  CHECK(SUCCEEDED(device->Reset(&f.params)));
  CHECK(e.resets.size() == 2);
  if (e.resets.size() == 2) { CHECK(FAILED(e.resets[0])); CHECK(e.resets[1] == S_OK); }
  CHECK(e.events.size() == 6);
  if (e.events.size() == 6) {
    CHECK(e.events[0] == D3D9BridgeEvent::PresentBefore);
    CHECK(e.events[1] == D3D9BridgeEvent::PresentAfter);
    CHECK(e.events[2] == D3D9BridgeEvent::ResetBefore);
    CHECK(e.events[3] == D3D9BridgeEvent::ResetAfter);
    CHECK(e.events[4] == D3D9BridgeEvent::ResetBefore);
    CHECK(e.events[5] == D3D9BridgeEvent::ResetAfter);
  }
  IDirect3DDevice9Ex* extended{};
  if (SUCCEEDED(device->QueryInterface(__uuidof(IDirect3DDevice9Ex), reinterpret_cast<void**>(&extended)))) {
    auto resetCount = e.resets.size();
    CHECK(FAILED(extended->ResetEx(&bad, nullptr)));
    CHECK(SUCCEEDED(extended->ResetEx(&f.params, nullptr)));
    CHECK(e.resets.size() == resetCount + 2);
    CHECK(SUCCEEDED(extended->PresentEx(nullptr,nullptr,nullptr,nullptr,0)));
    extended->Release();
  }
  CHECK(lifecycle->SetObserver(nullptr) == S_OK);
  auto count = e.events.size();
  CHECK(SUCCEEDED(device->Present(nullptr,nullptr,nullptr,nullptr)));
  CHECK(e.events.size() == count);
  CHECK(lifecycle->SetObserver(&desc) == S_OK);
  lifecycle->Release(); // registration is borrowed; no device retention
  CHECK(device->Release() == 0);
  CHECK(e.events.back() == D3D9BridgeEvent::FinalRelease);

  // Real concurrent unregister cannot return while callback data is in use.
  device = f.create(); if (!device) return;
  CHECK(SUCCEEDED(device->QueryInterface(IID_D3D9BridgeLifecycle, reinterpret_cast<void**>(&lifecycle))));
  if (!lifecycle) { device->Release(); return; }
  Events blocked;
  blocked.entered = CreateEventA(nullptr,TRUE,FALSE,nullptr);
  blocked.resume = CreateEventA(nullptr,TRUE,FALSE,nullptr);
  desc.Context = &blocked;
  CHECK(lifecycle->SetObserver(&desc) == S_OK);
  std::thread present([&] { CHECK(SUCCEEDED(device->Present(nullptr,nullptr,nullptr,nullptr))); });
  CHECK(WaitForSingleObject(blocked.entered, 5000) == WAIT_OBJECT_0);
  HANDLE cleared = CreateEventA(nullptr,TRUE,FALSE,nullptr);
  std::thread clear([&] { CHECK(lifecycle->SetObserver(nullptr) == S_OK); SetEvent(cleared); });
  CHECK(WaitForSingleObject(cleared, 50) == WAIT_TIMEOUT);
  SetEvent(blocked.resume); present.join(); clear.join();
  count = blocked.events.size();
  CHECK(SUCCEEDED(device->Present(nullptr,nullptr,nullptr,nullptr)));
  CHECK(blocked.events.size() == count);
  CloseHandle(cleared); CloseHandle(blocked.entered); CloseHandle(blocked.resume);
  lifecycle->Release(); CHECK(device->Release() == 0);
}

void bridge(Fixture& f, bool raw) {
  auto* first = f.create();
  if (first) CHECK(first->Release() == 0);
  ID3D9BridgeRuntime* runtime{};
  auto qi = f.factory->QueryInterface(IID_D3D9BridgeRuntime, reinterpret_cast<void**>(&runtime));
  CHECK(qi == S_OK); if (FAILED(qi)) return;
  for (int i=0; i<3; ++i) {
    auto* device = f.create(); if (!device) break;
    if (raw) {
      ID3D9BridgeLifecycle* observer{};
      CHECK(SUCCEEDED(device->QueryInterface(IID_D3D9BridgeLifecycle, reinterpret_cast<void**>(&observer))));
      if (observer) {
        Events events;
        D3D9BridgeObserver desc{sizeof(desc), &events, &Events::notify};
        CHECK(observer->SetObserver(&desc) == D3DERR_INVALIDCALL);
        observer->Release();
      }
    }
    CHECK(runtime->Shutdown() == D3DERR_INVALIDCALL);
    CHECK(SUCCEEDED(device->Present(nullptr,nullptr,nullptr,nullptr)));
    f.finish_present(device);
    auto bad=f.params; bad.SwapEffect=static_cast<D3DSWAPEFFECT>(0);
    CHECK(FAILED(device->Reset(&bad)));
    CHECK(SUCCEEDED(device->Present(nullptr,nullptr,nullptr,nullptr)));
    f.finish_present(device);
    CHECK(SUCCEEDED(device->Reset(&f.params)));
    CHECK(SUCCEEDED(device->Present(nullptr,nullptr,nullptr,nullptr)));
    f.finish_present(device);
    CHECK(device->Release() == 0);
  }
  auto* primary = f.create();
  auto* secondary = f.create();
  if (primary) CHECK(primary->Release() == 0);
  if (secondary) {
    CHECK(runtime->Shutdown() == D3DERR_INVALIDCALL);
    CHECK(SUCCEEDED(secondary->Present(nullptr,nullptr,nullptr,nullptr)));
    f.finish_present(secondary);
    CHECK(secondary->Release() == 0);
  }
  const auto shutdownStart = GetTickCount64();
  CHECK(runtime->Shutdown() == S_OK);
  CHECK(GetTickCount64() - shutdownStart < 5000);
  CHECK(runtime->Shutdown() == S_FALSE);
  IDirect3DDevice9* rejected{};
  CHECK(f.factory->CreateDevice(0,D3DDEVTYPE_HAL,f.window,D3DCREATE_HARDWARE_VERTEXPROCESSING,&f.params,&rejected) == D3DERR_INVALIDCALL);
  CHECK(rejected == nullptr);
  runtime->Release();
}
}
int main(int argc, char** argv) {
  if (argc!=3) return 2;
  if (std::strcmp(argv[2], "load-only") == 0) {
    CHECK(SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS));
    const DWORD before = GetPriorityClass(GetCurrentProcess());
    auto library = LoadLibraryA(argv[1]);
    CHECK(library != nullptr);
    CHECK(GetPriorityClass(GetCurrentProcess()) == before);
    CHECK(GetFileAttributesA("scripts/BridgeD3D9.log") == INVALID_FILE_ATTRIBUTES);
    if (library) CHECK(FreeLibrary(library));
    CHECK(GetModuleHandleA(argv[1]) == nullptr);
    return api_test::finish();
  }
  Fixture f;
  if (!f.open(argv[1], std::strcmp(argv[2], "backend-ex") == 0)) { std::fprintf(stderr,"device fixture failed: %lu\n",GetLastError()); return 2; }
  if (std::strncmp(argv[2],"backend",7)==0) backend(f); else bridge(f, std::strcmp(argv[2], "bridge-raw") == 0);
  return api_test::finish();
}
