// Public D3D9 calls on an actual DXVK/Vulkan device. Timing is reported, never
// used as a pass/fail threshold. Run baseline/candidate in alternating processes.
#include "test_check.hpp"
#include <d3d9.h>
#include <d3dcompiler.h>
#include <sa_renderstack/backend_api.h>
#include <array>
#include <algorithm>
#include <charconv>
#include <cstring>
#include <string>
#include <vector>
#include <random>

template<class T> struct Ptr {
  T* p{};
  Ptr() = default;
  Ptr(const Ptr&) = delete;
  Ptr& operator=(const Ptr&) = delete;
  void reset() { if (p) p->Release(); p = nullptr; }
  ~Ptr() { reset(); }
  T* operator->() const { return p; }
};

#define HR(call) do { HRESULT hrResult = (call); if (FAILED(hrResult)) { \
  std::fprintf(stderr, "FAIL %s: %08lx\n", #call, static_cast<unsigned long>(hrResult)); return false; } } while (false)

struct Fixture {
  HMODULE module{};
  HWND window{};
  Ptr<IDirect3D9> d3d;
  Ptr<IDirect3DDevice9> dev;
  Ptr<IDirect3DVertexShader9> vs;
  Ptr<IDirect3DPixelShader9> ps;
  Ptr<IDirect3DVertexBuffer9> vb;
  Ptr<IDirect3DQuery9> fence;
  std::array<Ptr<IDirect3DTexture9>, 2> textures;
  ~Fixture() {
    for (auto& texture : textures) texture.reset();
    fence.reset(); vb.reset(); ps.reset(); vs.reset(); dev.reset(); d3d.reset();
    if (window) DestroyWindow(window);
    if (module) FreeLibrary(module);
  }
  bool open(const char* path, bool dedup) {
    const std::string config = std::string("d3d9.gtaSaCompat=True;d3d9.gtaSaCompatDiagnostics=False;")
      + "dxvk.hud=;d3d9.presentInterval=0;d3d9.gtaSaConstantDedup=" + (dedup ? "True" : "False")
      + ";d3d9.gtaSaCoalesceSamplerBindings=" + (dedup ? "True" : "False")
      + ";d3d9.gtaSaCoalesceTextureBindings=" + (dedup ? "True" : "False")
      + ";d3d9.gtaSaResourceBindingCache=" + (dedup ? "True" : "False");
    SetEnvironmentVariableA("DXVK_CONFIG", config.c_str());
    SetEnvironmentVariableA("DXVK_CONFIG_FILE", "constant-benchmark-no-external.conf");
    SetEnvironmentVariableA("DXVK_LOG_LEVEL", "error");
    module = LoadLibraryA(path);
    if (!module) return false;
    auto create = reinterpret_cast<IDirect3D9* (WINAPI*)(UINT)>(GetProcAddress(module, "Direct3DCreate9"));
    if (!create || !(d3d.p = create(D3D_SDK_VERSION))) return false;
    WNDCLASSA wc{}; wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "RenderStackConstantBenchmark";
    RegisterClassA(&wc);
    window = CreateWindowA(wc.lpszClassName, wc.lpszClassName, 0, 0, 0, 64, 64,
      nullptr, nullptr, wc.hInstance, nullptr);
    if (!window) return false;
    D3DPRESENT_PARAMETERS pp{};
    pp.BackBufferWidth = pp.BackBufferHeight = 64;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8; pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = window;
    pp.Windowed = TRUE; pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    HR(d3d->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev.p));
    HR(dev->CreateQuery(D3DQUERYTYPE_EVENT, &fence.p));
    return true;
  }
  bool wait() {
    HR(fence->Issue(D3DISSUE_END));
    const ULONGLONG end = GetTickCount64() + 30000;
    HRESULT hr;
    while ((hr = fence->GetData(nullptr, 0, D3DGETDATA_FLUSH)) == S_FALSE) {
      if (GetTickCount64() > end) return false;
      SwitchToThread();
    }
    HR(hr);
    return true;
  }
};

long long ticks() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
double ns(long long t, UINT n) {
  LARGE_INTEGER f; QueryPerformanceFrequency(&f); return double(t) * 1e9 / double(f.QuadPart) / n;
}

