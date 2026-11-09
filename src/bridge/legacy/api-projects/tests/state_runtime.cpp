#include "test_check.hpp"
#include <api3_state_batch.hpp>
#include <api7_selective_journal.hpp>

#include <array>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include <type_traits>

namespace {
template <typename T> class ComPtr {
public:
  ComPtr() = default;
  ComPtr(const ComPtr&) = delete;
  ~ComPtr() { reset(); }
  void reset() { if (ptr_) ptr_->Release(); ptr_ = nullptr; }
  T** out() { reset(); return &ptr_; }
  T* get() const { return ptr_; }
  T* operator->() const { return ptr_; }
private:
  T* ptr_{};
};

struct Device {
  HMODULE module{};
  HWND window{};
  ComPtr<IDirect3D9> d3d;
  ComPtr<IDirect3DDevice9> device;
  ComPtr<ID3D9GtaSaCompatDevice6> api;
  D3DPRESENT_PARAMETERS params{};
  wchar_t configFile[MAX_PATH]{};

  ~Device() {
    api.reset(); device.reset(); d3d.reset();
    if (window) DestroyWindow(window);
    if (module) FreeLibrary(module);
    if (configFile[0]) DeleteFileW(configFile);
  }

  bool open(const char* backend, bool optimized) {
    const char* enabled = optimized ? "True" : "False";
    std::string config = std::string("d3d9.gtaSaCompat=True;d3d9.gtaSaCompatDiagnostics=False;")
      + "d3d9.presentInterval=0;dxvk.hud=;d3d9.gtaSaStateBlockPrefilter=" + enabled
      + ";d3d9.gtaSaStateBlockFastSkip=" + enabled
      + ";d3d9.gtaSaCoalesceSamplerBindings=" + enabled
      + ";d3d9.gtaSaCoalesceTextureBindings=" + enabled
      + ";d3d9.gtaSaResourceBindingCache=" + enabled
      + ";d3d9.gtaSaConstantDedup=" + enabled;
    // DXVK's Windows environment reader rejects values >= MAX_PATH. The full
    // profile exceeds that length, so use a unique, disposable config file.
    for (auto& c : config) if (c == ';') c = '\n';
    wchar_t tempDirectory[MAX_PATH];
    const DWORD tempLength = GetTempPathW(MAX_PATH, tempDirectory);
    if (!tempLength || tempLength >= MAX_PATH
        || !GetTempFileNameW(tempDirectory, L"rss", 0, configFile)) return false;
    HANDLE file = CreateFileW(configFile, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL saved = WriteFile(file, config.data(), DWORD(config.size()), &written, nullptr);
    CloseHandle(file);
    if (!saved || written != config.size()) return false;
    SetEnvironmentVariableA("DXVK_CONFIG", nullptr);
    SetEnvironmentVariableW(L"DXVK_CONFIG_FILE", configFile);
    SetEnvironmentVariableA("DXVK_LOG_LEVEL", "error");
    module = LoadLibraryA(backend);
    if (!module) return false;
    using Create9 = IDirect3D9* (WINAPI*)(UINT);
    const auto create = reinterpret_cast<Create9>(GetProcAddress(module, "Direct3DCreate9"));
    if (!create) return false;
    constexpr char name[] = "RenderStackStateCorrectness";
    WNDCLASSA wc{};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = name;
    if (!RegisterClassA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    window = CreateWindowA(name, name, 0, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    if (!window) return false;
    *d3d.out() = create(D3D_SDK_VERSION);
    if (!d3d.get()) return false;
    params.BackBufferWidth = params.BackBufferHeight = 64;
    params.BackBufferFormat = D3DFMT_A8R8G8B8;
    params.BackBufferCount = 1;
    params.SwapEffect = D3DSWAPEFFECT_DISCARD;
    params.hDeviceWindow = window;
    params.Windowed = TRUE;
    params.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    const HRESULT created = d3d->CreateDevice(0, D3DDEVTYPE_HAL, window,
      D3DCREATE_HARDWARE_VERTEXPROCESSING, &params, device.out());
    if (FAILED(created)) {
      std::fprintf(stderr, "FAIL CreateDevice hr=%08lx\n", static_cast<unsigned long>(created));
      return false;
    }
    const HRESULT queried = device->QueryInterface(__uuidof(ID3D9GtaSaCompatDevice6),
      reinterpret_cast<void**>(api.out()));
    if (FAILED(queried))
      std::fprintf(stderr, "FAIL compat QueryInterface hr=%08lx\n", static_cast<unsigned long>(queried));
    return SUCCEEDED(queried);
  }
};

// REQUIRE evaluates HRESULT-bearing expressions exactly once and still cleans
// scoped COM references/journals when a prerequisite unexpectedly fails.
#define REQUIRE(...) do { const bool passed = bool(__VA_ARGS__); \
  ::api_test::check(passed, #__VA_ARGS__, __FILE__, __LINE__); if (!passed) return; } while (false)

enum class Capture { Full, SelectiveOn, SelectiveOff };

class Journal {
public:
  Journal(ID3D9GtaSaCompatDevice6* api, Capture mode) : api_(api) {
    begin = mode == Capture::Full ? api_->BeginStateJournal() : api_->BeginSelectiveStateJournal();
    active_ = SUCCEEDED(begin);
    if (active_ && mode == Capture::SelectiveOn)
      begin = api_->SetStateJournalCaptureEnabled(TRUE);
  }
  ~Journal() { if (active_) (void)api_->RestoreStateJournal(); }
  HRESULT restore() { active_ = false; return api_->RestoreStateJournal(); }
  HRESULT begin{};
private:
  ID3D9GtaSaCompatDevice6* api_;
  bool active_{};
};

void stateblock_bools(Device& f, Capture mode, bool allStates) {
  std::printf("case=stateblock-bools capture=%u all=%u\n", unsigned(mode), unsigned(allStates));
  const std::array<BOOL, 16> saved{};
  std::array<BOOL, 16> baseline;
  baseline.fill(TRUE);
  ComPtr<IDirect3DStateBlock9> block;
  if (allStates) {
    REQUIRE(SUCCEEDED(f.device->SetVertexShaderConstantB(0, saved.data(), 16)));
    REQUIRE(SUCCEEDED(f.device->SetPixelShaderConstantB(0, saved.data(), 16)));
    REQUIRE(SUCCEEDED(f.device->CreateStateBlock(D3DSBT_ALL, block.out())));
  } else {
    REQUIRE(SUCCEEDED(f.device->BeginStateBlock()));
    REQUIRE(SUCCEEDED(f.device->SetVertexShaderConstantB(4, saved.data(), 3)));
    REQUIRE(SUCCEEDED(f.device->SetPixelShaderConstantB(4, saved.data(), 3)));
    REQUIRE(SUCCEEDED(f.device->EndStateBlock(block.out())));
  }
  REQUIRE(SUCCEEDED(f.device->SetVertexShaderConstantB(0, baseline.data(), 16)));
  REQUIRE(SUCCEEDED(f.device->SetPixelShaderConstantB(0, baseline.data(), 16)));
  Journal journal(f.api.get(), mode);
  REQUIRE(SUCCEEDED(journal.begin));
  REQUIRE(SUCCEEDED(block->Apply()));
  std::array<BOOL, 16> during{};
  REQUIRE(SUCCEEDED(f.device->GetVertexShaderConstantB(0, during.data(), 16)));
  CHECK(during[4] == FALSE);

  auto expected = baseline;
  if (mode == Capture::SelectiveOff) {
    if (allStates) expected = saved;
    else for (unsigned i = 4; i < 7; ++i) expected[i] = FALSE;
  }
  if (mode == Capture::SelectiveOn && !allStates) {
    REQUIRE(SUCCEEDED(f.api->SetStateJournalCaptureEnabled(FALSE)));
    REQUIRE(SUCCEEDED(f.device->SetVertexShaderConstantB(0, saved.data(), 1)));
    REQUIRE(SUCCEEDED(f.device->SetPixelShaderConstantB(0, saved.data(), 1)));
    expected[0] = FALSE; // masked capture must not restore unrelated bits
  }
  REQUIRE(SUCCEEDED(journal.restore()));
  for (bool pixel : {false, true}) {
    std::array<BOOL, 16> actual{};
    REQUIRE(SUCCEEDED(pixel ? f.device->GetPixelShaderConstantB(0, actual.data(), 16)
                           : f.device->GetVertexShaderConstantB(0, actual.data(), 16)));
    CHECK(actual == expected);
  }
}

void up_bindings(Device& f, Capture mode, bool indexed) {
  std::printf("case=up-bindings capture=%u indexed=%u\n", unsigned(mode), unsigned(indexed));
  ComPtr<IDirect3DVertexBuffer9> vb;
  ComPtr<IDirect3DIndexBuffer9> ib;
  REQUIRE(SUCCEEDED(f.device->CreateVertexBuffer(64, 0, D3DFVF_XYZ, D3DPOOL_MANAGED, vb.out(), nullptr)));
  REQUIRE(SUCCEEDED(f.device->CreateIndexBuffer(6, 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, ib.out(), nullptr)));
  REQUIRE(SUCCEEDED(f.device->SetVertexDeclaration(nullptr)));
  REQUIRE(SUCCEEDED(f.device->SetFVF(D3DFVF_XYZ)));
  REQUIRE(SUCCEEDED(f.device->SetStreamSource(0, vb.get(), 12, 12)));
  REQUIRE(SUCCEEDED(f.device->SetIndices(ib.get())));
  const float vertices[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
  const WORD indices[3] = {0, 1, 2};
  Journal journal(f.api.get(), mode);
  REQUIRE(SUCCEEDED(journal.begin));
  REQUIRE(SUCCEEDED(f.device->BeginScene()));
  const HRESULT draw = indexed
    ? f.device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 3, 1, indices, D3DFMT_INDEX16, vertices, 12)
    : f.device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, vertices, 12);
  CHECK(SUCCEEDED(draw));
  CHECK(SUCCEEDED(f.device->EndScene()));
  ComPtr<IDirect3DVertexBuffer9> stream;
  ComPtr<IDirect3DIndexBuffer9> index;
  UINT offset = 0, stride = 0;
  REQUIRE(SUCCEEDED(f.device->GetStreamSource(0, stream.out(), &offset, &stride)));
  CHECK(stream.get() == nullptr);
  REQUIRE(SUCCEEDED(journal.restore()));
  REQUIRE(SUCCEEDED(f.device->GetStreamSource(0, stream.out(), &offset, &stride)));
  REQUIRE(SUCCEEDED(f.device->GetIndices(index.out())));
  const bool capture = mode != Capture::SelectiveOff;
  CHECK(stream.get() == (capture ? vb.get() : nullptr));
  CHECK(offset == (capture ? 12u : 0u));
  CHECK(stride == (capture ? 12u : 0u));
  CHECK(index.get() == (capture || !indexed ? ib.get() : nullptr));
  CHECK(SUCCEEDED(f.device->SetStreamSource(0, nullptr, 0, 0)));
  CHECK(SUCCEEDED(f.device->SetIndices(nullptr)));
}

void render_target_side_effects(Device& f, Capture mode, bool sameTarget) {
  std::printf("case=render-target capture=%u same=%u\n", unsigned(mode), unsigned(sameTarget));
  ComPtr<IDirect3DSurface9> original, target, actualTarget;
  REQUIRE(SUCCEEDED(f.device->GetRenderTarget(0, original.out())));
  REQUIRE(SUCCEEDED(f.device->CreateRenderTarget(16, 16, D3DFMT_A8R8G8B8,
    D3DMULTISAMPLE_NONE, 0, FALSE, target.out(), nullptr)));
  const D3DVIEWPORT9 viewport{1, 2, 10, 11, 0.2f, 0.8f};
  const RECT scissor{2, 3, 8, 9};
  REQUIRE(SUCCEEDED(f.device->SetViewport(&viewport)));
  REQUIRE(SUCCEEDED(f.device->SetScissorRect(&scissor)));
  Journal journal(f.api.get(), mode);
  REQUIRE(SUCCEEDED(journal.begin));
  IDirect3DSurface9* selected = sameTarget ? original.get() : target.get();
  REQUIRE(SUCCEEDED(f.device->SetRenderTarget(0, selected)));
  REQUIRE(SUCCEEDED(journal.restore()));
  D3DVIEWPORT9 actualViewport{};
  RECT actualScissor{};
  REQUIRE(SUCCEEDED(f.device->GetViewport(&actualViewport)));
  REQUIRE(SUCCEEDED(f.device->GetScissorRect(&actualScissor)));
  REQUIRE(SUCCEEDED(f.device->GetRenderTarget(0, actualTarget.out())));
  CHECK(actualTarget.get() == selected); // RT objects remain outside the journal
  if (mode != Capture::SelectiveOff) {
    CHECK(std::memcmp(&actualViewport, &viewport, sizeof(viewport)) == 0);
    CHECK(EqualRect(&actualScissor, &scissor));
  } else {
    CHECK(actualViewport.X == 0 && actualViewport.Y == 0);
    CHECK(actualViewport.Width == (sameTarget ? 64u : 16u));
    CHECK(actualViewport.Height == (sameTarget ? 64u : 16u));
    CHECK(actualScissor.left == 0 && actualScissor.top == 0);
  }
  CHECK(SUCCEEDED(f.device->SetRenderTarget(0, original.get())));
}

void undefined_light_reapply(Device& f, Capture mode, DWORD lightIndex) {
  std::printf("case=undefined-light capture=%u\n", unsigned(mode));
  D3DLIGHT9 light{};
  light.Type = D3DLIGHT_POINT;
  light.Diffuse.r = light.Diffuse.g = light.Diffuse.b = light.Diffuse.a = 1;
  light.Range = 10;
  light.Attenuation0 = 1;
  D3DLIGHT9 actual{};
  REQUIRE(FAILED(f.device->GetLight(lightIndex, &actual)));
  Journal journal(f.api.get(), mode);
  REQUIRE(SUCCEEDED(journal.begin));
  REQUIRE(SUCCEEDED(f.device->SetLight(lightIndex, &light)));
  ComPtr<IDirect3DStateBlock9> block;
  REQUIRE(SUCCEEDED(f.device->CreateStateBlock(D3DSBT_ALL, block.out())));
  REQUIRE(SUCCEEDED(block->Apply()));
  REQUIRE(SUCCEEDED(journal.restore()));
  CHECK(FAILED(f.device->GetLight(lightIndex, &actual)));
  REQUIRE(SUCCEEDED(block->Apply()));
  CHECK(SUCCEEDED(f.device->GetLight(lightIndex, &actual)));
  CHECK(std::memcmp(&actual, &light, sizeof(light)) == 0);
}

void sdk_build_and_restore(Device& f) {
  std::puts("case=sdk-api3-submit-and-api7-raii");
  const std::array<float, 4> old{1, 2, 3, 4};
  const std::array<float, 8> newest{5, 6, 7, 8, 9, 10, 11, 12};
  api3::StateBatchBuilder builder;
  REQUIRE(builder.add_vertex_float(1, old).has_value());
  REQUIRE(builder.add_vertex_float(0, newest).has_value());
  REQUIRE(builder.add_pixel_float(1, old).has_value());
  REQUIRE(builder.add_pixel_float(0, newest).has_value());
  const std::array<float, 8> baseline{};
  REQUIRE(SUCCEEDED(f.device->SetVertexShaderConstantF(0, baseline.data(), 2)));
  REQUIRE(SUCCEEDED(f.device->SetPixelShaderConstantF(0, baseline.data(), 2)));
  {
    api7::SelectiveJournal original(f.api.get());
    REQUIRE(original.begin().has_value());
    REQUIRE(original.capture(true).has_value());
    api7::SelectiveJournal moved(std::move(original));
    CHECK(!original.capture(true));
    REQUIRE(SUCCEEDED(builder.submit(f.api.get())));
    std::array<float, 8> actual{};
    REQUIRE(SUCCEEDED(f.device->GetVertexShaderConstantF(0, actual.data(), 2)));
    CHECK(actual == newest);
    REQUIRE(SUCCEEDED(f.device->GetPixelShaderConstantF(0, actual.data(), 2)));
    CHECK(actual == newest);
    // No explicit restore: scope destruction must restore the real device.
  }
  std::array<float, 8> actual{};
  REQUIRE(SUCCEEDED(f.device->GetVertexShaderConstantF(0, actual.data(), 2)));
  CHECK(actual == baseline);
  REQUIRE(SUCCEEDED(f.device->GetPixelShaderConstantF(0, actual.data(), 2)));
  CHECK(actual == baseline);
  REQUIRE(SUCCEEDED(f.api->BeginStateJournal()));
  CHECK(SUCCEEDED(f.api->RestoreStateJournal()));
}

void batch_failure_semantics(Device& f) {
  std::puts("case=batch-validation-and-late-draw-failure");
  const float old[4] = {1, 2, 3, 4}, changed[4] = {5, 6, 7, 8};
  REQUIRE(SUCCEEDED(f.device->SetVertexShaderConstantF(0, old, 1)));
  D3D9GtaSaFloatConstantRange ranges[] = {{0, 1, changed}, {256, 1, changed}};
  D3D9GtaSaStateBatch batch{};
  batch.StructSize = sizeof(batch); batch.ApiVersion = 3;
  batch.VertexFloatRangeCount = 2; batch.VertexFloatRanges = ranges;
  CHECK(f.api->SubmitStateBatch(&batch) == D3DERR_INVALIDCALL);
  float actual[4]{};
  REQUIRE(SUCCEEDED(f.device->GetVertexShaderConstantF(0, actual, 1)));
  CHECK(std::memcmp(actual, old, sizeof(old)) == 0);

  batch.VertexFloatRangeCount = 1;
  REQUIRE(SUCCEEDED(f.device->SetVertexDeclaration(nullptr)));
  Journal journal(f.api.get(), Capture::Full);
  REQUIRE(SUCCEEDED(journal.begin));
  D3D9GtaSaStateDrawBatch draw{};
  draw.StructSize = sizeof(draw); draw.ApiVersion = 6; draw.StateBatch = &batch;
  draw.Draw = {sizeof(D3D9GtaSaDrawDesc), D3D9_GTA_SA_DRAW_PRIMITIVE,
    D3DPT_TRIANGLELIST, 0, 0, 0, 0, 0, 1};
  // Descriptor validation succeeds, then a missing declaration fails the draw.
  CHECK(f.api->SubmitStateDrawBatch(&draw) == D3DERR_INVALIDCALL);
  REQUIRE(SUCCEEDED(f.device->GetVertexShaderConstantF(0, actual, 1)));
  CHECK(std::memcmp(actual, changed, sizeof(changed)) == 0);
  REQUIRE(SUCCEEDED(journal.restore()));
  REQUIRE(SUCCEEDED(f.device->GetVertexShaderConstantF(0, actual, 1)));
  CHECK(std::memcmp(actual, old, sizeof(old)) == 0);
}

template<typename T> HRESULT constants(Device& f, bool pixel, bool get, UINT start, T* data, UINT count) {
  if constexpr (std::is_same_v<T, float>) {
    if (get) return pixel ? f.device->GetPixelShaderConstantF(start, data, count)
                          : f.device->GetVertexShaderConstantF(start, data, count);
    return pixel ? f.device->SetPixelShaderConstantF(start, data, count)
                 : f.device->SetVertexShaderConstantF(start, data, count);
  } else {
    if (get) return pixel ? f.device->GetPixelShaderConstantI(start, data, count)
                          : f.device->GetVertexShaderConstantI(start, data, count);
    return pixel ? f.device->SetPixelShaderConstantI(start, data, count)
                 : f.device->SetVertexShaderConstantI(start, data, count);
  }
}

template<typename T> void constant_roundtrip(Device& f, bool pixel) {
  const UINT limit = std::is_same_v<T, float> ? (pixel ? 224u : 256u) : 16u;
  // Include signed zero, distinct NaN payloads, infinities and subnormal bits.
  const uint32_t bits[] = {0, 0x80000000, 0x7fc00001, 0x7fc00002, 0x7f800000, 0xff800000, 1, 0xffffffff};
  for (UINT count : {1u, 2u, 3u, 4u, 5u, 16u, 64u, 224u, 256u}) {
    if (count > limit) continue;
    const UINT start = limit - count;
    std::vector<T> storage(count * 4 + 1), read(count * 4);
    T* data = storage.data() + 1; // deliberately not 16-byte aligned
    for (UINT k = 0; k < count * 4; ++k) std::memcpy(data + k, &bits[k % 8], sizeof(T));
    for (UINT changed : {0u, count / 2, count - 1}) {
      REQUIRE(SUCCEEDED(constants(f, pixel, false, start, data, count)));
      REQUIRE(SUCCEEDED(constants(f, pixel, false, start, data, count))); // unchanged
      uint32_t value; std::memcpy(&value, data + changed * 4, 4); value ^= 0x80000000;
      std::memcpy(data + changed * 4, &value, 4);
      REQUIRE(SUCCEEDED(constants(f, pixel, false, start, data, count)));
      REQUIRE(SUCCEEDED(constants(f, pixel, true, start, read.data(), count)));
      CHECK(std::memcmp(read.data(), data, count * sizeof(T) * 4) == 0);
    }
  }
  // Recording an equal range must still record it; applying it later restores
  // bytes through the state-block path, independent of the device fast path.
  std::array<T, 16> old{}, changed{}; changed[15] = T(3);
  REQUIRE(SUCCEEDED(constants(f, pixel, false, 0, old.data(), 4)));
  ComPtr<IDirect3DStateBlock9> block;
  REQUIRE(SUCCEEDED(f.device->BeginStateBlock()));
  REQUIRE(SUCCEEDED(constants(f, pixel, false, 0, old.data(), 4)));
  REQUIRE(SUCCEEDED(f.device->EndStateBlock(block.out())));
  REQUIRE(SUCCEEDED(constants(f, pixel, false, 0, changed.data(), 4)));
  REQUIRE(SUCCEEDED(block->Apply()));
  REQUIRE(SUCCEEDED(constants(f, pixel, true, 0, changed.data(), 4)));
  CHECK(std::memcmp(old.data(), changed.data(), sizeof(old)) == 0);

  for (Capture mode : {Capture::Full, Capture::SelectiveOn, Capture::SelectiveOff}) {
    REQUIRE(SUCCEEDED(constants(f, pixel, false, 0, old.data(), 4)));
    Journal journal(f.api.get(), mode); REQUIRE(SUCCEEDED(journal.begin));
    changed = old; changed[15] = T(9); // long equal prefix before first write
    REQUIRE(SUCCEEDED(constants(f, pixel, false, 0, changed.data(), 4)));
    changed[3] = T(7); // overlapping write must preserve the first old value
    REQUIRE(SUCCEEDED(constants(f, pixel, false, 0, changed.data(), 4)));
    REQUIRE(SUCCEEDED(journal.restore()));
    std::array<T, 16> actual{};
    REQUIRE(SUCCEEDED(constants(f, pixel, true, 0, actual.data(), 4)));
    const auto& expected = mode == Capture::SelectiveOff ? changed : old;
    CHECK(std::memcmp(actual.data(), expected.data(), sizeof(actual)) == 0);
  }
}

void unchanged_selective_capture(Device& f, bool dedup) {
  // Dedup-on ignores an equal write before capture is disabled. Dedup-off
  // preserves its eager snapshot. Neither mode may change across this refactor.
  for (bool pixel : {false, true}) {
    std::array<float,16> old{}, changed{}; changed[15] = 3;
    REQUIRE(SUCCEEDED(constants(f, pixel, false, 0, old.data(), 4)));
    Journal journal(f.api.get(), Capture::SelectiveOn);
    REQUIRE(SUCCEEDED(journal.begin));
    REQUIRE(SUCCEEDED(constants(f, pixel, false, 0, old.data(), 4)));
    REQUIRE(SUCCEEDED(f.api->SetStateJournalCaptureEnabled(FALSE)));
    REQUIRE(SUCCEEDED(constants(f, pixel, false, 0, changed.data(), 4)));
    REQUIRE(SUCCEEDED(journal.restore()));
    std::array<float,16> actual{};
    REQUIRE(SUCCEEDED(constants(f, pixel, true, 0, actual.data(), 4)));
    const auto& expected = dedup ? changed : old;
    CHECK(std::memcmp(actual.data(), expected.data(), sizeof(actual)) == 0);

    // A real tail change must snapshot the original full submitted range.
    // A later capture-off prefix write must also roll back from that snapshot.
    REQUIRE(SUCCEEDED(constants(f, pixel, false, 0, old.data(), 4)));
    Journal fullRange(f.api.get(), Capture::SelectiveOn);
    REQUIRE(SUCCEEDED(fullRange.begin));
    changed = old; changed[15] = 5;
    REQUIRE(SUCCEEDED(constants(f, pixel, false, 0, changed.data(), 4)));
    REQUIRE(SUCCEEDED(f.api->SetStateJournalCaptureEnabled(FALSE)));
    changed[0] = 7;
    REQUIRE(SUCCEEDED(constants(f, pixel, false, 0, changed.data(), 4)));
    REQUIRE(SUCCEEDED(fullRange.restore()));
    REQUIRE(SUCCEEDED(constants(f, pixel, true, 0, actual.data(), 4)));
    CHECK(std::memcmp(actual.data(), old.data(), sizeof(actual)) == 0);
  }
}

template<typename T> void constant_scan_boundaries(Device& f, bool pixel) {
  const UINT limit = std::is_same_v<T, float> ? (pixel ? 224u : 256u) : 16u;
  std::printf("case=constant-scan-boundaries-%s-%s\n", pixel ? "ps" : "vs",
    std::is_same_v<T, float> ? "float" : "int");
  // Exercise every possible first mismatch, both copy-loop parities, and
  // unchanged writes. Read the entire bank to detect writes past either end.
  for (UINT count : {1u, 2u, 3u, 4u, 5u, 7u, 14u, 16u}) {
    if (count + 2 > limit) continue;
    for (UINT first = 0; first < count; ++first) {
      std::vector<T> original(limit * 4, T(7)), expected = original, actual(limit * 4);
      std::vector<T> source(count * 4, T(7));
      for (UINT reg = first; reg < count; ++reg) for (UINT lane = 0; lane < 4; ++lane)
        source[reg * 4 + lane] = T(20 + reg * 4 + lane);
      std::memcpy(expected.data() + 4, source.data(), count * 4 * sizeof(T));
      REQUIRE(SUCCEEDED(constants(f, pixel, false, 0, original.data(), limit)));
      REQUIRE(SUCCEEDED(constants(f, pixel, false, 1, source.data(), count)));
      REQUIRE(SUCCEEDED(constants(f, pixel, true, 0, actual.data(), limit)));
      CHECK(std::memcmp(actual.data(), expected.data(), limit * 4 * sizeof(T)) == 0);
      REQUIRE(SUCCEEDED(constants(f, pixel, false, 1, source.data(), count)));
      REQUIRE(SUCCEEDED(constants(f, pixel, true, 0, actual.data(), limit)));
      CHECK(std::memcmp(actual.data(), expected.data(), limit * 4 * sizeof(T)) == 0);
    }
  }
}

// Compare every 32-bit component independently. In particular, signed zero
// and NaN payload changes must not be lost to a floating-point comparison.
template<typename T> void constant_scan_lanes(Device& f, bool pixel) {
  const UINT limit = std::is_same_v<T, float> ? (pixel ? 224u : 256u) : 16u;
  constexpr UINT count = 16;
  std::printf("case=constant-scan-lanes-%s-%s\n", pixel ? "ps" : "vs",
    std::is_same_v<T, float> ? "float" : "int");
  const uint32_t bits[] = {0, 0x80000000, 0x7fc00001, 0x7f800000};
  std::vector<T> original(limit * 4), actual(limit * 4);
  for (UINT i = 0; i < limit * 4; ++i)
    std::memcpy(original.data() + i, bits + i % 4, sizeof(T));
  const UINT start = limit - count;
  for (UINT misalignment : {1u, 2u, 3u}) for (UINT first = 0; first < count; ++first)
      for (UINT lane = 0; lane < 4; ++lane) {
    alignas(16) std::array<T, count * 4 + 4> storage{};
    T* source = storage.data() + misalignment;
    std::memcpy(source, original.data() + start * 4, count * 4 * sizeof(T));
    uint32_t changed;
    std::memcpy(&changed, source + first * 4 + lane, sizeof(changed));
    changed ^= 1u;
    std::memcpy(source + first * 4 + lane, &changed, sizeof(changed));
    auto expected = original;
    std::memcpy(expected.data() + start * 4, source, count * 4 * sizeof(T));
    REQUIRE(SUCCEEDED(constants(f, pixel, false, 0, original.data(), limit)));
    REQUIRE(SUCCEEDED(constants(f, pixel, false, start, source, count)));
    REQUIRE(SUCCEEDED(constants(f, pixel, true, 0, actual.data(), limit)));
    CHECK(std::memcmp(actual.data(), expected.data(), limit * 4 * sizeof(T)) == 0);
    REQUIRE(SUCCEEDED(constants(f, pixel, false, start, source, count)));
    REQUIRE(SUCCEEDED(constants(f, pixel, true, 0, actual.data(), limit)));
    CHECK(std::memcmp(actual.data(), expected.data(), limit * 4 * sizeof(T)) == 0);
  }
}

void reset_and_reuse(Device& f) {
  std::puts("case=reset-discard-and-wrapper-reuse");
  REQUIRE(SUCCEEDED(f.api->BeginStateJournal()));
  REQUIRE(SUCCEEDED(f.device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW)));
  D3DPRESENT_PARAMETERS invalid = f.params;
  invalid.SwapEffect = static_cast<D3DSWAPEFFECT>(0);
  CHECK(f.device->Reset(&invalid) == D3DERR_INVALIDCALL);
  CHECK(f.api->RestoreStateJournal() == D3DERR_INVALIDCALL);
  REQUIRE(SUCCEEDED(f.device->Reset(&f.params)));
  {
    api7::SelectiveJournal journal(f.api.get());
    REQUIRE(journal.begin().has_value());
    REQUIRE(journal.capture(true).has_value());
    REQUIRE(SUCCEEDED(f.device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW)));
    REQUIRE(SUCCEEDED(f.device->Reset(&f.params)));
    CHECK(!journal.restore());
    CHECK(!journal.capture(true));
    REQUIRE(journal.begin().has_value());
    REQUIRE(journal.capture(true).has_value());
    REQUIRE(SUCCEEDED(f.device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW)));
  }
  DWORD cull = 0;
  REQUIRE(SUCCEEDED(f.device->GetRenderState(D3DRS_CULLMODE, &cull)));
  CHECK(cull == D3DCULL_CCW);
  REQUIRE(SUCCEEDED(f.api->BeginSelectiveStateJournal()));
  CHECK(f.api->BeginSelectiveStateJournal() == D3DERR_INVALIDCALL);
  CHECK(f.device->BeginStateBlock() == D3DERR_INVALIDCALL);
  CHECK(SUCCEEDED(f.api->RestoreStateJournal()));
}
}

