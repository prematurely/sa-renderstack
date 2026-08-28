#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include <array>
#include <cstdio>
#include <cstring>

using Direct3DCreate9Fn = IDirect3D9* (WINAPI*)(UINT);

static LRESULT CALLBACK ProbeWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  return DefWindowProcA(window, message, wParam, lParam);
}

static bool EqualFloat4(const float* a, const float* b) {
  return std::memcmp(a, b, 4u * sizeof(float)) == 0;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: gta_sa.exe <d3d9.dll>\n");
    return 1;
  }

  HMODULE module = LoadLibraryA(argv[1]);
  if (!module) {
    std::fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError());
    return 2;
  }

  auto create9 = reinterpret_cast<Direct3DCreate9Fn>(
    GetProcAddress(module, "Direct3DCreate9"));
  if (!create9) {
    std::fprintf(stderr, "Direct3DCreate9 export missing\n");
    FreeLibrary(module);
    return 3;
  }

  WNDCLASSA windowClass = { };
  windowClass.lpfnWndProc = ProbeWndProc;
  windowClass.hInstance = GetModuleHandleA(nullptr);
  windowClass.lpszClassName = "DxvkStateBlockPrefilterProbe";
  RegisterClassA(&windowClass);

  HWND window = CreateWindowA(
    windowClass.lpszClassName, "DXVK state-block probe", WS_OVERLAPPEDWINDOW,
    CW_USEDEFAULT, CW_USEDEFAULT, 64, 64, nullptr, nullptr,
    windowClass.hInstance, nullptr);
  if (!window) {
    std::fprintf(stderr, "CreateWindow failed: %lu\n", GetLastError());
    FreeLibrary(module);
    return 4;
  }

  IDirect3D9* d3d9 = create9(D3D_SDK_VERSION);
  if (!d3d9) {
    DestroyWindow(window);
    FreeLibrary(module);
    return 5;
  }

  D3DPRESENT_PARAMETERS params = { };
  params.Windowed = TRUE;
  params.SwapEffect = D3DSWAPEFFECT_DISCARD;
  params.hDeviceWindow = window;
  params.BackBufferWidth = 64;
  params.BackBufferHeight = 64;
  params.BackBufferFormat = D3DFMT_A8R8G8B8;

  IDirect3DDevice9* device = nullptr;
  HRESULT result = d3d9->CreateDevice(
    D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
    D3DCREATE_HARDWARE_VERTEXPROCESSING, &params, &device);
  if (FAILED(result)) {
    std::fprintf(stderr, "CreateDevice failed: 0x%08lx\n", ULONG(result));
    d3d9->Release();
    DestroyWindow(window);
    FreeLibrary(module);
    return 6;
  }

  IDirect3DTexture9* textureA = nullptr;
  IDirect3DTexture9* textureB = nullptr;
  result = device->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &textureA, nullptr);
  result = SUCCEEDED(result)
    ? device->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &textureB, nullptr)
    : result;
  if (FAILED(result)) {
    std::fprintf(stderr, "CreateTexture failed: 0x%08lx\n", ULONG(result));
    if (textureA) textureA->Release();
    device->Release();
    d3d9->Release();
    DestroyWindow(window);
    FreeLibrary(module);
    return 7;
  }

  const std::array<float, 4> vertexA = { 1.0f, 2.0f, 3.0f, 4.0f };
  const std::array<float, 4> vertexB = { 5.0f, 6.0f, 7.0f, 8.0f };
  const std::array<float, 4> pixelA = { 0.1f, 0.2f, 0.3f, 0.4f };
  const std::array<float, 4> pixelB = { 0.5f, 0.6f, 0.7f, 0.8f };
  const BOOL boolA = TRUE;
  const BOOL boolB = FALSE;

  device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
  device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
  device->SetTexture(0, textureA);
  device->SetVertexShaderConstantF(0, vertexA.data(), 1);
  device->SetPixelShaderConstantF(0, pixelA.data(), 1);
  device->SetVertexShaderConstantB(0, &boolA, 1);
  device->SetPixelShaderConstantB(0, &boolA, 1);

  IDirect3DStateBlock9* block = nullptr;
  result = device->CreateStateBlock(D3DSBT_ALL, &block);
  if (FAILED(result) || !block) {
    std::fprintf(stderr, "CreateStateBlock failed: 0x%08lx\n", ULONG(result));
    textureB->Release();
    textureA->Release();
    device->Release();
    d3d9->Release();
    DestroyWindow(window);
    FreeLibrary(module);
    return 8;
  }

  device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
  device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
  device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  device->SetTexture(0, textureB);
  device->SetVertexShaderConstantF(0, vertexB.data(), 1);
  device->SetPixelShaderConstantF(0, pixelB.data(), 1);
  device->SetVertexShaderConstantB(0, &boolB, 1);
  device->SetPixelShaderConstantB(0, &boolB, 1);

  result = block->Apply();
  for (uint32_t i = 0; SUCCEEDED(result) && i < 32u; i++) {
    result = block->Capture();
    if (SUCCEEDED(result))
      result = block->Apply();
  }

  DWORD renderState = 0;
  DWORD samplerState = 0;
  DWORD textureStageState = 0;
  std::array<float, 4> vertexResult = { };
  std::array<float, 4> pixelResult = { };
  BOOL vertexBoolResult = FALSE;
  BOOL pixelBoolResult = FALSE;
  IDirect3DBaseTexture9* textureResult = nullptr;

  if (SUCCEEDED(result)) result = device->GetRenderState(D3DRS_ALPHABLENDENABLE, &renderState);
  if (SUCCEEDED(result)) result = device->GetSamplerState(0, D3DSAMP_MINFILTER, &samplerState);
  if (SUCCEEDED(result)) result = device->GetTextureStageState(0, D3DTSS_COLOROP, &textureStageState);
  if (SUCCEEDED(result)) result = device->GetTexture(0, &textureResult);
  if (SUCCEEDED(result)) result = device->GetVertexShaderConstantF(0, vertexResult.data(), 1);
  if (SUCCEEDED(result)) result = device->GetPixelShaderConstantF(0, pixelResult.data(), 1);
  if (SUCCEEDED(result)) result = device->GetVertexShaderConstantB(0, &vertexBoolResult, 1);
  if (SUCCEEDED(result)) result = device->GetPixelShaderConstantB(0, &pixelBoolResult, 1);

  const bool passed = SUCCEEDED(result)
    && renderState == FALSE
    && samplerState == D3DTEXF_POINT
    && textureStageState == D3DTOP_MODULATE
    && textureResult == textureA
    && EqualFloat4(vertexResult.data(), vertexA.data())
    && EqualFloat4(pixelResult.data(), pixelA.data())
    && vertexBoolResult == TRUE
    && pixelBoolResult == TRUE;

  std::printf("stateblock_prefilter=%s result=0x%08lx\n",
    passed ? "pass" : "fail", ULONG(result));

  if (textureResult) textureResult->Release();
  block->Release();
  device->SetTexture(0, nullptr);
  textureB->Release();
  textureA->Release();
  device->Release();
  d3d9->Release();
  DestroyWindow(window);
  FreeLibrary(module);
  return passed ? 0 : 9;
}