bool setters(Fixture& f) {
  constexpr UINT iterations = 300000;
  for (bool pixel : {false, true}) for (bool ints : {false, true}) {
    const UINT limit = ints ? 16 : (pixel ? 224 : 256);
    for (UINT count : {1u, 4u, 16u, 64u, 224u, 256u}) {
      if (count > limit) continue;
      for (const char* pattern : {"same", "first", "tail", "all"}) {
        std::vector<float> a(count * 4, 0.25f), b = a;
        if (!std::strcmp(pattern, "first")) b[0] = 0.75f;
        if (!std::strcmp(pattern, "tail")) b[(count - 1) * 4] = 0.75f;
        if (!std::strcmp(pattern, "all")) for (auto& x : b) x = 0.75f;
        // int inputs use their own typed storage (no float/int aliasing).
        std::vector<int> ia(count * 4, 1), ib = ia;
        for (UINT i = 0; i < count * 4; ++i) if (a[i] != b[i]) ib[i] = 2;
        auto submit = [&](UINT i) {
          if (ints) return pixel ? f.dev->SetPixelShaderConstantI(0, (i & 1) ? ia.data() : ib.data(), count)
                                 : f.dev->SetVertexShaderConstantI(0, (i & 1) ? ia.data() : ib.data(), count);
          return pixel ? f.dev->SetPixelShaderConstantF(0, (i & 1) ? a.data() : b.data(), count)
                       : f.dev->SetVertexShaderConstantF(0, (i & 1) ? a.data() : b.data(), count);
        };
        for (UINT i = 0; i < 3000; ++i) HR(submit(i));
        HRESULT status = D3D_OK;
        const auto begin = ticks();
        for (UINT i = 0; i < iterations; ++i) status |= submit(i);
        const auto end = ticks();
        HR(status);
        std::printf("setter,%s,%s,%u,%s,%u,%.3f\n", pixel ? "ps" : "vs", ints ? "int" : "float",
          count, pattern, iterations, ns(end - begin, iterations));
      }
    }
  }
  return true;
}

bool draw_setup(Fixture& f) {
  f.vs.reset(); f.ps.reset(); f.vb.reset();
  for (auto& texture : f.textures) texture.reset();
  const char* vs = "float4 tint:register(c3);struct O{float4 p:POSITION;float4 c:COLOR0;};"
    "O main(float4 p:POSITION){O o;o.p=p;o.c=tint;return o;}";
  const char* ps = "float4 tint:register(c3);sampler2D tex:register(s0);sampler2D second:register(s5);"
    "float4 main(float4 c:COLOR0):COLOR{return c*tint*(0.5*tex2D(tex,float2(0.5,0.5))"
    "+0.5*tex2D(second,float2(0.5,0.5)));}";
  Ptr<ID3DBlob> vcode, pcode, errors;
  HR(D3DCompile(vs, std::strlen(vs), nullptr, nullptr, nullptr, "main", "vs_3_0", 0, 0, &vcode.p, &errors.p));
  HR(D3DCompile(ps, std::strlen(ps), nullptr, nullptr, nullptr, "main", "ps_3_0", 0, 0, &pcode.p, nullptr));
  HR(f.dev->CreateVertexShader(static_cast<const DWORD*>(vcode->GetBufferPointer()), &f.vs.p));
  HR(f.dev->CreatePixelShader(static_cast<const DWORD*>(pcode->GetBufferPointer()), &f.ps.p));
  HR(f.dev->SetVertexShader(f.vs.p)); HR(f.dev->SetPixelShader(f.ps.p));
  HR(f.dev->SetFVF(D3DFVF_XYZ));
  const float vertices[] = {-1,-1,0, -1,3,0, 3,-1,0};
  HR(f.dev->CreateVertexBuffer(sizeof(vertices), 0, D3DFVF_XYZ, D3DPOOL_MANAGED, &f.vb.p, nullptr));
  void* data = nullptr; HR(f.vb->Lock(0, sizeof(vertices), &data, 0));
  std::memcpy(data, vertices, sizeof(vertices)); HR(f.vb->Unlock());
  HR(f.dev->SetStreamSource(0, f.vb.p, 0, 12));
  HR(f.dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
  HR(f.dev->SetRenderState(D3DRS_ZENABLE, FALSE));
  HR(f.dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE));
  for (auto& texture : f.textures) {
    HR(f.dev->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture.p, nullptr));
    D3DLOCKED_RECT lock{}; HR(texture->LockRect(0, &lock, nullptr, 0));
    for (UINT y = 0; y < 2; ++y) for (UINT x = 0; x < 2; ++x) {
      const DWORD white = 0xffffffff;
      std::memcpy(static_cast<char*>(lock.pBits) + y * lock.Pitch + x * 4, &white, 4);
    }
    HR(texture->UnlockRect(0));
  }
  HR(f.dev->SetTexture(0, f.textures[0].p));
  HR(f.dev->SetTexture(5, f.textures[1].p));
  HR(f.dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT));
  HR(f.dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
  return true;
}

