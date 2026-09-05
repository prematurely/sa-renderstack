#include <windows.h>
#include <d3d9.h>

#include <sa_renderstack/backend_api.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace {
using Direct3DCreate9Fn = IDirect3D9* (WINAPI*)(UINT);
constexpr int kSkip = 3;

struct PassProbe {
  int id;
  std::atomic_uint calls{0};
  std::atomic_uint failures{0};
  std::mutex mutex;
  std::vector<int> order;
};

HRESULT STDMETHODCALLTYPE RecordPass(void* userData,
    const D3D9GtaSaVulkanFrameContext* frame) {
  auto* probe = static_cast<PassProbe*>(userData);
  if (!probe || !frame || frame->StructSize < sizeof(*frame) ||
      frame->ApiVersion < 2u || frame->Stage != D3D9_GTA_SA_VULKAN_PASS_AFTER_BLIT)
    return E_INVALIDARG;
  ++probe->calls;
  std::lock_guard lock(probe->mutex);
  probe->order.push_back(probe->id);
  // This probe deliberately records metadata only. It does not submit, end,
  // reset, or alter the supplied command buffer or image layouts.
  return S_OK;
}

HRESULT STDMETHODCALLTYPE FailingPass(void* userData,
    const D3D9GtaSaVulkanFrameContext* frame) {
  auto* probe = static_cast<PassProbe*>(userData);
  if (probe) ++probe->failures;
  (void)frame;
  return E_FAIL; // backend must disable this callback without stopping others
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

HWND CreateProbeWindow(HINSTANCE instance) {
  constexpr wchar_t kClassName[] = L"SaRenderStackApi2Probe";
  WNDCLASSW wc{}; wc.hInstance = instance; wc.lpfnWndProc = WndProc;
  wc.lpszClassName = kClassName;
  if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    return nullptr;
  return CreateWindowExW(0, kClassName, L"SA RenderStack API2 probe",
                         WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr,
                         instance, nullptr);
}

D3D9GtaSaVulkanPassDesc MakeDesc(const char* name, int priority,
    D3D9GtaSaRecordVulkanPass record, PassProbe* probe) {
  D3D9GtaSaVulkanPassDesc desc{};
  desc.StructSize = sizeof(desc);
  desc.ApiVersion = 2u;
  desc.Priority = priority;
  desc.Stage = D3D9_GTA_SA_VULKAN_PASS_AFTER_BLIT;
  desc.Flags = D3D9_GTA_SA_VULKAN_PASS_RESTORES_LAYOUTS;
  strncpy_s(desc.Name, name, _TRUNCATE);
  desc.Record = record;
  desc.UserData = probe;
  return desc;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
  const std::filesystem::path dllPath = argc > 1 ? argv[1] : L"d3d9.dll";
  HMODULE module = LoadLibraryW(dllPath.c_str());
  if (!module) {
    std::wcerr << L"[SKIP] cannot load backend: " << dllPath << L"\n";
    return kSkip;
  }
  auto create9 = reinterpret_cast<Direct3DCreate9Fn>(
      GetProcAddress(module, "Direct3DCreate9"));
  if (!create9) { std::cout << "[SKIP] Direct3DCreate9 export unavailable\n";
    FreeLibrary(module); return kSkip; }
  IDirect3D9* d3d = create9(D3D_SDK_VERSION);
  if (!d3d) { std::cout << "[SKIP] no D3D9 adapter/GPU\n";
    FreeLibrary(module); return kSkip; }
  HWND window = CreateProbeWindow(GetModuleHandleW(nullptr));
  if (!window) { std::cout << "[SKIP] probe window creation failed\n";
    d3d->Release(); FreeLibrary(module); return kSkip; }

  D3DPRESENT_PARAMETERS pp{}; pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = window;
  pp.BackBufferFormat = D3DFMT_UNKNOWN;
  IDirect3DDevice9* device = nullptr;
  HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
      D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device);
  if (FAILED(hr) || !device) { std::cout << "[SKIP] D3D9 device unavailable\n";
    DestroyWindow(window); d3d->Release(); FreeLibrary(module); return kSkip; }

  ID3D9GtaSaCompatDevice1* api = nullptr;
  hr = device->QueryInterface(__uuidof(ID3D9GtaSaCompatDevice1),
                              reinterpret_cast<void**>(&api));
  if (FAILED(hr) || !api) { std::cout << "[SKIP] API2 interface unavailable\n";
    device->Release(); DestroyWindow(window); d3d->Release(); FreeLibrary(module);
    return kSkip; }

  PassProbe failing{0}, low{1}, high{2};
  auto bad = MakeDesc("api2-failing", -100, &FailingPass, &failing);
  auto first = MakeDesc("api2-low", 10, &RecordPass, &low);
  auto second = MakeDesc("api2-high", 20, &RecordPass, &high);
  UINT64 badToken = 0, firstToken = 0, secondToken = 0;
  HRESULT badHr = api->RegisterVulkanPass(&bad, &badToken);
  HRESULT firstHr = api->RegisterVulkanPass(&first, &firstToken);
  HRESULT secondHr = api->RegisterVulkanPass(&second, &secondToken);
  std::cout << "register failing=" << std::hex << static_cast<unsigned long>(badHr)
            << " low=" << static_cast<unsigned long>(firstHr)
            << " high=" << static_cast<unsigned long>(secondHr) << std::dec << '\n';
  if (FAILED(badHr) || FAILED(firstHr) || FAILED(secondHr)) {
    std::cout << "[FAIL] RegisterVulkanPass contract\n";
    if (badToken) api->UnregisterVulkanPass(badToken);
    if (firstToken) api->UnregisterVulkanPass(firstToken);
    if (secondToken) api->UnregisterVulkanPass(secondToken);
    api->Release(); device->Release(); DestroyWindow(window); d3d->Release();
    FreeLibrary(module); return 1;
  }

  // Present drives the backend's existing frame command buffer and therefore
  // is the only legal way for this external probe to receive a frame context.
  device->Present(nullptr, nullptr, nullptr, nullptr);
  device->Present(nullptr, nullptr, nullptr, nullptr);

  HRESULT unregBad = api->UnregisterVulkanPass(badToken);
  HRESULT unregFirst = api->UnregisterVulkanPass(firstToken);
  HRESULT unregSecond = api->UnregisterVulkanPass(secondToken);
  HRESULT unregAgain = api->UnregisterVulkanPass(secondToken);
  std::cout << "callbacks failing=" << failing.calls.load() << " failures="
            << failing.failures.load() << " low=" << low.calls.load()
            << " high=" << high.calls.load() << '\n';
  std::cout << "unregister=0x" << std::hex << static_cast<unsigned long>(unregBad)
            << ",0x" << static_cast<unsigned long>(unregFirst) << ",0x"
            << static_cast<unsigned long>(unregSecond) << ",again=0x"
            << static_cast<unsigned long>(unregAgain) << std::dec << '\n';

  const bool invoked = low.calls.load() || high.calls.load() || failing.calls.load();
  bool orderOk = true;
  { std::scoped_lock lock(low.mutex, high.mutex);
    if (low.order.size() >= 1 && high.order.size() >= 1)
      orderOk = low.order.front() == 1 && high.order.front() == 2;
  }
  const bool unregisterOk = SUCCEEDED(unregBad) && SUCCEEDED(unregFirst) &&
                            SUCCEEDED(unregSecond) && FAILED(unregAgain);
  if (!invoked)
    std::cout << "[FAIL] Vulkan frame callbacks were not reached; runtime verification did not execute\n";
  else if (unregisterOk && orderOk && failing.failures.load() <= 1)
    std::cout << "[PASS] API2 registration, priority and failure isolation\n";
  else
    std::cout << "[FAIL] API2 callback contract\n";

  api->Release(); device->Release(); DestroyWindow(window); d3d->Release();
  FreeLibrary(module);
  return !invoked ? 1 : ((unregisterOk && orderOk && failing.failures.load() <= 1) ? 0 : 1);
}
