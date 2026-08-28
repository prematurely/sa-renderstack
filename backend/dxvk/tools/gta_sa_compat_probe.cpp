#include <windows.h>
#include <d3d9.h>

#include <cstdio>
#include <cstring>
#include <atomic>
#include <cmath>
#include <cstdint>

#include <d3d9_gta_sa_api.h>

using Direct3DCreate9Proc = IDirect3D9* (WINAPI*) (UINT);

static std::atomic_uint g_vulkanPassCalls = { 0u };
static std::atomic_uint64_t g_vulkanPassOrder = { 0u };
static HANDLE g_slowPassStarted = nullptr;
static HANDLE g_slowPassFinished = nullptr;

static HRESULT STDMETHODCALLTYPE ProbeVulkanPass(
        void* userData,
  const D3D9GtaSaVulkanFrameContext* frame) {
  if (frame == nullptr
   || frame->StructSize < sizeof(D3D9GtaSaVulkanFrameContext)
   || frame->ApiVersion != D3D9_GTA_SA_COMPAT_API_VERSION
   || frame->Stage != D3D9_GTA_SA_VULKAN_PASS_AFTER_BLIT
   || frame->CommandBuffer == VK_NULL_HANDLE
   || frame->SourceImage == VK_NULL_HANDLE
   || frame->OutputImage == VK_NULL_HANDLE)
    return E_INVALIDARG;

  UINT64 passId = UINT64(reinterpret_cast<uintptr_t>(userData));
  UINT64 order = g_vulkanPassOrder.load();
  while (!g_vulkanPassOrder.compare_exchange_weak(order, order * 10u + passId)) { }

  g_vulkanPassCalls.fetch_add(1u);
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE SlowProbeVulkanPass(
        void*,
  const D3D9GtaSaVulkanFrameContext* frame) {
  if (frame == nullptr || frame->CommandBuffer == VK_NULL_HANDLE)
    return E_INVALIDARG;

  SetEvent(g_slowPassStarted);
  Sleep(500);
  SetEvent(g_slowPassFinished);
  return S_OK;
}

static LRESULT CALLBACK ProbeWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  return DefWindowProcA(window, message, wparam, lparam);
}

static HRESULT WaitForDeviceWork(IDirect3DDevice9* device) {
  IDirect3DQuery9* query = nullptr;
  HRESULT result = device->CreateQuery(D3DQUERYTYPE_EVENT, &query);
  if (FAILED(result))
    return result;

  result = query->Issue(D3DISSUE_END);
  if (SUCCEEDED(result)) {
    DWORD start = GetTickCount();
    do {
      result = query->GetData(nullptr, 0, D3DGETDATA_FLUSH);
      if (result != S_FALSE)
        break;
      Sleep(1);
    } while (GetTickCount() - start < 5000u);

    if (result == S_FALSE)
      result = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
  }

  query->Release();
  return result;
}

static bool NearlyEqual(float a, float b) {
  return std::fabs(a - b) < 0.0001f;
}

static uint64_t ReadQpc() {
  LARGE_INTEGER value = { };
  QueryPerformanceCounter(&value);
  return uint64_t(value.QuadPart);
}

template <typename T>
static void ReleaseCom(T*& object) {
  if (object != nullptr) {
    object->Release();
    object = nullptr;
  }
}

static bool FillTexture(IDirect3DTexture9* texture, D3DCOLOR color) {
  if (texture == nullptr)
    return false;

  D3DLOCKED_RECT locked = { };
  if (FAILED(texture->LockRect(0u, &locked, nullptr, 0u)) || locked.pBits == nullptr)
    return false;

  *static_cast<DWORD*>(locked.pBits) = color;
  texture->UnlockRect(0u);
  return true;
}

static bool ReadSurfacePixel(
        IDirect3DDevice9* device,
        IDirect3DSurface9* renderTarget,
        IDirect3DSurface9* readback,
        UINT x,
        UINT y,
        D3DCOLOR expected) {
  if (FAILED(device->GetRenderTargetData(renderTarget, readback)))
    return false;

  D3DLOCKED_RECT locked = { };
  if (FAILED(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY)) || locked.pBits == nullptr)
    return false;

  const auto* row = static_cast<const uint8_t*>(locked.pBits) + size_t(y) * locked.Pitch;
  const DWORD actual = *reinterpret_cast<const DWORD*>(row + size_t(x) * sizeof(DWORD));
  readback->UnlockRect();
  return (actual & 0x00ffffffu) == (expected & 0x00ffffffu);
}