bool pixel_check(Fixture& f) {
  // Setters are followed by actual draws, synchronization and pixel readback.
  // Exercise a changed tail after an equal prefix, then alternate both stages.
  Ptr<IDirect3DSurface9> target, readback;
  HR(f.dev->GetRenderTarget(0, &target.p));
  HR(f.dev->CreateOffscreenPlainSurface(64, 64, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &readback.p, nullptr));
  for (bool vertex : {false, true}) for (UINT frame = 0; frame < 4; ++frame) {
    std::array<float,16> white{}, color{};
    for (UINT k = 12; k < 16; ++k) white[k] = 1.0f;
    color[12 + (frame & 1 ? 2 : 0)] = color[15] = 1.0f;
    HR(f.dev->SetVertexShaderConstantF(0, vertex ? color.data() : white.data(), 4));
    HR(f.dev->SetPixelShaderConstantF(0, vertex ? white.data() : color.data(), 4));
    HR(f.dev->BeginScene()); HR(f.dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1)); HR(f.dev->EndScene());
    if (!f.wait()) return false;
    HR(f.dev->GetRenderTargetData(target.p, readback.p));
    D3DLOCKED_RECT lock{}; HR(readback->LockRect(&lock, nullptr, D3DLOCK_READONLY));
    DWORD actual; std::memcpy(&actual, static_cast<const char*>(lock.pBits) + 32 * lock.Pitch + 32 * 4, 4);
    HR(readback->UnlockRect());
    const DWORD expected = frame & 1 ? 0xff0000ff : 0xffff0000;
    CHECK(actual == expected);
  }
  return true;
}