int main(int argc, char** argv) {
  if (argc != 3 || (std::strcmp(argv[2], "reference") && std::strcmp(argv[2], "optimized"))) {
    std::fprintf(stderr, "usage: api-state-runtime-tests <absolute-backend-dll> reference|optimized\n");
    return 2;
  }
  std::printf("runtime-profile=%s backend=%s\n", argv[2], argv[1]);
  Device fixture;
  if (!fixture.open(argv[1], !std::strcmp(argv[2], "optimized"))) {
    std::fprintf(stderr, "FAIL real D3D9/Vulkan device initialization, error=%lu\n", GetLastError());
    return 2;
  }
  DWORD lightIndex = 1200;
  for (Capture mode : {Capture::Full, Capture::SelectiveOn, Capture::SelectiveOff}) {
    for (bool variant : {false, true}) {
      stateblock_bools(fixture, mode, variant);
      up_bindings(fixture, mode, variant);
      render_target_side_effects(fixture, mode, variant);
    }
    if (mode != Capture::SelectiveOff) undefined_light_reapply(fixture, mode, lightIndex++);
  }
  sdk_build_and_restore(fixture);
  batch_failure_semantics(fixture);
  for (bool pixel : {false, true}) {
    constant_roundtrip<float>(fixture, pixel);
    constant_roundtrip<int>(fixture, pixel);
    constant_scan_boundaries<float>(fixture, pixel);
    constant_scan_boundaries<int>(fixture, pixel);
    constant_scan_lanes<float>(fixture, pixel);
    constant_scan_lanes<int>(fixture, pixel);
  }
  unchanged_selective_capture(fixture, !std::strcmp(argv[2], "optimized"));
  reset_and_reuse(fixture);
  return api_test::finish();
}