static bool RunStateDrawBatchProbe(
        IDirect3DDevice9*          device,
        ID3D9GtaSaCompatDevice5*  compat) {
  struct ProbeVertex {
    float x;
    float y;
    float z;
    float rhw;
    float u;
    float v;
  };

  constexpr DWORD ProbeFvf = D3DFVF_XYZRHW | D3DFVF_TEX1;
  constexpr D3DCOLOR Red = D3DCOLOR_ARGB(255, 255, 0, 0);
  constexpr D3DCOLOR Green = D3DCOLOR_ARGB(255, 0, 255, 0);
  constexpr D3DCOLOR Blue = D3DCOLOR_ARGB(255, 0, 0, 255);

  IDirect3DSurface9* oldRenderTarget = nullptr;
  IDirect3DSurface9* oldDepthStencil = nullptr;
  IDirect3DSurface9* renderTarget = nullptr;
  IDirect3DSurface9* readback = nullptr;
  IDirect3DTexture9* textureA = nullptr;
  IDirect3DTexture9* textureB = nullptr;
  IDirect3DTexture9* textureC = nullptr;
  IDirect3DVertexBuffer9* vertexBuffer = nullptr;
  IDirect3DIndexBuffer9* indexBuffer = nullptr;

  HRESULT result = device->GetRenderTarget(0u, &oldRenderTarget);
  if (SUCCEEDED(result))
    device->GetDepthStencilSurface(&oldDepthStencil);
  if (SUCCEEDED(result)) {
    result = device->CreateRenderTarget(
      64u, 64u, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0u, TRUE,
      &renderTarget, nullptr);
  }
  if (SUCCEEDED(result)) {
    result = device->CreateOffscreenPlainSurface(
      64u, 64u, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr);
  }
  if (SUCCEEDED(result))
    result = device->CreateTexture(1u, 1u, 1u, 0u, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &textureA, nullptr);
  if (SUCCEEDED(result))
    result = device->CreateTexture(1u, 1u, 1u, 0u, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &textureB, nullptr);
  if (SUCCEEDED(result))
    result = device->CreateTexture(1u, 1u, 1u, 0u, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &textureC, nullptr);
  if (SUCCEEDED(result)) {
    result = device->CreateVertexBuffer(
      3u * sizeof(ProbeVertex), D3DUSAGE_WRITEONLY, ProbeFvf,
      D3DPOOL_MANAGED, &vertexBuffer, nullptr);
  }
  if (SUCCEEDED(result)) {
    result = device->CreateIndexBuffer(
      3u * sizeof(WORD), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
      D3DPOOL_MANAGED, &indexBuffer, nullptr);
  }

  bool setupPassed = SUCCEEDED(result)
    && FillTexture(textureA, Red)
    && FillTexture(textureB, Green)
    && FillTexture(textureC, Blue);

  if (setupPassed) {
    const ProbeVertex vertices[] = {
      { -0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f },
      { 63.5f, -0.5f, 0.0f, 1.0f, 1.0f, 0.0f },
      { -0.5f, 63.5f, 0.0f, 1.0f, 0.0f, 1.0f },
    };
    const WORD indices[] = { 0u, 1u, 2u };

    void* mapped = nullptr;
    if (FAILED(vertexBuffer->Lock(0u, sizeof(vertices), &mapped, 0u)) || mapped == nullptr) {
      setupPassed = false;
    } else {
      std::memcpy(mapped, vertices, sizeof(vertices));
      vertexBuffer->Unlock();
    }

    mapped = nullptr;
    if (setupPassed
     && (FAILED(indexBuffer->Lock(0u, sizeof(indices), &mapped, 0u)) || mapped == nullptr)) {
      setupPassed = false;
    } else if (setupPassed) {
      std::memcpy(mapped, indices, sizeof(indices));
      indexBuffer->Unlock();
    }
  }

  if (setupPassed) {
    result = device->SetRenderTarget(0u, renderTarget);
    if (SUCCEEDED(result)) result = device->SetDepthStencilSurface(nullptr);
    if (SUCCEEDED(result)) result = device->SetFVF(ProbeFvf);
    if (SUCCEEDED(result)) result = device->SetStreamSource(0u, vertexBuffer, 0u, sizeof(ProbeVertex));
    if (SUCCEEDED(result)) result = device->SetIndices(indexBuffer);
    if (SUCCEEDED(result)) result = device->SetRenderState(D3DRS_ZENABLE, FALSE);
    if (SUCCEEDED(result)) result = device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    if (SUCCEEDED(result)) result = device->SetRenderState(D3DRS_LIGHTING, FALSE);
    if (SUCCEEDED(result)) result = device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    if (SUCCEEDED(result)) result = device->SetTextureStageState(0u, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    if (SUCCEEDED(result)) result = device->SetTextureStageState(0u, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    if (SUCCEEDED(result)) result = device->SetTextureStageState(0u, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    if (SUCCEEDED(result)) result = device->SetTextureStageState(0u, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    if (SUCCEEDED(result)) result = device->SetSamplerState(0u, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    if (SUCCEEDED(result)) result = device->SetSamplerState(0u, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    if (SUCCEEDED(result)) result = device->SetSamplerState(0u, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    setupPassed = SUCCEEDED(result);
  }

  D3D9GtaSaTextureBinding textureBinding = { 0u, textureB };
  D3D9GtaSaStateBatch stateBatch = { };
  stateBatch.StructSize = sizeof(stateBatch);
  stateBatch.ApiVersion = D3D9_GTA_SA_COMPAT_API_VERSION;
  stateBatch.TextureBindingCount = 1u;
  stateBatch.TextureBindings = &textureBinding;

  D3D9GtaSaStateDrawBatch stateDraw = { };
  stateDraw.StructSize = sizeof(stateDraw);
  stateDraw.ApiVersion = D3D9_GTA_SA_COMPAT_API_VERSION;
  stateDraw.StateBatch = &stateBatch;
  stateDraw.Draw.StructSize = sizeof(stateDraw.Draw);
  stateDraw.Draw.Kind = D3D9_GTA_SA_DRAW_PRIMITIVE;
  stateDraw.Draw.PrimitiveType = D3DPT_TRIANGLELIST;
  stateDraw.Draw.StartIndexOrVertex = 0u;
  stateDraw.Draw.PrimitiveCount = 1u;

  bool validationPassed = setupPassed
    && compat->SubmitStateDrawBatch(nullptr) == E_POINTER;
  D3D9GtaSaStateDrawBatch invalid = stateDraw;
  invalid.StructSize = sizeof(invalid) - 1u;
  validationPassed = validationPassed
    && compat->SubmitStateDrawBatch(&invalid) == D3DERR_INVALIDCALL;
  invalid = stateDraw;
  invalid.ApiVersion = 5u;
  validationPassed = validationPassed
    && compat->SubmitStateDrawBatch(&invalid) == D3DERR_INVALIDCALL;
  invalid = stateDraw;
  invalid.StateBatch = nullptr;
  validationPassed = validationPassed
    && compat->SubmitStateDrawBatch(&invalid) == D3DERR_INVALIDCALL;
  invalid = stateDraw;
  invalid.Draw.StructSize = sizeof(invalid.Draw) - 1u;
  validationPassed = validationPassed
    && compat->SubmitStateDrawBatch(&invalid) == D3DERR_INVALIDCALL;
  invalid = stateDraw;
  invalid.Draw.Kind = 0xffffffffu;
  validationPassed = validationPassed
    && compat->SubmitStateDrawBatch(&invalid) == D3DERR_INVALIDCALL;
  invalid = stateDraw;
  invalid.Draw.Reserved = 1u;
  validationPassed = validationPassed
    && compat->SubmitStateDrawBatch(&invalid) == D3DERR_INVALIDCALL;
  invalid = stateDraw;
  invalid.Draw.PrimitiveType = static_cast<D3DPRIMITIVETYPE>(0u);
  validationPassed = validationPassed
    && compat->SubmitStateDrawBatch(&invalid) == D3DERR_INVALIDCALL;
  invalid = stateDraw;
  invalid.Draw.BaseVertexIndex = 1;
  validationPassed = validationPassed
    && compat->SubmitStateDrawBatch(&invalid) == D3DERR_INVALIDCALL;

  bool noPartialState = false;
  if (setupPassed) {
    device->SetTexture(0u, textureA);
    invalid = stateDraw;
    invalid.Draw.Kind = 0xffffffffu;
    const HRESULT invalidResult = compat->SubmitStateDrawBatch(&invalid);
    IDirect3DBaseTexture9* currentTexture = nullptr;
    const HRESULT getResult = device->GetTexture(0u, &currentTexture);
    noPartialState = invalidResult == D3DERR_INVALIDCALL
      && SUCCEEDED(getResult) && currentTexture == textureA;
    ReleaseCom(currentTexture);
  }

  bool primitiveDrawPassed = false;
  bool primitiveJournalRestored = false;
  if (setupPassed) {
    device->Clear(0u, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0u);
    device->SetTexture(0u, textureA);
    result = device->BeginScene();
    if (SUCCEEDED(result)) result = compat->BeginStateJournal();
    if (SUCCEEDED(result)) result = compat->SubmitStateDrawBatch(&stateDraw);
    if (SUCCEEDED(result)) result = compat->RestoreStateJournal();
    if (SUCCEEDED(result)) result = device->EndScene();

    IDirect3DBaseTexture9* restoredTexture = nullptr;
    const HRESULT getResult = device->GetTexture(0u, &restoredTexture);
    primitiveJournalRestored = SUCCEEDED(getResult) && restoredTexture == textureA;
    ReleaseCom(restoredTexture);
    primitiveDrawPassed = SUCCEEDED(result)
      && ReadSurfacePixel(device, renderTarget, readback, 8u, 8u, Green);
  }

  bool indexedDrawPassed = false;
  bool indexedJournalRestored = false;
  if (setupPassed) {
    textureBinding.Texture = textureC;
    stateDraw.Draw.Kind = D3D9_GTA_SA_DRAW_INDEXED_PRIMITIVE;
    stateDraw.Draw.BaseVertexIndex = 0;
    stateDraw.Draw.MinVertexIndex = 0u;
    stateDraw.Draw.NumVertices = 3u;
    stateDraw.Draw.StartIndexOrVertex = 0u;
    stateDraw.Draw.PrimitiveCount = 1u;

    device->Clear(0u, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0u);
    device->SetTexture(0u, textureA);
    result = device->BeginScene();
    if (SUCCEEDED(result)) result = compat->BeginStateJournal();
    if (SUCCEEDED(result)) result = compat->SubmitStateDrawBatch(&stateDraw);
    if (SUCCEEDED(result)) result = compat->RestoreStateJournal();
    if (SUCCEEDED(result)) result = device->EndScene();

    IDirect3DBaseTexture9* restoredTexture = nullptr;
    const HRESULT getResult = device->GetTexture(0u, &restoredTexture);
    indexedJournalRestored = SUCCEEDED(getResult) && restoredTexture == textureA;
    ReleaseCom(restoredTexture);
    indexedDrawPassed = SUCCEEDED(result)
      && ReadSurfacePixel(device, renderTarget, readback, 8u, 8u, Blue);
  }

  constexpr UINT BenchmarkIterations = 50000u;
  LARGE_INTEGER frequency = { };
  QueryPerformanceFrequency(&frequency);
  double separateNs = 0.0;
  double fusedNs = 0.0;
  HRESULT benchmarkResult = setupPassed ? D3D_OK : E_FAIL;
  if (setupPassed) {
    stateDraw.Draw.Kind = D3D9_GTA_SA_DRAW_PRIMITIVE;
    stateDraw.Draw.BaseVertexIndex = 0;
    stateDraw.Draw.MinVertexIndex = 0u;
    stateDraw.Draw.NumVertices = 0u;
    stateDraw.Draw.StartIndexOrVertex = 0u;
    stateDraw.Draw.PrimitiveCount = 0u;

    uint64_t begin = ReadQpc();
    for (UINT i = 0u; i < BenchmarkIterations && SUCCEEDED(benchmarkResult); i++) {
      textureBinding.Texture = (i & 1u) ? textureB : textureA;
      benchmarkResult = compat->SubmitStateBatch(&stateBatch);
      if (SUCCEEDED(benchmarkResult))
        benchmarkResult = device->DrawPrimitive(D3DPT_TRIANGLELIST, 0u, 0u);
    }
    uint64_t end = ReadQpc();
    separateNs = frequency.QuadPart
      ? 1.0e9 * double(end - begin) / double(frequency.QuadPart) / BenchmarkIterations
      : 0.0;

    begin = ReadQpc();
    for (UINT i = 0u; i < BenchmarkIterations && SUCCEEDED(benchmarkResult); i++) {
      textureBinding.Texture = (i & 1u) ? textureB : textureA;
      benchmarkResult = compat->SubmitStateDrawBatch(&stateDraw);
    }
    end = ReadQpc();
    fusedNs = frequency.QuadPart
      ? 1.0e9 * double(end - begin) / double(frequency.QuadPart) / BenchmarkIterations
      : 0.0;
  }

  std::printf(
    "state_draw_validation=%s no_partial_state=%s dp_pixel=%s dp_restore=%s dip_pixel=%s dip_restore=%s result=0x%08lx\n",
    validationPassed ? "yes" : "no",
    noPartialState ? "yes" : "no",
    primitiveDrawPassed ? "yes" : "no",
    primitiveJournalRestored ? "yes" : "no",
    indexedDrawPassed ? "yes" : "no",
    indexedJournalRestored ? "yes" : "no",
    ULONG(result));
  std::printf(
    "state_draw_benchmark iterations=%u separate_ns=%.1f fused_ns=%.1f speedup=%.2fx result=0x%08lx\n",
    BenchmarkIterations, separateNs, fusedNs,
    fusedNs > 0.0 ? separateNs / fusedNs : 0.0,
    ULONG(benchmarkResult));

  device->SetTexture(0u, nullptr);
  device->SetIndices(nullptr);
  device->SetStreamSource(0u, nullptr, 0u, 0u);
  if (oldRenderTarget != nullptr)
    device->SetRenderTarget(0u, oldRenderTarget);
  device->SetDepthStencilSurface(oldDepthStencil);

  ReleaseCom(indexBuffer);
  ReleaseCom(vertexBuffer);
  ReleaseCom(textureC);
  ReleaseCom(textureB);
  ReleaseCom(textureA);
  ReleaseCom(readback);
  ReleaseCom(renderTarget);
  ReleaseCom(oldDepthStencil);
  ReleaseCom(oldRenderTarget);

  return setupPassed && validationPassed && noPartialState
      && primitiveDrawPassed && primitiveJournalRestored
      && indexedDrawPassed && indexedJournalRestored
      && SUCCEEDED(benchmarkResult);
}

static bool RunEffectStateBatchProbe(
        IDirect3DDevice9*          device,
        ID3D9GtaSaCompatDevice4*  compat) {
  IDirect3DTexture9* textureA = nullptr;
  IDirect3DTexture9* textureB = nullptr;
  HRESULT result = device->CreateTexture(
    4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &textureA, nullptr);
  if (SUCCEEDED(result)) {
    result = device->CreateTexture(
      4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &textureB, nullptr);
  }
  if (FAILED(result) || textureA == nullptr || textureB == nullptr) {
    if (textureA) textureA->Release();
    if (textureB) textureB->Release();
    std::fprintf(stderr, "effect_batch texture setup failed: 0x%08lx\n", ULONG(result));
    return false;
  }

  D3DMATRIX initialTransform = { };
  initialTransform._11 = initialTransform._22 = initialTransform._33 = initialTransform._44 = 1.0f;
  D3DMATRIX changedTransform = initialTransform;
  changedTransform._41 = 37.0f;

  D3DMATERIAL9 initialMaterial = { };
  initialMaterial.Diffuse.r = 0.125f;
  initialMaterial.Diffuse.g = 0.250f;
  initialMaterial.Diffuse.b = 0.375f;
  initialMaterial.Diffuse.a = 1.0f;
  D3DMATERIAL9 changedMaterial = initialMaterial;
  changedMaterial.Diffuse.r = 0.875f;

  D3DLIGHT9 initialLight = { };
  initialLight.Type = D3DLIGHT_DIRECTIONAL;
  initialLight.Diffuse.r = 0.25f;
  initialLight.Direction.z = 1.0f;
  D3DLIGHT9 changedLight = initialLight;
  changedLight.Diffuse.r = 0.75f;
  changedLight.Direction.x = 1.0f;
  changedLight.Direction.z = 0.0f;

  const float initialVsF[8] = {
    1.0f, 2.0f, 3.0f, 4.0f,
    5.0f, 6.0f, 7.0f, 8.0f,
  };
  const float changedVsF[8] = {
    11.0f, 12.0f, 13.0f, 14.0f,
    15.0f, 16.0f, 17.0f, 18.0f,
  };
  const int initialVsI[4] = { 1, 2, 3, 4 };
  const int changedVsI[4] = { 11, 12, 13, 14 };
  const BOOL initialVsB = FALSE;
  const BOOL changedVsB = TRUE;
  const float initialPsF[8] = {
    21.0f, 22.0f, 23.0f, 24.0f,
    25.0f, 26.0f, 27.0f, 28.0f,
  };
  const float changedPsF[8] = {
    31.0f, 32.0f, 33.0f, 34.0f,
    35.0f, 36.0f, 37.0f, 38.0f,
  };
  const int initialPsI[4] = { 5, 6, 7, 8 };
  const int changedPsI[4] = { 15, 16, 17, 18 };
  const BOOL initialPsB = FALSE;
  const BOOL changedPsB = TRUE;

  result = device->SetTransform(D3DTS_WORLD, &initialTransform);
  if (SUCCEEDED(result)) result = device->SetMaterial(&initialMaterial);
  if (SUCCEEDED(result)) result = device->SetLight(0u, &initialLight);
  if (SUCCEEDED(result)) result = device->LightEnable(0u, FALSE);
  if (SUCCEEDED(result)) result = device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  if (SUCCEEDED(result)) result = device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
  if (SUCCEEDED(result)) result = device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ZERO);
  if (SUCCEEDED(result)) result = device->SetTexture(0u, textureA);
  if (SUCCEEDED(result)) result = device->SetTextureStageState(0u, D3DTSS_COLOROP, D3DTOP_MODULATE);
  if (SUCCEEDED(result)) result = device->SetSamplerState(0u, D3DSAMP_MINFILTER, D3DTEXF_POINT);
  if (SUCCEEDED(result)) result = device->SetSamplerState(0u, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
  if (SUCCEEDED(result)) result = device->SetNPatchMode(0.0f);
  if (SUCCEEDED(result)) result = device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
  if (SUCCEEDED(result)) result = device->SetVertexShader(nullptr);
  if (SUCCEEDED(result)) result = device->SetPixelShader(nullptr);
  if (SUCCEEDED(result)) result = device->SetVertexShaderConstantF(8u, initialVsF, 2u);
  if (SUCCEEDED(result)) result = device->SetVertexShaderConstantI(1u, initialVsI, 1u);
  if (SUCCEEDED(result)) result = device->SetVertexShaderConstantB(2u, &initialVsB, 1u);
  if (SUCCEEDED(result)) result = device->SetPixelShaderConstantF(12u, initialPsF, 2u);
  if (SUCCEEDED(result)) result = device->SetPixelShaderConstantI(3u, initialPsI, 1u);
  if (SUCCEEDED(result)) result = device->SetPixelShaderConstantB(4u, &initialPsB, 1u);

  D3D9GtaSaTransformBinding transforms[] = {
    { D3DTS_WORLD, changedTransform },
  };
  D3D9GtaSaLightBinding lights[] = {
    { 0u, changedLight },
  };
  D3D9GtaSaLightEnableBinding lightEnables[] = {
    { 0u, TRUE },
  };
  D3D9GtaSaRenderStateBinding renderStates[] = {
    { D3DRS_ALPHABLENDENABLE, TRUE },
    { D3DRS_SRCBLEND, D3DBLEND_SRCALPHA },
    { D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA },
  };
  D3D9GtaSaTextureBinding textures[] = {
    { 0u, textureB },
  };
  D3D9GtaSaTextureStageStateBinding textureStageStates[] = {
    { 0u, D3DTSS_COLOROP, D3DTOP_SELECTARG1 },
  };
  D3D9GtaSaSamplerStateBinding samplerStates[] = {
    { 0u, D3DSAMP_MINFILTER, D3DTEXF_LINEAR },
    { 0u, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR },
  };
  D3D9GtaSaFloatConstantRange vsFloatRanges[] = {
    { 8u, 2u, changedVsF },
  };
  D3D9GtaSaIntConstantRange vsIntRanges[] = {
    { 1u, 1u, changedVsI },
  };
  D3D9GtaSaBoolConstantRange vsBoolRanges[] = {
    { 2u, 1u, &changedVsB },
  };
  D3D9GtaSaFloatConstantRange psFloatRanges[] = {
    { 12u, 2u, changedPsF },
  };
  D3D9GtaSaIntConstantRange psIntRanges[] = {
    { 3u, 1u, changedPsI },
  };
  D3D9GtaSaBoolConstantRange psBoolRanges[] = {
    { 4u, 1u, &changedPsB },
  };

  D3D9GtaSaEffectStateBatch batch = { };
  batch.StructSize = sizeof(batch);
  batch.ApiVersion = D3D9_GTA_SA_COMPAT_API_VERSION;
  batch.Flags = D3D9_GTA_SA_EFFECT_STATE_HAS_MATERIAL
              | D3D9_GTA_SA_EFFECT_STATE_HAS_NPATCH_MODE
              | D3D9_GTA_SA_EFFECT_STATE_HAS_FVF
              | D3D9_GTA_SA_EFFECT_STATE_HAS_VERTEX_SHADER
              | D3D9_GTA_SA_EFFECT_STATE_HAS_PIXEL_SHADER;
  batch.TransformCount = 1u;
  batch.Transforms = transforms;
  batch.LightCount = 1u;
  batch.Lights = lights;
  batch.LightEnableCount = 1u;
  batch.LightEnables = lightEnables;
  batch.RenderStateCount = 3u;
  batch.RenderStates = renderStates;
  batch.TextureBindingCount = 1u;
  batch.TextureBindings = textures;
  batch.TextureStageStateCount = 1u;
  batch.TextureStageStates = textureStageStates;
  batch.SamplerStateCount = 2u;
  batch.SamplerStates = samplerStates;
  batch.Material = changedMaterial;
  batch.NPatchMode = 1.0f;
  batch.FVF = D3DFVF_XYZ | D3DFVF_TEX1;
  batch.VertexFloatRangeCount = 1u;
  batch.VertexFloatRanges = vsFloatRanges;
  batch.VertexIntRangeCount = 1u;
  batch.VertexIntRanges = vsIntRanges;
  batch.VertexBoolRangeCount = 1u;
  batch.VertexBoolRanges = vsBoolRanges;
  batch.PixelFloatRangeCount = 1u;
  batch.PixelFloatRanges = psFloatRanges;
  batch.PixelIntRangeCount = 1u;
  batch.PixelIntRanges = psIntRanges;
  batch.PixelBoolRangeCount = 1u;
  batch.PixelBoolRanges = psBoolRanges;

  D3D9GtaSaEffectStateBatch emptyBatch = { };
  emptyBatch.StructSize = sizeof(emptyBatch);
  emptyBatch.ApiVersion = D3D9_GTA_SA_COMPAT_API_VERSION;
  bool validationPassed = compat->SubmitEffectStateBatch(nullptr) == E_POINTER
    && SUCCEEDED(compat->SubmitEffectStateBatch(&emptyBatch));
  D3D9GtaSaEffectStateBatch invalidBatch = emptyBatch;
  invalidBatch.StructSize = sizeof(invalidBatch) - 1u;
  validationPassed = validationPassed
    && compat->SubmitEffectStateBatch(&invalidBatch) == D3DERR_INVALIDCALL;
  invalidBatch = emptyBatch;
  invalidBatch.ApiVersion = 4u;
  validationPassed = validationPassed
    && compat->SubmitEffectStateBatch(&invalidBatch) == D3DERR_INVALIDCALL;
  invalidBatch = emptyBatch;
  invalidBatch.Flags = 0x80000000u;
  validationPassed = validationPassed
    && compat->SubmitEffectStateBatch(&invalidBatch) == D3DERR_INVALIDCALL;
  invalidBatch = emptyBatch;
  invalidBatch.Reserved = 1u;
  validationPassed = validationPassed
    && compat->SubmitEffectStateBatch(&invalidBatch) == D3DERR_INVALIDCALL;
  invalidBatch = emptyBatch;
  invalidBatch.RenderStateCount = 1u;
  validationPassed = validationPassed
    && compat->SubmitEffectStateBatch(&invalidBatch) == D3DERR_INVALIDCALL;
  D3D9GtaSaTransformBinding invalidTransform = {
    static_cast<D3DTRANSFORMSTATETYPE>(0x7fffffffu), changedTransform,
  };
  invalidBatch = emptyBatch;
  invalidBatch.TransformCount = 1u;
  invalidBatch.Transforms = &invalidTransform;
  validationPassed = validationPassed
    && compat->SubmitEffectStateBatch(&invalidBatch) == D3DERR_INVALIDCALL;
  D3D9GtaSaFloatConstantRange invalidRange = { 255u, 2u, changedVsF };
  invalidBatch = emptyBatch;
  invalidBatch.VertexFloatRangeCount = 1u;
  invalidBatch.VertexFloatRanges = &invalidRange;
  validationPassed = validationPassed
    && compat->SubmitEffectStateBatch(&invalidBatch) == D3DERR_INVALIDCALL;

  float boundaryConstant[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
  D3D9GtaSaFloatConstantRange boundaryRanges[256] = { };
  for (UINT i = 0u; i < 256u; i++) {
    boundaryRanges[i] = { 255u, 1u, boundaryConstant };
  }
  D3D9GtaSaEffectStateBatch boundaryBatch = emptyBatch;
  boundaryBatch.VertexFloatRangeCount = 256u;
  boundaryBatch.VertexFloatRanges = boundaryRanges;
  validationPassed = validationPassed
    && SUCCEEDED(compat->SubmitEffectStateBatch(&boundaryBatch));
  boundaryBatch.VertexFloatRangeCount = 257u;
  validationPassed = validationPassed
    && compat->SubmitEffectStateBatch(&boundaryBatch) == D3DERR_INVALIDCALL;

  if (SUCCEEDED(result)) result = compat->BeginStateJournal();
  if (SUCCEEDED(result)) result = compat->SubmitEffectStateBatch(&batch);

  D3DMATRIX currentTransform = { };
  D3DMATERIAL9 currentMaterial = { };
  D3DLIGHT9 currentLight = { };
  BOOL currentLightEnable = FALSE;
  DWORD currentAlphaBlend = 0u;
  DWORD currentSrcBlend = 0u;
  DWORD currentDestBlend = 0u;
  DWORD currentTss = 0u;
  DWORD currentMinFilter = 0u;
  DWORD currentMagFilter = 0u;
  DWORD currentFvf = 0u;
  IDirect3DBaseTexture9* currentTexture = nullptr;
  float currentVsF[8] = { };
  int currentVsI[4] = { };
  BOOL currentVsB = FALSE;
  float currentPsF[8] = { };
  int currentPsI[4] = { };
  BOOL currentPsB = FALSE;

  if (SUCCEEDED(result)) result = device->GetTransform(D3DTS_WORLD, &currentTransform);
  if (SUCCEEDED(result)) result = device->GetMaterial(&currentMaterial);
  if (SUCCEEDED(result)) result = device->GetLight(0u, &currentLight);
  if (SUCCEEDED(result)) result = device->GetLightEnable(0u, &currentLightEnable);
  if (SUCCEEDED(result)) result = device->GetRenderState(D3DRS_ALPHABLENDENABLE, &currentAlphaBlend);
  if (SUCCEEDED(result)) result = device->GetRenderState(D3DRS_SRCBLEND, &currentSrcBlend);
  if (SUCCEEDED(result)) result = device->GetRenderState(D3DRS_DESTBLEND, &currentDestBlend);
  if (SUCCEEDED(result)) result = device->GetTexture(0u, &currentTexture);
  if (SUCCEEDED(result)) result = device->GetTextureStageState(0u, D3DTSS_COLOROP, &currentTss);
  if (SUCCEEDED(result)) result = device->GetSamplerState(0u, D3DSAMP_MINFILTER, &currentMinFilter);
  if (SUCCEEDED(result)) result = device->GetSamplerState(0u, D3DSAMP_MAGFILTER, &currentMagFilter);
  if (SUCCEEDED(result)) result = device->GetFVF(&currentFvf);
  if (SUCCEEDED(result)) result = device->GetVertexShaderConstantF(8u, currentVsF, 2u);
  if (SUCCEEDED(result)) result = device->GetVertexShaderConstantI(1u, currentVsI, 1u);
  if (SUCCEEDED(result)) result = device->GetVertexShaderConstantB(2u, &currentVsB, 1u);
  if (SUCCEEDED(result)) result = device->GetPixelShaderConstantF(12u, currentPsF, 2u);
  if (SUCCEEDED(result)) result = device->GetPixelShaderConstantI(3u, currentPsI, 1u);
  if (SUCCEEDED(result)) result = device->GetPixelShaderConstantB(4u, &currentPsB, 1u);

  const bool applied = SUCCEEDED(result)
    && NearlyEqual(currentTransform._41, changedTransform._41)
    && NearlyEqual(currentMaterial.Diffuse.r, changedMaterial.Diffuse.r)
    && NearlyEqual(currentLight.Diffuse.r, changedLight.Diffuse.r)
    && currentLightEnable != FALSE
    && currentAlphaBlend == TRUE
    && currentSrcBlend == D3DBLEND_SRCALPHA
    && currentDestBlend == D3DBLEND_INVSRCALPHA
    && currentTexture == textureB
    && currentTss == D3DTOP_SELECTARG1
    && currentMinFilter == D3DTEXF_LINEAR
    && currentMagFilter == D3DTEXF_LINEAR
    && NearlyEqual(device->GetNPatchMode(), 1.0f)
    && currentFvf == (D3DFVF_XYZ | D3DFVF_TEX1)
    && std::memcmp(currentVsF, changedVsF, sizeof(changedVsF)) == 0
    && std::memcmp(currentVsI, changedVsI, sizeof(changedVsI)) == 0
    && currentVsB != FALSE
    && std::memcmp(currentPsF, changedPsF, sizeof(changedPsF)) == 0
    && std::memcmp(currentPsI, changedPsI, sizeof(changedPsI)) == 0
    && currentPsB != FALSE;
  if (currentTexture) currentTexture->Release();

  HRESULT restoreResult = compat->RestoreStateJournal();
  currentTexture = nullptr;
  currentLightEnable = TRUE;
  if (SUCCEEDED(restoreResult)) restoreResult = device->GetTransform(D3DTS_WORLD, &currentTransform);
  if (SUCCEEDED(restoreResult)) restoreResult = device->GetMaterial(&currentMaterial);
  if (SUCCEEDED(restoreResult)) restoreResult = device->GetLight(0u, &currentLight);
  if (SUCCEEDED(restoreResult)) restoreResult = device->GetLightEnable(0u, &currentLightEnable);
  if (SUCCEEDED(restoreResult)) restoreResult = device->GetRenderState(D3DRS_ALPHABLENDENABLE, &currentAlphaBlend);
  if (SUCCEEDED(restoreResult)) restoreResult = device->GetRenderState(D3DRS_SRCBLEND, &currentSrcBlend);
  if (SUCCEEDED(restoreResult)) restoreResult = device->GetRenderState(D3DRS_DESTBLEND, &currentDestBlend);
  if (SUCCEEDED(restoreResult)) restoreResult = device->GetTexture(0u, &currentTexture);
  if (SUCCEEDED(restoreResult)) restoreResult = device->GetTextureStageState(0u, D3DTSS_COLOROP, &currentTss);
  if (SUCCEEDED(restoreResult)) restoreResult = device->GetSamplerState(0u, D3DSAMP_MINFILTER, &currentMinFilter);
  if (SUCCEEDED(restoreResult)) restoreResult = device->GetSamplerState(0u, D3DSAMP_MAGFILTER, &currentMagFilter);
  if (SUCCEEDED(restoreResult)) restoreResult = device->GetFVF(&currentFvf);
  if (SUCCEEDED(restoreResult)) restoreResult = device->GetVertexShaderConstantF(8u, currentVsF, 2u);
  if (SUCCEEDED(restoreResult)) restoreResult = device->GetVertexShaderConstantI(1u, currentVsI, 1u);
  if (SUCCEEDED(restoreResult)) restoreResult = device->GetVertexShaderConstantB(2u, &currentVsB, 1u);
  if (SUCCEEDED(restoreResult)) restoreResult = device->GetPixelShaderConstantF(12u, currentPsF, 2u);
  if (SUCCEEDED(restoreResult)) restoreResult = device->GetPixelShaderConstantI(3u, currentPsI, 1u);
  if (SUCCEEDED(restoreResult)) restoreResult = device->GetPixelShaderConstantB(4u, &currentPsB, 1u);

  const bool restored = SUCCEEDED(restoreResult)
    && NearlyEqual(currentTransform._41, initialTransform._41)
    && NearlyEqual(currentMaterial.Diffuse.r, initialMaterial.Diffuse.r)
    && NearlyEqual(currentLight.Diffuse.r, initialLight.Diffuse.r)
    && currentLightEnable == FALSE
    && currentAlphaBlend == FALSE
    && currentSrcBlend == D3DBLEND_ONE
    && currentDestBlend == D3DBLEND_ZERO
    && currentTexture == textureA
    && currentTss == D3DTOP_MODULATE
    && currentMinFilter == D3DTEXF_POINT
    && currentMagFilter == D3DTEXF_POINT
    && NearlyEqual(device->GetNPatchMode(), 0.0f)
    && currentFvf == (D3DFVF_XYZ | D3DFVF_DIFFUSE)
    && std::memcmp(currentVsF, initialVsF, sizeof(initialVsF)) == 0
    && std::memcmp(currentVsI, initialVsI, sizeof(initialVsI)) == 0
    && currentVsB == FALSE
    && std::memcmp(currentPsF, initialPsF, sizeof(initialPsF)) == 0
    && std::memcmp(currentPsI, initialPsI, sizeof(initialPsI)) == 0
    && currentPsB == FALSE;
  if (currentTexture) currentTexture->Release();

  IDirect3DVertexDeclaration9* restoredNullDeclaration = nullptr;
  HRESULT nullDeclarationResult = device->SetVertexDeclaration(nullptr);
  if (SUCCEEDED(nullDeclarationResult))
    nullDeclarationResult = compat->BeginStateJournal();
  if (SUCCEEDED(nullDeclarationResult))
    nullDeclarationResult = compat->SubmitEffectStateBatch(&batch);
  if (SUCCEEDED(nullDeclarationResult))
    nullDeclarationResult = compat->RestoreStateJournal();
  if (SUCCEEDED(nullDeclarationResult))
    nullDeclarationResult = device->GetVertexDeclaration(&restoredNullDeclaration);
  const bool nullDeclarationRestored = SUCCEEDED(nullDeclarationResult)
    && restoredNullDeclaration == nullptr;
  if (restoredNullDeclaration) restoredNullDeclaration->Release();

  constexpr UINT benchmarkIterations = 50000u;
  constexpr UINT redundantRepetitions = 8u;
  LARGE_INTEGER frequency = { };
  QueryPerformanceFrequency(&frequency);
  uint64_t directBegin = ReadQpc();
  for (UINT i = 0u; i < benchmarkIterations; i++) {
    const DWORD toggle = i & 1u;
    for (UINT repeat = 0u; repeat < redundantRepetitions; repeat++) {
      device->SetRenderState(D3DRS_ALPHABLENDENABLE, toggle);
      device->SetRenderState(D3DRS_SRCBLEND, toggle ? D3DBLEND_SRCALPHA : D3DBLEND_ONE);
      device->SetRenderState(D3DRS_DESTBLEND, toggle ? D3DBLEND_INVSRCALPHA : D3DBLEND_ZERO);
      device->SetTexture(0u, toggle ? textureB : textureA);
      device->SetTextureStageState(0u, D3DTSS_COLOROP,
        toggle ? D3DTOP_SELECTARG1 : D3DTOP_MODULATE);
      device->SetSamplerState(0u, D3DSAMP_MINFILTER,
        toggle ? D3DTEXF_LINEAR : D3DTEXF_POINT);
      device->SetSamplerState(0u, D3DSAMP_MAGFILTER,
        toggle ? D3DTEXF_LINEAR : D3DTEXF_POINT);
      device->SetVertexShaderConstantF(8u, toggle ? changedVsF : initialVsF, 2u);
      device->SetVertexShaderConstantI(1u, toggle ? changedVsI : initialVsI, 1u);
      device->SetVertexShaderConstantB(2u, toggle ? &changedVsB : &initialVsB, 1u);
      device->SetPixelShaderConstantF(12u, toggle ? changedPsF : initialPsF, 2u);
      device->SetPixelShaderConstantI(3u, toggle ? changedPsI : initialPsI, 1u);
      device->SetPixelShaderConstantB(4u, toggle ? &changedPsB : &initialPsB, 1u);
    }
  }
  uint64_t directEnd = ReadQpc();

  D3D9GtaSaEffectStateBatch benchmarkBatch = batch;
  benchmarkBatch.Flags = 0u;
  benchmarkBatch.TransformCount = 0u;
  benchmarkBatch.Transforms = nullptr;
  benchmarkBatch.LightCount = 0u;
  benchmarkBatch.Lights = nullptr;
  benchmarkBatch.LightEnableCount = 0u;
  benchmarkBatch.LightEnables = nullptr;
  uint64_t batchBegin = ReadQpc();
  HRESULT benchmarkResult = D3D_OK;
  for (UINT i = 0u; i < benchmarkIterations && SUCCEEDED(benchmarkResult); i++) {
    const DWORD toggle = i & 1u;
    renderStates[0].Value = toggle;
    renderStates[1].Value = toggle ? D3DBLEND_SRCALPHA : D3DBLEND_ONE;
    renderStates[2].Value = toggle ? D3DBLEND_INVSRCALPHA : D3DBLEND_ZERO;
    textures[0].Texture = toggle ? textureB : textureA;
    textureStageStates[0].Value = toggle ? D3DTOP_SELECTARG1 : D3DTOP_MODULATE;
    samplerStates[0].Value = toggle ? D3DTEXF_LINEAR : D3DTEXF_POINT;
    samplerStates[1].Value = toggle ? D3DTEXF_LINEAR : D3DTEXF_POINT;
    vsFloatRanges[0].Data = toggle ? changedVsF : initialVsF;
    vsIntRanges[0].Data = toggle ? changedVsI : initialVsI;
    vsBoolRanges[0].Data = toggle ? &changedVsB : &initialVsB;
    psFloatRanges[0].Data = toggle ? changedPsF : initialPsF;
    psIntRanges[0].Data = toggle ? changedPsI : initialPsI;
    psBoolRanges[0].Data = toggle ? &changedPsB : &initialPsB;
    benchmarkResult = compat->SubmitEffectStateBatch(&benchmarkBatch);
  }
  uint64_t batchEnd = ReadQpc();
  const double directNs = frequency.QuadPart
    ? 1.0e9 * double(directEnd - directBegin) / double(frequency.QuadPart) / benchmarkIterations
    : 0.0;
  const double batchNs = frequency.QuadPart
    ? 1.0e9 * double(batchEnd - batchBegin) / double(frequency.QuadPart) / benchmarkIterations
    : 0.0;

  renderStates[0].Value = TRUE;
  renderStates[1].Value = D3DBLEND_SRCALPHA;
  renderStates[2].Value = D3DBLEND_INVSRCALPHA;
  textures[0].Texture = textureB;
  textureStageStates[0].Value = D3DTOP_SELECTARG1;
  samplerStates[0].Value = D3DTEXF_LINEAR;
  samplerStates[1].Value = D3DTEXF_LINEAR;
  vsFloatRanges[0].Data = changedVsF;
  vsIntRanges[0].Data = changedVsI;
  vsBoolRanges[0].Data = &changedVsB;
  psFloatRanges[0].Data = changedPsF;
  psIntRanges[0].Data = changedPsI;
  psBoolRanges[0].Data = &changedPsB;

  device->SetTransform(D3DTS_WORLD, &initialTransform);
  device->SetMaterial(&initialMaterial);
  device->SetLight(0u, &initialLight);
  device->LightEnable(0u, FALSE);
  device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
  device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ZERO);
  device->SetTexture(0u, textureA);
  device->SetTextureStageState(0u, D3DTSS_COLOROP, D3DTOP_MODULATE);
  device->SetSamplerState(0u, D3DSAMP_MINFILTER, D3DTEXF_POINT);
  device->SetSamplerState(0u, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
  device->SetNPatchMode(0.0f);
  device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
  device->SetVertexShaderConstantF(8u, initialVsF, 2u);
  device->SetVertexShaderConstantI(1u, initialVsI, 1u);
  device->SetVertexShaderConstantB(2u, &initialVsB, 1u);
  device->SetPixelShaderConstantF(12u, initialPsF, 2u);
  device->SetPixelShaderConstantI(3u, initialPsI, 1u);
  device->SetPixelShaderConstantB(4u, &initialPsB, 1u);

  constexpr UINT journalIterations = 10000u;
  uint64_t journalBegin = ReadQpc();
  HRESULT journalBenchmarkResult = D3D_OK;
  for (UINT i = 0u; i < journalIterations && SUCCEEDED(journalBenchmarkResult); i++) {
    journalBenchmarkResult = compat->BeginStateJournal();
    if (SUCCEEDED(journalBenchmarkResult))
      journalBenchmarkResult = compat->SubmitEffectStateBatch(&batch);
    if (SUCCEEDED(journalBenchmarkResult))
      journalBenchmarkResult = compat->RestoreStateJournal();
  }
  uint64_t journalEnd = ReadQpc();
  const double journalNs = frequency.QuadPart
    ? 1.0e9 * double(journalEnd - journalBegin) / double(frequency.QuadPart) / journalIterations
    : 0.0;

  std::printf(
    "effect_batch_validation=%s applied=%s restored=%s null_decl_restored=%s result=0x%08lx restore=0x%08lx\n",
    validationPassed ? "yes" : "no",
    applied ? "yes" : "no",
    restored ? "yes" : "no",
    nullDeclarationRestored ? "yes" : "no",
    ULONG(result), ULONG(restoreResult));
  std::printf(
    "effect_batch_benchmark iterations=%u captured_calls=%u submitted_states=13 direct_ns=%.1f batch_ns=%.1f speedup=%.2fx result=0x%08lx\n",
    benchmarkIterations, 13u * redundantRepetitions, directNs, batchNs,
    batchNs > 0.0 ? directNs / batchNs : 0.0,
    ULONG(benchmarkResult));
  std::printf(
    "effect_journal_benchmark iterations=%u transaction_ns=%.1f result=0x%08lx\n",
    journalIterations, journalNs, ULONG(journalBenchmarkResult));

  device->SetTexture(0u, nullptr);
  textureA->Release();
  textureB->Release();
  return validationPassed && applied && restored && nullDeclarationRestored
      && SUCCEEDED(benchmarkResult) && SUCCEEDED(journalBenchmarkResult);
}


static bool RunUndefinedLightJournalProbe(
        IDirect3DDevice9*          device,
        ID3D9GtaSaCompatDevice3*  compat) {
  constexpr DWORD SetLightIndex = 63u;
  constexpr DWORD EnableLightIndex = 62u;

  D3DLIGHT9 light = { };
  light.Type = D3DLIGHT_DIRECTIONAL;
  light.Direction.z = 1.0f;

  D3DLIGHT9 queriedLight = { };
  BOOL queriedEnable = FALSE;
  const bool setStartsUndefined = FAILED(device->GetLight(SetLightIndex, &queriedLight));
  const bool enableStartsUndefined =
    FAILED(device->GetLight(EnableLightIndex, &queriedLight))
    && FAILED(device->GetLightEnable(EnableLightIndex, &queriedEnable));

  const HRESULT setBeginResult = compat->BeginStateJournal();
  HRESULT setResult = setBeginResult;
  if (SUCCEEDED(setResult))
    setResult = device->SetLight(SetLightIndex, &light);
  const HRESULT setRestoreResult = SUCCEEDED(setBeginResult)
    ? compat->RestoreStateJournal()
    : setBeginResult;
  const bool setRestoredUndefined =
    FAILED(device->GetLight(SetLightIndex, &queriedLight));

  const HRESULT enableBeginResult = compat->BeginStateJournal();
  HRESULT enableResult = enableBeginResult;
  if (SUCCEEDED(enableResult))
    enableResult = device->LightEnable(EnableLightIndex, TRUE);
  const HRESULT enableRestoreResult = SUCCEEDED(enableBeginResult)
    ? compat->RestoreStateJournal()
    : enableBeginResult;
  const bool enableRestoredUndefined =
    FAILED(device->GetLight(EnableLightIndex, &queriedLight))
    && FAILED(device->GetLightEnable(EnableLightIndex, &queriedEnable));

  const bool passed = setStartsUndefined && enableStartsUndefined
    && SUCCEEDED(setResult) && SUCCEEDED(setRestoreResult)
    && SUCCEEDED(enableResult) && SUCCEEDED(enableRestoreResult)
    && setRestoredUndefined && enableRestoredUndefined;
  std::printf(
    "journal_undefined_lights=%s set=0x%08lx set_restore=0x%08lx enable=0x%08lx enable_restore=0x%08lx\n",
    passed ? "yes" : "no",
    ULONG(setResult), ULONG(setRestoreResult),
    ULONG(enableResult), ULONG(enableRestoreResult));
  return passed;
}

int main(int argc, char** argv) {
  if (argc < 2 || argc > 3 || (argc == 3 && std::strcmp(argv[2], "--force") != 0)) {
    std::fprintf(stderr, "usage: gta_sa_compat_probe.exe <d3d9.dll> [--force]\n");
    return 2;
  }

  if (argc == 3) {
    SetEnvironmentVariableA(
      "DXVK_CONFIG",
      "d3d9.gtaSaCompat=True;d3d9.gtaSaCompatDiagnostics=True;d3d9.presentInterval=0");
  }

  HMODULE module = LoadLibraryA(argv[1]);
  if (module == nullptr) {
    std::fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError());
    return 3;
  }

  auto create9 = reinterpret_cast<Direct3DCreate9Proc>(
    GetProcAddress(module, "Direct3DCreate9"));
  if (create9 == nullptr) {
    std::fprintf(stderr, "Direct3DCreate9 export missing\n");
    FreeLibrary(module);
    return 4;
  }

  WNDCLASSA windowClass = { };
  windowClass.lpfnWndProc = ProbeWindowProc;
  windowClass.hInstance = GetModuleHandleA(nullptr);
  windowClass.lpszClassName = "DxvkGtaSaCompatProbe";
  RegisterClassA(&windowClass);

  HWND window = CreateWindowExA(
    0, windowClass.lpszClassName, "DXVK GTA SA probe", WS_OVERLAPPEDWINDOW,
    CW_USEDEFAULT, CW_USEDEFAULT, 640, 480,
    nullptr, nullptr, windowClass.hInstance, nullptr);
  if (window == nullptr) {
    std::fprintf(stderr, "CreateWindow failed: %lu\n", GetLastError());
    FreeLibrary(module);
    return 5;
  }

  IDirect3D9* d3d9 = create9(D3D_SDK_VERSION);
  if (d3d9 == nullptr) {
    std::fprintf(stderr, "Direct3DCreate9 returned null\n");
    DestroyWindow(window);
    FreeLibrary(module);
    return 6;
  }

  D3DPRESENT_PARAMETERS params = { };
  params.BackBufferWidth = 640;
  params.BackBufferHeight = 480;
  params.BackBufferFormat = D3DFMT_A8R8G8B8;
  params.BackBufferCount = 1;
  params.SwapEffect = D3DSWAPEFFECT_DISCARD;
  params.hDeviceWindow = window;
  params.Windowed = TRUE;
  params.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

  IDirect3DDevice9* device = nullptr;
  HRESULT result = d3d9->CreateDevice(
    D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
    D3DCREATE_HARDWARE_VERTEXPROCESSING,
    &params, &device);
  if (FAILED(result)) {
    std::fprintf(stderr, "CreateDevice failed: 0x%08lx\n", ULONG(result));
    d3d9->Release();
    DestroyWindow(window);
    FreeLibrary(module);
    return 7;
  }

  ID3D9GtaSaCompatDevice5* compat = nullptr;
  result = device->QueryInterface(
    __uuidof(ID3D9GtaSaCompatDevice5),
    reinterpret_cast<void**>(&compat));
  if (FAILED(result) || compat == nullptr) {
    std::fprintf(stderr, "GTA SA compatibility interface unavailable: 0x%08lx\n", ULONG(result));
    device->Release();
    d3d9->Release();
    DestroyWindow(window);
    FreeLibrary(module);
    return 8;
  }

  const bool effectStateBatchPassed = RunEffectStateBatchProbe(device, compat);
  const bool stateDrawBatchPassed = RunStateDrawBatchProbe(device, compat);
  const bool undefinedLightJournalPassed =
    RunUndefinedLightJournalProbe(device, compat);

  HRESULT resetJournalResult = compat->BeginStateJournal();
  if (SUCCEEDED(resetJournalResult)) {
    resetJournalResult = device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  }
  if (SUCCEEDED(resetJournalResult))
    resetJournalResult = device->Reset(&params);
  const HRESULT restoreAfterResetResult = compat->RestoreStateJournal();
  const bool resetJournalDiscardPassed = SUCCEEDED(resetJournalResult)
    && restoreAfterResetResult == D3DERR_INVALIDCALL;
  std::printf("journal_reset_discard=%s reset=0x%08lx restore=0x%08lx\n",
    resetJournalDiscardPassed ? "yes" : "no",
    ULONG(resetJournalResult), ULONG(restoreAfterResetResult));

  const float vertex0[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
  const float vertex13[4] = { 5.0f, 6.0f, 7.0f, 8.0f };
  const float pixel1[4] = { 9.0f, 10.0f, 11.0f, 12.0f };
  const float pixel137[4] = { 13.0f, 14.0f, 15.0f, 16.0f };
  const BOOL vertexBool = TRUE;
  const BOOL pixelBool = TRUE;
  IDirect3DTexture9* batchTexture = nullptr;
  HRESULT stateBatchResult = device->CreateTexture(
    4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
    &batchTexture, nullptr);

  D3D9GtaSaFloatConstantRange vertexFloatRanges[] = {
    { 0u, 1u, vertex0 },
    { 13u, 1u, vertex13 },
  };
  D3D9GtaSaBoolConstantRange vertexBoolRanges[] = {
    { 0u, 1u, &vertexBool },
  };
  D3D9GtaSaFloatConstantRange pixelFloatRanges[] = {
    { 1u, 1u, pixel1 },
    { 137u, 1u, pixel137 },
  };
  D3D9GtaSaBoolConstantRange pixelBoolRanges[] = {
    { 0u, 1u, &pixelBool },
  };
  D3D9GtaSaTextureBinding textureBindings[] = {
    { 0u, batchTexture },
  };
  D3D9GtaSaStateBatch stateBatch = { };
  stateBatch.StructSize = sizeof(stateBatch);
  stateBatch.ApiVersion = D3D9_GTA_SA_COMPAT_API_VERSION;
  stateBatch.VertexFloatRangeCount = 2u;
  stateBatch.VertexFloatRanges = vertexFloatRanges;
  stateBatch.VertexBoolRangeCount = 1u;
  stateBatch.VertexBoolRanges = vertexBoolRanges;
  stateBatch.PixelFloatRangeCount = 2u;
  stateBatch.PixelFloatRanges = pixelFloatRanges;
  stateBatch.PixelBoolRangeCount = 1u;
  stateBatch.PixelBoolRanges = pixelBoolRanges;
  stateBatch.TextureBindingCount = 1u;
  stateBatch.TextureBindings = textureBindings;

  if (SUCCEEDED(stateBatchResult))
    stateBatchResult = compat->SubmitStateBatch(&stateBatch);

  float readVertex0[4] = { };
  float readVertex13[4] = { };
  float readPixel1[4] = { };
  float readPixel137[4] = { };
  BOOL readVertexBool = FALSE;
  BOOL readPixelBool = FALSE;
  IDirect3DBaseTexture9* readTexture = nullptr;
  if (SUCCEEDED(stateBatchResult))
    stateBatchResult = device->GetVertexShaderConstantF(0u, readVertex0, 1u);
  if (SUCCEEDED(stateBatchResult))
    stateBatchResult = device->GetVertexShaderConstantF(13u, readVertex13, 1u);
  if (SUCCEEDED(stateBatchResult))
    stateBatchResult = device->GetVertexShaderConstantB(0u, &readVertexBool, 1u);
  if (SUCCEEDED(stateBatchResult))
    stateBatchResult = device->GetPixelShaderConstantF(1u, readPixel1, 1u);
  if (SUCCEEDED(stateBatchResult))
    stateBatchResult = device->GetPixelShaderConstantF(137u, readPixel137, 1u);
  if (SUCCEEDED(stateBatchResult))
    stateBatchResult = device->GetPixelShaderConstantB(0u, &readPixelBool, 1u);
  if (SUCCEEDED(stateBatchResult))
    stateBatchResult = device->GetTexture(0u, &readTexture);

  const bool stateBatchValuesMatch = SUCCEEDED(stateBatchResult)
    && std::memcmp(readVertex0, vertex0, sizeof(vertex0)) == 0
    && std::memcmp(readVertex13, vertex13, sizeof(vertex13)) == 0
    && std::memcmp(readPixel1, pixel1, sizeof(pixel1)) == 0
    && std::memcmp(readPixel137, pixel137, sizeof(pixel137)) == 0
    && readVertexBool == TRUE
    && readPixelBool == TRUE
    && readTexture == batchTexture;
  std::printf("state_batch=%s result=0x%08lx\n",
    stateBatchValuesMatch ? "yes" : "no", ULONG(stateBatchResult));
  if (readTexture != nullptr)
    readTexture->Release();
  device->SetTexture(0u, nullptr);
  if (batchTexture != nullptr)
    batchTexture->Release();

  struct ProbePassConfig {
    UINT Id;
    INT Priority;
  } passConfigs[] = {
    { 2u, 200 },
    { 1u, 100 },
    { 3u, 200 },
  };

  UINT64 passTokens[3] = { };
  HRESULT passRegisterResult = D3D_OK;

  for (UINT i = 0u; i < 3u; i++) {
    D3D9GtaSaVulkanPassDesc passDesc = { };
    passDesc.StructSize = sizeof(passDesc);
    passDesc.ApiVersion = D3D9_GTA_SA_COMPAT_API_VERSION;
    passDesc.Priority = passConfigs[i].Priority;
    passDesc.Stage = D3D9_GTA_SA_VULKAN_PASS_AFTER_BLIT;
    passDesc.Flags = D3D9_GTA_SA_VULKAN_PASS_RESTORES_LAYOUTS;
    std::snprintf(passDesc.Name, sizeof(passDesc.Name), "ProbeNoOp%u", passConfigs[i].Id);
    passDesc.Record = ProbeVulkanPass;
    passDesc.UserData = reinterpret_cast<void*>(uintptr_t(passConfigs[i].Id));

    passRegisterResult = compat->RegisterVulkanPass(&passDesc, &passTokens[i]);
    if (FAILED(passRegisterResult)) {
      std::fprintf(stderr, "RegisterVulkanPass %u failed: 0x%08lx\n",
        passConfigs[i].Id, ULONG(passRegisterResult));
      break;
    }
  }

  if (FAILED(passRegisterResult)) {
    for (UINT i = 0u; i < 3u; i++) {
      if (passTokens[i])
        compat->UnregisterVulkanPass(passTokens[i]);
    }
    compat->Release();
    device->Release();
    d3d9->Release();
    DestroyWindow(window);
    FreeLibrary(module);
    return 9;
  }

  device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(16, 32, 48), 1.0f, 0);
  result = device->Present(nullptr, nullptr, nullptr, nullptr);
  if (FAILED(result))
    std::fprintf(stderr, "Present failed: 0x%08lx\n", ULONG(result));

  HRESULT firstPresentSyncResult = WaitForDeviceWork(device);
  if (FAILED(firstPresentSyncResult))
    std::fprintf(stderr, "First Present synchronization failed: 0x%08lx\n", ULONG(firstPresentSyncResult));

  HRESULT resetResult = device->Reset(&params);
  if (FAILED(resetResult))
    std::fprintf(stderr, "Reset failed: 0x%08lx\n", ULONG(resetResult));

  if (SUCCEEDED(resetResult)) {
    device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(24, 48, 72), 1.0f, 0);
    result = device->Present(nullptr, nullptr, nullptr, nullptr);
    if (FAILED(result))
      std::fprintf(stderr, "Present after Reset failed: 0x%08lx\n", ULONG(result));
  }

  HRESULT secondPresentSyncResult = SUCCEEDED(resetResult)
    ? WaitForDeviceWork(device)
    : resetResult;
  if (FAILED(secondPresentSyncResult))
    std::fprintf(stderr, "Second Present synchronization failed: 0x%08lx\n", ULONG(secondPresentSyncResult));

  HRESULT syncResetResult = device->Reset(&params);
  if (FAILED(syncResetResult))
    std::fprintf(stderr, "Final synchronization Reset failed: 0x%08lx\n", ULONG(syncResetResult));

  D3D9GtaSaCompatStatus status = { };
  status.StructSize = sizeof(status);
  result = compat->GetStatus(&status);
  if (FAILED(result)) {
    std::fprintf(stderr, "GetStatus failed: 0x%08lx\n", ULONG(result));
  } else {
    std::printf(
      "api=%u flags=0x%08x backbuffer=%ux%u format=%u presents=%llu resets=%llu failed_resets=%llu\n",
      status.ApiVersion, status.Flags,
      status.BackBufferWidth, status.BackBufferHeight, UINT(status.BackBufferFormat),
      static_cast<unsigned long long>(status.PresentCount),
      static_cast<unsigned long long>(status.ResetCount),
      static_cast<unsigned long long>(status.FailedResetCount));
  }

  UINT passCalls = g_vulkanPassCalls.load();
  UINT64 passOrder = g_vulkanPassOrder.load();
  std::printf("vulkan_pass_calls=%u order=%llu tokens=%llu,%llu,%llu\n",
    passCalls,
    static_cast<unsigned long long>(passOrder),
    static_cast<unsigned long long>(passTokens[0]),
    static_cast<unsigned long long>(passTokens[1]),
    static_cast<unsigned long long>(passTokens[2]));

  HRESULT passUnregisterResult = D3D_OK;
  for (UINT i = 0u; i < 3u; i++) {
    HRESULT unregisterResult = compat->UnregisterVulkanPass(passTokens[i]);
    if (FAILED(unregisterResult)) {
      passUnregisterResult = unregisterResult;
      std::fprintf(stderr, "UnregisterVulkanPass failed: 0x%08lx\n", ULONG(unregisterResult));
    }
  }

  g_slowPassStarted = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  g_slowPassFinished = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  UINT64 slowPassToken = 0;
  HRESULT slowPassResult = g_slowPassStarted && g_slowPassFinished
    ? D3D_OK
    : HRESULT_FROM_WIN32(GetLastError());
  DWORD slowPassWait = WAIT_FAILED;
  DWORD slowPassUnregisterMs = 0;
  bool slowPassFinished = false;

  if (SUCCEEDED(slowPassResult)) {
    D3D9GtaSaVulkanPassDesc slowPassDesc = { };
    slowPassDesc.StructSize = sizeof(slowPassDesc);
    slowPassDesc.ApiVersion = D3D9_GTA_SA_COMPAT_API_VERSION;
    slowPassDesc.Priority = 0;
    slowPassDesc.Stage = D3D9_GTA_SA_VULKAN_PASS_AFTER_BLIT;
    slowPassDesc.Flags = D3D9_GTA_SA_VULKAN_PASS_RESTORES_LAYOUTS;
    std::snprintf(slowPassDesc.Name, sizeof(slowPassDesc.Name), "ProbeSlowUnregister");
    slowPassDesc.Record = SlowProbeVulkanPass;

    slowPassResult = compat->RegisterVulkanPass(&slowPassDesc, &slowPassToken);
  }

  if (SUCCEEDED(slowPassResult)) {
    slowPassResult = device->Present(nullptr, nullptr, nullptr, nullptr);
  }

  if (SUCCEEDED(slowPassResult)) {
    slowPassWait = WaitForSingleObject(g_slowPassStarted, 5000);
    if (slowPassWait == WAIT_OBJECT_0) {
      DWORD start = GetTickCount();
      slowPassResult = compat->UnregisterVulkanPass(slowPassToken);
      slowPassUnregisterMs = GetTickCount() - start;
      slowPassFinished = WaitForSingleObject(g_slowPassFinished, 0) == WAIT_OBJECT_0;
      slowPassToken = 0;
    } else {
      slowPassResult = HRESULT_FROM_WIN32(
        slowPassWait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError());
    }
  }

  HRESULT slowPassSyncResult = SUCCEEDED(slowPassResult)
    ? WaitForDeviceWork(device)
    : slowPassResult;
  std::printf("slow_unregister=%s elapsed_ms=%lu callback_finished=%s\n",
    SUCCEEDED(slowPassResult) && slowPassUnregisterMs >= 250u ? "yes" : "no",
    static_cast<unsigned long>(slowPassUnregisterMs),
    slowPassFinished ? "yes" : "no");

  if (slowPassToken)
    compat->UnregisterVulkanPass(slowPassToken);
  if (g_slowPassStarted)
    CloseHandle(g_slowPassStarted);
  if (g_slowPassFinished)
    CloseHandle(g_slowPassFinished);
  g_slowPassStarted = nullptr;
  g_slowPassFinished = nullptr;

  ID3D9VkInteropDevice* interop = nullptr;
  HRESULT interopResult = compat->GetVulkanInterop(&interop);
  std::printf("vulkan_interop=%s result=0x%08lx\n",
    SUCCEEDED(interopResult) && interop != nullptr ? "yes" : "no",
    ULONG(interopResult));
  if (interop != nullptr)
    reinterpret_cast<IUnknown*>(interop)->Release();

  compat->Release();
  device->Release();
  d3d9->Release();
  DestroyWindow(window);
  UnregisterClassA(windowClass.lpszClassName, windowClass.hInstance);
  FreeLibrary(module);

  return FAILED(result)
      || FAILED(firstPresentSyncResult)
      || FAILED(secondPresentSyncResult)
      || FAILED(resetResult)
      || FAILED(syncResetResult)
      || FAILED(interopResult)
      || FAILED(passRegisterResult)
      || FAILED(passUnregisterResult)
      || FAILED(slowPassResult)
      || FAILED(slowPassSyncResult)
      || FAILED(stateBatchResult)
      || !stateBatchValuesMatch
      || !effectStateBatchPassed
      || !stateDrawBatchPassed
      || !undefinedLightJournalPassed
      || !resetJournalDiscardPassed
      || slowPassUnregisterMs < 250u
      || !slowPassFinished
      || passCalls != 6u
      || passOrder != 123123u ? 1 : 0;
}