bool binding_pixel_check(Fixture& f) {
  Ptr<IDirect3DSurface9> target, readback;
  HR(f.dev->GetRenderTarget(0, &target.p));
  HR(f.dev->CreateOffscreenPlainSurface(64, 64, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &readback.p, nullptr));
  std::array<Ptr<IDirect3DTexture9>,3> images;
  for (UINT i = 0; i < 3; ++i) {
    HR(f.dev->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &images[i].p, nullptr));
    D3DLOCKED_RECT lock{}; HR(images[i]->LockRect(0, &lock, nullptr, 0));
    for (UINT y = 0; y < 2; ++y) for (UINT x = 0; x < 2; ++x) {
      const DWORD color = i == 0 || (i == 2 && x == 0) ? 0xffff0000 : 0xff0000ff;
      std::memcpy(static_cast<char*>(lock.pBits) + y * lock.Pitch + x * 4, &color, 4);
    }
    HR(images[i]->UnlockRect(0));
  }
  auto shader = [&](const std::string& source) -> bool {
    Ptr<ID3DBlob> code;
    Ptr<IDirect3DPixelShader9> compiled;
    HR(D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr, "main", "ps_3_0", 0, 0, &code.p, nullptr));
    HR(f.dev->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &compiled.p));
    HR(f.dev->SetPixelShader(compiled.p));
    return true;
  };
  auto drawRead = [&](DWORD expected) -> bool {
    HR(f.dev->BeginScene()); HR(f.dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1)); HR(f.dev->EndScene());
    if (!f.wait()) return false;
    HR(f.dev->GetRenderTargetData(target.p, readback.p));
    D3DLOCKED_RECT lock{}; HR(readback->LockRect(&lock, nullptr, D3DLOCK_READONLY));
    DWORD actual; std::memcpy(&actual, static_cast<const char*>(lock.pBits) + 32 * lock.Pitch + 32 * 4, 4);
    HR(readback->UnlockRect());
    for (UINT shift : {0u, 8u, 16u, 24u})
      CHECK(std::abs(int((actual >> shift) & 255) - int((expected >> shift) & 255)) <= 1);
    return true;
  };
  for (UINT slot : {0u, 5u, 15u}) {
    const std::string prefix = "sampler2D tex:register(s" + std::to_string(slot) + ");";
    if (!shader(prefix + "float4 main():COLOR{return tex2D(tex,float2(0.5,0.5));}")) return false;
    HR(f.dev->SetSamplerState(slot, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP));
    HR(f.dev->SetSamplerState(slot, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP));
    HR(f.dev->SetSamplerState(slot, D3DSAMP_SRGBTEXTURE, FALSE));
    for (UINT frame = 0; frame < 4; ++frame) {
      HR(f.dev->SetTexture(slot, images[frame & 1].p));
      HR(f.dev->SetSamplerState(slot, D3DSAMP_MINFILTER, (frame & 1) ? D3DTEXF_POINT : D3DTEXF_LINEAR));
      HR(f.dev->SetSamplerState(slot, D3DSAMP_MAGFILTER, (frame & 1) ? D3DTEXF_POINT : D3DTEXF_LINEAR));
      if (!drawRead(frame & 1 ? 0xff0000ff : 0xffff0000)) return false;
    }
    HR(f.dev->SetTexture(slot, images[2].p));
    HR(f.dev->SetSamplerState(slot, D3DSAMP_MINFILTER, D3DTEXF_POINT));
    HR(f.dev->SetSamplerState(slot, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
    if (!drawRead(0xff0000ff)) return false;
    HR(f.dev->SetSamplerState(slot, D3DSAMP_MINFILTER, D3DTEXF_LINEAR));
    HR(f.dev->SetSamplerState(slot, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR));
    if (!drawRead(0xff800080)) return false;
    if (!shader(prefix + "float4 main():COLOR{return tex2D(tex,float2(2,2));}")) return false;
    HR(f.dev->SetSamplerState(slot, D3DSAMP_ADDRESSU, D3DTADDRESS_BORDER));
    HR(f.dev->SetSamplerState(slot, D3DSAMP_ADDRESSV, D3DTADDRESS_BORDER));
    HR(f.dev->SetSamplerState(slot, D3DSAMP_BORDERCOLOR, 0xff00ff00));
    if (!drawRead(0xff00ff00)) return false;
    HR(f.dev->SetSamplerState(slot, D3DSAMP_BORDERCOLOR, 0xffffff00));
    if (!drawRead(0xffffff00)) return false;
    HR(f.dev->SetTexture(slot, nullptr));
    Ptr<IDirect3DBaseTexture9> unbound;
    HR(f.dev->GetTexture(slot, &unbound.p)); CHECK(unbound.p == nullptr);
  }
  // Two dirty slots retain the general batch path; restoring a state block or
  // journal must also invalidate binding caches before the next draw.
  if (!shader("sampler2D a:register(s0);sampler2D b:register(s5);"
      "float4 main():COLOR{return 0.25*tex2D(a,float2(0.5,0.5))+0.75*tex2D(b,float2(0.5,0.5));}")) return false;
  for (UINT slot : {0u, 5u}) {
    HR(f.dev->SetSamplerState(slot, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP));
    HR(f.dev->SetSamplerState(slot, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP));
  }
  auto pair = [&](bool flip) -> bool {
    HR(f.dev->SetTexture(0, images[flip ? 1 : 0].p));
    HR(f.dev->SetTexture(5, images[flip ? 0 : 1].p));
    for (UINT slot : {0u, 5u})
      HR(f.dev->SetSamplerState(slot, D3DSAMP_MAGFILTER, flip ? D3DTEXF_POINT : D3DTEXF_LINEAR));
    return true;
  };
  if (!pair(false) || !drawRead(0xff4000bf)) return false;
  Ptr<IDirect3DStateBlock9> block;
  HR(f.dev->CreateStateBlock(D3DSBT_ALL, &block.p));
  if (!pair(true) || !drawRead(0xffbf0040)) return false;
  HR(block->Apply()); if (!drawRead(0xff4000bf)) return false;
  Ptr<ID3D9GtaSaCompatDevice6> api;
  HR(f.dev->QueryInterface(__uuidof(ID3D9GtaSaCompatDevice6), reinterpret_cast<void**>(&api.p)));
  HR(api->BeginStateJournal());
  if (!pair(true) || !drawRead(0xffbf0040)) return false;
  HR(api->RestoreStateJournal()); if (!drawRead(0xff4000bf)) return false;
  HR(f.dev->SetTexture(5, nullptr)); HR(f.dev->SetTexture(15, nullptr));
  // Restore the benchmark's shaders, streams and uniform white textures.
  return draw_setup(f);
}

bool texture_lifetime_pass(Fixture& f) {
  Ptr<IDirect3DSurface9> original, readback;
  HR(f.dev->GetRenderTarget(0, &original.p));
  HR(f.dev->CreateOffscreenPlainSurface(64, 64, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &readback.p, nullptr));
  auto image = [&](Ptr<IDirect3DTexture9>& texture, DWORD color) -> bool {
    HR(f.dev->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture.p, nullptr));
    D3DLOCKED_RECT lock{}; HR(texture->LockRect(0, &lock, nullptr, 0));
    for (UINT y = 0; y < 2; ++y) for (UINT x = 0; x < 2; ++x)
      std::memcpy(static_cast<char*>(lock.pBits) + y * lock.Pitch + x * 4, &color, 4);
    HR(texture->UnlockRect(0));
    return true;
  };
  auto pixel = [&](IDirect3DSurface9* target, DWORD expected) -> bool {
    HR(f.dev->GetRenderTargetData(target, readback.p));
    D3DLOCKED_RECT lock{}; HR(readback->LockRect(&lock, nullptr, D3DLOCK_READONLY));
    DWORD actual; std::memcpy(&actual, static_cast<const char*>(lock.pBits) + 32 * lock.Pitch + 32 * 4, 4);
    HR(readback->UnlockRect());
    for (UINT shift : {0u, 8u, 16u, 24u})
      CHECK(std::abs(int((actual >> shift) & 255) - int((expected >> shift) & 255)) <= 1);
    return true;
  };
  auto draw = [&]() -> bool {
    HR(f.dev->BeginScene()); HR(f.dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1)); HR(f.dev->EndScene());
    return true;
  };
  auto drawRead = [&](DWORD expected) -> bool {
    return draw() && f.wait() && pixel(original.p, expected);
  };
  for (bool multi : {false, true}) {
    std::printf("case=texture-lifetime-%s\n", multi ? "multi" : "single");
    const char* source = multi
      ? "sampler2D a:register(s0);sampler2D b:register(s5);float4 main():COLOR{return .25*tex2D(a,float2(.5,.5))+.75*tex2D(b,float2(.5,.5));}"
      : "sampler2D a:register(s0);float4 main():COLOR{return tex2D(a,float2(.5,.5));}";
    Ptr<ID3DBlob> code; Ptr<IDirect3DPixelShader9> shader;
    HR(D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, "main", "ps_3_0", 0, 0, &code.p, nullptr));
    HR(f.dev->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &shader.p));
    HR(f.dev->SetPixelShader(shader.p));
    HR(f.dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE));
    for (UINT slot : {0u, 5u}) {
      HR(f.dev->SetSamplerState(slot, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP));
      HR(f.dev->SetSamplerState(slot, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP));
      HR(f.dev->SetSamplerState(slot, D3DSAMP_MINFILTER, D3DTEXF_POINT));
      HR(f.dev->SetSamplerState(slot, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
      HR(f.dev->SetSamplerState(slot, D3DSAMP_SRGBTEXTURE, FALSE));
    }
    Ptr<IDirect3DTexture9> red, blue, gray;
    if (!image(red, 0xffff0000) || !image(blue, 0xff0000ff) || !image(gray, 0xff808080)) return false;
    auto pair = [&](bool flip) -> bool {
      HR(f.dev->SetTexture(0, flip ? blue.p : red.p));
      if (multi) HR(f.dev->SetTexture(5, flip ? red.p : blue.p));
      return true;
    };
    const DWORD first = multi ? 0xff4000bf : 0xffff0000;
    const DWORD second = multi ? 0xffbf0040 : 0xff0000ff;
    if (!pair(false) || !drawRead(first)) return false;
    // Dirty the slots then restore the original views without an intervening
    // draw: the resource cache can elide the rebind, while pixels must persist.
    if (!pair(true) || !pair(false) || !drawRead(first)) return false;
    if (multi) {
      // Exercise compacted pending entries with a hit before a miss, then a
      // miss before a hit. Distinct colors make an incorrect slot visible.
      HR(f.dev->SetTexture(0, blue.p)); HR(f.dev->SetTexture(0, red.p));
      HR(f.dev->SetTexture(5, gray.p));
      if (!drawRead(0xffa06060)) return false;
      HR(f.dev->SetTexture(0, blue.p));
      HR(f.dev->SetTexture(5, blue.p)); HR(f.dev->SetTexture(5, gray.p));
      if (!drawRead(0xff6060a0)) return false;
      if (!pair(false) || !drawRead(first)) return false;
    }
    Ptr<IDirect3DStateBlock9> block;
    HR(f.dev->CreateStateBlock(D3DSBT_ALL, &block.p));
    if (!pair(true) || !drawRead(second)) return false;
    HR(block->Apply()); if (!drawRead(first)) return false;
    Ptr<ID3D9GtaSaCompatDevice6> api;
    HR(f.dev->QueryInterface(__uuidof(ID3D9GtaSaCompatDevice6), reinterpret_cast<void**>(&api.p)));
    HR(api->BeginStateJournal());
    if (!pair(true) || !drawRead(second)) return false;
    HR(api->RestoreStateJournal()); if (!drawRead(first)) return false;
    HR(f.dev->SetTexture(0, gray.p));
    if (multi) HR(f.dev->SetTexture(5, gray.p));
    if (!drawRead(0xff808080)) return false;
    HR(f.dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, TRUE));
    if (multi) HR(f.dev->SetSamplerState(5, D3DSAMP_SRGBTEXTURE, TRUE));
    if (!drawRead(0xff373737)) return false;
    HR(f.dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE));
    if (multi) HR(f.dev->SetSamplerState(5, D3DSAMP_SRGBTEXTURE, FALSE));
    if (!drawRead(0xff808080)) return false;

    // Queue different draws to different targets, dropping each application's
    // texture references before the explicit completion fence. Later draws
    // replace earlier cached views, so the cache cannot keep all of them alive.
    std::array<Ptr<IDirect3DSurface9>,8> targets;
    for (UINT i = 0; i < targets.size(); ++i) {
      Ptr<IDirect3DTexture9> a, b;
      if (!image(a, i & 1 ? 0xff0000ff : 0xffff0000) ||
          !image(b, i & 1 ? 0xffff0000 : 0xff0000ff)) return false;
      HR(f.dev->CreateRenderTarget(64, 64, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &targets[i].p, nullptr));
      HR(f.dev->SetRenderTarget(0, targets[i].p));
      HR(f.dev->SetTexture(0, a.p));
      if (multi) HR(f.dev->SetTexture(5, b.p));
      if (!draw()) return false;
      HR(f.dev->SetTexture(0, nullptr));
      if (multi) HR(f.dev->SetTexture(5, nullptr));
      // a and b release at the end of this iteration, before the fence.
    }
    if (!f.wait()) return false;
    for (UINT i = 0; i < targets.size(); ++i)
      if (!pixel(targets[i].p, i & 1 ? second : first)) return false;
    HR(f.dev->SetRenderTarget(0, original.p));
    HR(f.dev->SetTexture(0, nullptr)); HR(f.dev->SetTexture(5, nullptr));
  }
  return true;
}

bool texture_lifetime_check(Fixture& f) {
  if (!texture_lifetime_pass(f) || !f.wait()) return false;
  f.fence.reset();
  D3DPRESENT_PARAMETERS pp{};
  pp.BackBufferWidth = pp.BackBufferHeight = 64;
  pp.BackBufferFormat = D3DFMT_A8R8G8B8; pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = f.window;
  pp.Windowed = TRUE; pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  HR(f.dev->Reset(&pp));
  HR(f.dev->CreateQuery(D3DQUERYTYPE_EVENT, &f.fence.p));
  if (!draw_setup(f)) return false;
  std::printf("case=texture-lifetime-after-reset\n");
  return texture_lifetime_pass(f) && f.wait();
}

struct BenchTiming {
  double cpuNs{}, completeNs{}, cycles{};
  DWORD cpuStart{}, cpuEnd{};
};

bool draw_benchmark(Fixture& f, UINT mode, BenchTiming* timing = nullptr) {
  constexpr UINT iterations = 16000;
  std::array<float,16> a{}, b{};
  for (UINT k = 12; k < 16; ++k) { a[k] = 0.25f; b[k] = 0.75f; }
  HR(f.dev->BeginScene());
  auto bind = [&](UINT i) -> bool {
    if (mode >= 5) {
      HR(f.dev->SetTexture(0, f.textures[1].p));
      HR(f.dev->SetTexture(0, f.textures[0].p));
      if (mode == 6) {
        HR(f.dev->SetTexture(5, f.textures[0].p));
        HR(f.dev->SetTexture(5, f.textures[1].p));
      }
      if (mode == 7) HR(f.dev->SetTexture(5, f.textures[i & 1].p));
      if (mode == 8) HR(f.dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, i & 1));
      return true;
    }
    if (mode == 1 || mode >= 3)
      HR(f.dev->SetTexture(0, f.textures[i & 1].p));
    if (mode >= 2)
      HR(f.dev->SetSamplerState(0, D3DSAMP_MAGFILTER, (i & 1) ? D3DTEXF_POINT : D3DTEXF_LINEAR));
    if (mode == 4) {
      HR(f.dev->SetTexture(5, f.textures[(i ^ 1) & 1].p));
      HR(f.dev->SetSamplerState(5, D3DSAMP_MAGFILTER, (i & 1) ? D3DTEXF_POINT : D3DTEXF_LINEAR));
    }
    return true;
  };
  for (UINT i = 0; i < 2000; ++i) {
    if (!bind(i)) return false;
    HR(f.dev->SetVertexShaderConstantF(0, (i & 1) ? a.data() : b.data(), 4));
    HR(f.dev->SetPixelShaderConstantF(0, (i & 1) ? a.data() : b.data(), 4));
    HR(f.dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1));
  }
  if (!f.wait()) return false;
  const DWORD cpuStart = GetCurrentProcessorNumber();
  ULONG64 cyclesStart, cyclesEnd;
  if (!QueryThreadCycleTime(GetCurrentThread(), &cyclesStart)) return false;
  const auto begin = ticks();
  for (UINT i = 0; i < iterations; ++i) {
    if (!bind(i)) return false;
    HR(f.dev->SetVertexShaderConstantF(0, (i & 1) ? a.data() : b.data(), 4));
    HR(f.dev->SetPixelShaderConstantF(0, (i & 1) ? a.data() : b.data(), 4));
    HR(f.dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1));
  }
  const auto submitted = ticks();
  if (!QueryThreadCycleTime(GetCurrentThread(), &cyclesEnd)) return false;
  const DWORD cpuEnd = GetCurrentProcessorNumber();
  if (!f.wait()) return false;
  const auto complete = ticks();
  HR(f.dev->EndScene());
  if (timing) {
    *timing = {ns(submitted - begin, iterations), ns(complete - begin, iterations),
      double(cyclesEnd - cyclesStart) / iterations, cpuStart, cpuEnd};
    return true;
  }
  const char* names[] = {"draw", "texture-draw", "sampler-draw", "bind-draw", "multi-bind-draw",
    "texture-hit-draw", "texture-multi-hit-draw", "texture-mixed-draw", "texture-srgb-draw"};
  std::printf("%s,vs+ps,float,4,tail,%u,%.3f\n", names[mode],
    iterations, ns(submitted - begin, iterations));
  std::printf("%s-complete,vs+ps,float,4,tail,%u,%.3f\n", names[mode],
    iterations, ns(complete - begin, iterations));
  return true;
}

int paired_benchmark(int argc, char** argv) {
  auto number = [](const char* text, UINT& value) {
    const auto end = text + std::strlen(text);
    const auto result = std::from_chars(text, end, value);
    return result.ec == std::errc{} && result.ptr == end;
  };
  UINT rounds = 0, seed = 0, cpu = 0;
  if (argc != 9 || (std::strcmp(argv[4], "on") && std::strcmp(argv[4], "off"))
      || !number(argv[5], rounds) || !rounds || rounds > 16 || !number(argv[6], seed)
      || (std::strcmp(argv[7], "any") && !number(argv[7], cpu))
      || (std::strcmp(argv[8], "controls") && std::strcmp(argv[8], "all") && std::strcmp(argv[8], "textures"))) {
    std::fprintf(stderr, "usage: ... pair <baseline> <candidate> on|off <rounds> <seed> any|cpu controls|all|textures\n");
    return 2;
  }
  struct CallerAffinity {
    DWORD_PTR previous{};
    ~CallerAffinity() { if (previous) SetThreadAffinityMask(GetCurrentThread(), previous); }
  } affinity;
  const bool pinned = std::strcmp(argv[7], "any") != 0;
  if (pinned) {
    if (cpu >= sizeof(DWORD_PTR) * 8 || cpu >= GetActiveProcessorCount(0)
        || !(affinity.previous = SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << cpu))) {
      std::fprintf(stderr, "FAIL caller-only diagnostic affinity, cpu=%u error=%lu\n", cpu, GetLastError());
      return 2;
    }
  }
  // A/A uses the exact same device. For distinct DLLs rotate factory creation
  // order across processes as well as the timed order within each process.
  Fixture a, b;
  Fixture* fixtures[] = {&a, &b};
  const bool shared = !_stricmp(argv[2], argv[3]);
  if (shared) fixtures[1] = &a;
  const bool enabled = !std::strcmp(argv[4], "on");
  if (shared) {
    if (!a.open(argv[2], enabled)) return 2;
  } else {
    const UINT first = seed & 1;
    if (!fixtures[first]->open(argv[2 + first], enabled)
        || !fixtures[first ^ 1]->open(argv[2 + (first ^ 1)], enabled)) return 2;
  }
  if (!draw_setup(a) || !pixel_check(a) || !binding_pixel_check(a) || !a.wait()) return 1;
  if (!shared && (!draw_setup(b) || !pixel_check(b) || !binding_pixel_check(b) || !b.wait())) return 1;
  std::printf("{\"type\":\"metadata\",\"rounds\":%u,\"seed\":%u,\"profile\":\"%s\","
    "\"sharedDevice\":%s,\"baselineBase\":\"%p\",\"candidateBase\":\"%p\",\"pinned\":%s}\n",
    rounds, seed, argv[4], shared ? "true" : "false", fixtures[0]->module, fixtures[1]->module,
    pinned ? "true" : "false");
  struct Workload {
    std::string name;
    bool pixel{}, ints{};
    UINT count{}, pattern{}, mode{};
  };
  std::vector<Workload> workloads;
  const bool all = !std::strcmp(argv[8], "all");
  const bool textures = !std::strcmp(argv[8], "textures");
  const char* patterns[] = {"same", "first", "tail", "all"};
  for (bool pixel : {false, true}) for (bool ints : {false, true}) {
    for (UINT count : {1u, 4u, 16u, 64u, 224u, 256u}) {
      if (count > (ints ? 16u : pixel ? 224u : 256u)) continue;
      for (UINT pattern = 0; pattern < 4; ++pattern) {
        const bool control = (!pattern && ((!pixel && !ints && count == 4)
          || (count == 16 && (pixel || ints)))) || (pixel && !ints && count == 64 && pattern == 3);
        if (textures || (!all && !control)) continue;
        const std::string name = std::string("setter,") + (pixel ? "ps," : "vs,")
          + (ints ? "int," : "float,") + std::to_string(count) + "," + patterns[pattern];
        workloads.push_back({name, pixel, ints, count, pattern, 0});
      }
    }
  }
  const char* draws[] = {"draw", "texture-draw", "sampler-draw", "bind-draw", "multi-bind-draw",
    "texture-hit-draw", "texture-multi-hit-draw", "texture-mixed-draw", "texture-srgb-draw"};
  for (UINT mode = 0; mode < 9u; ++mode) {
    if (textures ? (mode != 1 && mode != 4 && mode < 5) : mode >= (all ? 5u : 2u)) continue;
    workloads.push_back({std::string(draws[mode]) + ",vs+ps,float,4,tail", false, false, 0, 0, mode});
  }
  std::mt19937 random(seed);
  std::shuffle(workloads.begin(), workloads.end(), random);
  auto emit = [](const std::string& name, UINT variant, UINT round, UINT iterations,
                 const BenchTiming& t, bool complete) {
    std::printf("{\"type\":\"sample\",\"case\":\"%s\",\"variant\":\"%s\",\"round\":%u,"
      "\"iterations\":%u,\"ns\":%.4f,\"cycles\":%.4f,\"cpuStart\":%lu,\"cpuEnd\":%lu}\n",
      name.c_str(), variant ? "candidate" : "baseline", round, iterations,
      complete ? t.completeNs : t.cpuNs, t.cycles, t.cpuStart, t.cpuEnd);
  };
  for (const auto& w : workloads) {
    std::vector<float> fa(w.count * 4, 0.25f), fb = fa;
    std::vector<int> ia(w.count * 4, 1), ib = ia;
    for (UINT i = 0; i < w.count * 4; ++i) {
      const bool change = w.pattern == 3 || (w.pattern == 1 && i == 0)
        || (w.pattern == 2 && i == (w.count - 1) * 4);
      if (change) {fb[i] = 0.75f; ib[i] = 2;}
    }
    auto submit = [&](IDirect3DDevice9* dev, UINT i) {
      if (w.ints) return w.pixel ? dev->SetPixelShaderConstantI(0, (i & 1) ? ia.data() : ib.data(), w.count)
                               : dev->SetVertexShaderConstantI(0, (i & 1) ? ia.data() : ib.data(), w.count);
      return w.pixel ? dev->SetPixelShaderConstantF(0, (i & 1) ? fa.data() : fb.data(), w.count)
                     : dev->SetVertexShaderConstantF(0, (i & 1) ? fa.data() : fb.data(), w.count);
    };
    for (UINT round = 0; round < rounds; ++round) for (UINT order = 0; order < 2; ++order) {
      const UINT variant = order ^ ((round + seed) & 1);
      Fixture& f = *fixtures[variant];
      BenchTiming t;
      if (!w.count) {
        if (!draw_benchmark(f, w.mode, &t)) return 1;
        emit(w.name, variant, round, 16000, t, false);
        const std::string completed = std::string(draws[w.mode]) + "-complete,vs+ps,float,4,tail";
        emit(completed, variant, round, 16000, t, true);
        continue;
      }
      auto measure = [&]() -> bool {
        for (UINT i = 0; i < 10000; ++i) HR(submit(f.dev.p, i));
        ULONG64 beforeCycles, afterCycles;
        t.cpuStart = GetCurrentProcessorNumber();
        if (!QueryThreadCycleTime(GetCurrentThread(), &beforeCycles)) return false;
        const auto begin = ticks();
        HRESULT status = D3D_OK;
        for (UINT i = 0; i < 300000; ++i) status |= submit(f.dev.p, i);
        const auto end = ticks();
        if (!QueryThreadCycleTime(GetCurrentThread(), &afterCycles)) return false;
        t.cpuEnd = GetCurrentProcessorNumber();
        HR(status);
        t.cpuNs = ns(end - begin, 300000);
        t.cycles = double(afterCycles - beforeCycles) / 300000;
        return true;
      };
      if (!measure()) return 1;
      emit(w.name, variant, round, 300000, t, false);
    }
  }
  return api_test::finish();
}

int main(int argc, char** argv) {
  if (argc > 1 && !std::strcmp(argv[1], "pair")) return paired_benchmark(argc, argv);
  if (argc != 4 || (std::strcmp(argv[2], "on") && std::strcmp(argv[2], "off"))
      || (std::strcmp(argv[3], "check") && std::strcmp(argv[3], "bench") && std::strcmp(argv[3], "lifetime"))) {
    std::fprintf(stderr, "usage: constant-submit-benchmark <dll> on|off check|bench|lifetime\n"); return 2;
  }
  Fixture f;
  if (!f.open(argv[1], !std::strcmp(argv[2], "on"))) return 2;
  const bool bench = !std::strcmp(argv[3], "bench");
  if (bench && !setters(f)) return 1;
  if (!draw_setup(f) || !pixel_check(f) || !binding_pixel_check(f)) return 1;
  if (!std::strcmp(argv[3], "lifetime") && !texture_lifetime_check(f)) return 1;
  if (bench) for (UINT mode = 0; mode < 5; ++mode) if (!draw_benchmark(f, mode)) return 1;
  return api_test::finish();
}
