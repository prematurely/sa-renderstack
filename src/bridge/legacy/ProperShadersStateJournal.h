#pragma once

#include <windows.h>
#include <d3d9.h>
#include <d3dx9effect.h>
#include "d3d9_gta_sa_api.h"

#include <atomic>
#include <cstdint>
#include <vector>

struct ProperShadersStateJournalDiagnostics
{
    std::uint64_t nativeBegins = 0;
    std::uint64_t nativeRestores = 0;
    std::uint64_t localFallbacks = 0;
    std::uint64_t nativeFailures = 0;
    std::uint64_t nativeCaptureEnables = 0;
    std::uint64_t nativeCaptureDisables = 0;
};

// Set from BridgeD3D9.ini [ProperShadersEffectOptimization] NativeStateJournal
// before any journal is constructed. When false the journal never binds the
// DXVK-side native journal (ID3D9GtaSaCompatDevice6) and always uses the local
// capture/restore path. The ID3D9GtaSaCompatDevice2 state-batch interface used
// by DirectConstants is still queried either way.
extern bool g_properShadersNativeStateJournalPolicy;

// ---- Journal-level trace-stability probe -----------------------------------
// The journal is every ProperShaders effect's state manager, so it observes ALL
// state writes regardless of technique, CTAB plan availability, or pass-lite
// eligibility. That is what the EffectInspector-level probe could not do: the
// hot per-OBJECT techniques (LitPrelight, DepthPass) have no plan at all, so
// they were never recorded there.
//
// When enabled, each transaction appends one compact record per state write
// (op + key + 64-bit value hash) into a fixed-size buffer. Nothing is captured
// or restored differently; the native and local journal paths are untouched.
// The buffer is handed to the analyser when the transaction ends.
extern bool g_properShadersJournalProbe;

// A short-lived, read-only attribution window controlled by BridgeD3D9's
// [PerformanceDiagnostics] section. It reuses the journal probe buffer but
// keeps its aggregate output separate from the older trace-stability probe.
extern std::atomic<bool> g_properShadersStateAttributionActive;

inline bool JournalProbeCaptureEnabled()
{
    return g_properShadersJournalProbe ||
        g_properShadersStateAttributionActive.load(std::memory_order_relaxed);
}

struct JournalProbeRecord
{
    std::uint8_t op = 0;      // JournalProbeOp
    std::uint32_t key = 0;    // state/stage/register identity
    std::uint64_t hash = 0;   // value fingerprint
    std::uint32_t pass = 0xFFFFFFFFu; // UINT_MAX = Begin/pre-pass state
};

enum class JournalProbeOp : std::uint8_t
{
    Transform = 1,
    Material,
    Light,
    LightEnable,
    RenderState,
    Texture,
    TextureStage,
    SamplerState,
    NPatch,
    Fvf,
    VertexShader,
    VsConstF,
    VsConstI,
    VsConstB,
    PixelShader,
    PsConstF,
    PsConstI,
    PsConstB,
};

// Receives one finished transaction's records. `techniqueName` may be null when
// the bridge could not resolve it. Implemented in EffectInspector.cpp.
void JournalProbeObserveTransaction(const char* techniqueName,
    const JournalProbeRecord* records, unsigned count, bool truncated);

struct JournalProbeTransactionInfo
{
    std::uint64_t sequence = 0;
    std::uint64_t frame = 0;
    std::uint64_t qpcTicks = 0;
    bool native = false;
};

// Observation-only transaction attribution. The implementation aggregates by
// technique and pass and writes only when JournalAttributionDump is requested.
void JournalAttributionObserveTransaction(const char* techniqueName,
    const JournalProbeRecord* records, unsigned count, bool truncated,
    const JournalProbeTransactionInfo& info);
void JournalAttributionObserveRestore(std::uint64_t sequence,
    std::uint64_t qpcTicks);
void JournalAttributionReset();
void JournalAttributionStartCapture();
void JournalAttributionStopCapture();
bool JournalAttributionTryBeginTransaction();
void JournalAttributionEndTransaction();
void JournalAttributionDump();
void JournalAttributionOnPresent();
std::uint64_t JournalAttributionCurrentFrame();

// Writes the accumulated per-technique histogram to
// scripts\BridgeD3D9.tracestab.log.
void JournalProbeDump();

class ProperShadersStateJournal final : public ID3DXEffectStateManager
{
public:
    explicit ProperShadersStateJournal(IDirect3DDevice9* device);

    HRESULT BeginTransaction(DWORD threadId, DWORD originalFlags);
    HRESULT Restore();
    void Disable();

    bool IsActive() const { return m_active; }
    bool IsDisabled() const { return m_disabled; }
    bool HasFailed() const { return FAILED(m_failure); }
    HRESULT FailureCode() const { return m_failure; }
    DWORD OriginalFlags() const { return m_originalFlags; }
    IDirect3DDevice9* Device() const { return m_device; }
    bool SupportsStateBatch() const { return m_stateBatch != nullptr; }
    bool SupportsNativeStateJournal() const { return m_nativeJournal != nullptr; }
    bool ProbeCaptureEnabled() const
    {
        return g_properShadersJournalProbe || m_probeAttributionEnabled;
    }

    static ProperShadersStateJournalDiagnostics GetDiagnostics();

    HRESULT SubmitStateBatch(const D3D9GtaSaStateBatch* batch);

    // Selective native journaling starts with capture disabled. These methods
    // temporarily enable capture for one synchronous D3DX effect operation.
    // Nested scopes only toggle the backend on the outermost transition.
    HRESULT BeginNativeCaptureScope();
    HRESULT EndNativeCaptureScope();

    void SetCurrentPass(UINT pass);
    bool HasCurrentPass() const { return m_hasCurrentPass; }
    UINT CurrentPass() const { return m_currentPass; }
    void SetPassActive(bool active) { m_passActive = active; }
    bool IsPassActive() const { return m_passActive; }

    // ---- Journal probe -----------------------------------------------------
    // The bridge sets the technique name for the current transaction (it knows
    // it; the journal does not) and hands the records to the analyser when the
    // transaction ends. Both are no-ops unless g_properShadersJournalProbe.
    void SetProbeTechnique(const char* name) { m_probeTechnique = name; }
    void FlushProbeRecords();

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE SetTransform(
        D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix) override;
    HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL9* material) override;
    HRESULT STDMETHODCALLTYPE SetLight(DWORD index, const D3DLIGHT9* light) override;
    HRESULT STDMETHODCALLTYPE LightEnable(DWORD index, BOOL enable) override;
    HRESULT STDMETHODCALLTYPE SetRenderState(
        D3DRENDERSTATETYPE state, DWORD value) override;
    HRESULT STDMETHODCALLTYPE SetTexture(
        DWORD stage, IDirect3DBaseTexture9* texture) override;
    HRESULT STDMETHODCALLTYPE SetTextureStageState(
        DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) override;
    HRESULT STDMETHODCALLTYPE SetSamplerState(
        DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) override;
    HRESULT STDMETHODCALLTYPE SetNPatchMode(float segments) override;
    HRESULT STDMETHODCALLTYPE SetFVF(DWORD fvf) override;
    HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9* shader) override;
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(
        UINT startRegister, const float* data, UINT registerCount) override;
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(
        UINT startRegister, const int* data, UINT registerCount) override;
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(
        UINT startRegister, const BOOL* data, UINT registerCount) override;
    HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9* shader) override;
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(
        UINT startRegister, const float* data, UINT registerCount) override;
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(
        UINT startRegister, const int* data, UINT registerCount) override;
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(
        UINT startRegister, const BOOL* data, UINT registerCount) override;

private:
    ~ProperShadersStateJournal();

    struct TransformEntry
    {
        D3DTRANSFORMSTATETYPE state{};
        D3DMATRIX value{};
    };

    struct LightEntry
    {
        DWORD index = 0;
        D3DLIGHT9 value{};
    };

    struct LightEnableEntry
    {
        DWORD index = 0;
        BOOL value = FALSE;
    };

    struct TextureStageEntry
    {
        DWORD stage = 0;
        D3DTEXTURESTAGESTATETYPE type{};
        DWORD value = 0;
    };

    struct RegisterSpan
    {
        UINT start = 0;
        UINT count = 0;
    };

    static constexpr UINT kRenderStateCount = 256;
    static constexpr UINT kTextureSlotCount = 20;
    static constexpr UINT kTextureStageCount = 8;
    static constexpr UINT kTextureStageStateCount = 33;
    static constexpr UINT kSamplerStateCount = 16;
    static constexpr UINT kVertexFloatRegisterCount = 256;
    static constexpr UINT kVertexIntRegisterCount = 16;
    static constexpr UINT kVertexBoolRegisterCount = 16;
    static constexpr UINT kPixelFloatRegisterCount = 224;
    static constexpr UINT kPixelIntRegisterCount = 16;
    static constexpr UINT kPixelBoolRegisterCount = 16;

    HRESULT CheckTransaction();
    HRESULT Fail(HRESULT failure);
    // Appends one probe record. Inlined and gated on the probe flag so the
    // disabled cost is a single global bool test per state write.
    void ProbeRecord(JournalProbeOp op, std::uint32_t key,
        const void* data, std::size_t bytes);
    HRESULT RecordSetResult(HRESULT result);
    HRESULT BeginNativeCapture();
    HRESULT EndNativeCapture(HRESULT operationResult);
    HRESULT ForceEndNativeCaptureScope();
    void AdvanceGeneration();
    void ClearJournal();
    static int TextureSlot(DWORD stage);

    template <typename Entry>
    HRESULT AppendEntry(std::vector<Entry>& entries, const Entry& entry);

    template <typename T, UINT RegisterCapacity, UINT Components, typename Getter>
    HRESULT CaptureConstantRange(
        UINT startRegister,
        UINT registerCount,
        uint32_t (&generations)[RegisterCapacity],
        T (&values)[RegisterCapacity * Components],
        RegisterSpan (&spans)[RegisterCapacity],
        UINT& spanCount,
        Getter&& getter);

    IDirect3DDevice9* m_device = nullptr;
    ID3D9GtaSaCompatDevice2* m_stateBatch = nullptr;
    ID3D9GtaSaCompatDevice6* m_nativeJournal = nullptr;
    std::atomic<ULONG> m_refs{ 1 };
    DWORD m_threadId = 0;
    DWORD m_originalFlags = 0;
    HRESULT m_failure = D3D_OK;
    uint32_t m_generation = 1;
    bool m_active = false;
    bool m_nativeActive = false;
    UINT m_nativeCaptureScopeDepth = 0;
    bool m_disabled = false;
    bool m_hasCurrentPass = false;
    bool m_passActive = false;
    UINT m_currentPass = 0;

    std::vector<TransformEntry> m_transforms;
    bool m_materialCaptured = false;
    D3DMATERIAL9 m_material{};
    std::vector<LightEntry> m_lights;
    std::vector<LightEnableEntry> m_lightEnables;

    uint32_t m_renderStateGeneration[kRenderStateCount]{};
    DWORD m_renderStateValues[kRenderStateCount]{};
    uint16_t m_renderStateTouched[kRenderStateCount]{};
    UINT m_renderStateTouchedCount = 0;

    uint32_t m_textureGeneration[kTextureSlotCount]{};
    IDirect3DBaseTexture9* m_textureValues[kTextureSlotCount]{};
    uint8_t m_textureTouched[kTextureSlotCount]{};
    UINT m_textureTouchedCount = 0;

    uint32_t m_textureStageGeneration[kTextureStageCount][kTextureStageStateCount]{};
    DWORD m_textureStageValues[kTextureStageCount][kTextureStageStateCount]{};
    uint16_t m_textureStageTouched[kTextureStageCount * kTextureStageStateCount]{};
    UINT m_textureStageTouchedCount = 0;

    uint32_t m_samplerGeneration[kTextureSlotCount][kSamplerStateCount]{};
    DWORD m_samplerValues[kTextureSlotCount][kSamplerStateCount]{};
    uint16_t m_samplerTouched[kTextureSlotCount * kSamplerStateCount]{};
    UINT m_samplerTouchedCount = 0;

    bool m_nPatchCaptured = false;
    float m_nPatchMode = 0.0f;
    bool m_fvfCaptured = false;
    DWORD m_fvf = 0;
    bool m_vertexShaderCaptured = false;
    IDirect3DVertexShader9* m_vertexShader = nullptr;
    bool m_pixelShaderCaptured = false;
    IDirect3DPixelShader9* m_pixelShader = nullptr;

    uint32_t m_vsFloatGeneration[kVertexFloatRegisterCount]{};
    float m_vsFloatValues[kVertexFloatRegisterCount * 4]{};
    RegisterSpan m_vsFloatSpans[kVertexFloatRegisterCount]{};
    UINT m_vsFloatSpanCount = 0;

    uint32_t m_vsIntGeneration[kVertexIntRegisterCount]{};
    int m_vsIntValues[kVertexIntRegisterCount * 4]{};
    RegisterSpan m_vsIntSpans[kVertexIntRegisterCount]{};
    UINT m_vsIntSpanCount = 0;

    uint32_t m_vsBoolGeneration[kVertexBoolRegisterCount]{};
    BOOL m_vsBoolValues[kVertexBoolRegisterCount]{};
    RegisterSpan m_vsBoolSpans[kVertexBoolRegisterCount]{};
    UINT m_vsBoolSpanCount = 0;

    uint32_t m_psFloatGeneration[kPixelFloatRegisterCount]{};
    float m_psFloatValues[kPixelFloatRegisterCount * 4]{};
    RegisterSpan m_psFloatSpans[kPixelFloatRegisterCount]{};
    UINT m_psFloatSpanCount = 0;

    uint32_t m_psIntGeneration[kPixelIntRegisterCount]{};
    int m_psIntValues[kPixelIntRegisterCount * 4]{};
    RegisterSpan m_psIntSpans[kPixelIntRegisterCount]{};
    UINT m_psIntSpanCount = 0;

    uint32_t m_psBoolGeneration[kPixelBoolRegisterCount]{};
    BOOL m_psBoolValues[kPixelBoolRegisterCount]{};
    RegisterSpan m_psBoolSpans[kPixelBoolRegisterCount]{};
    UINT m_psBoolSpanCount = 0;

    // Journal probe: fixed-size per-transaction record buffer. Sized for the
    // largest observed pass (LitSkin-class techniques write ~200 states); the
    // truncation flag is reported so a capped transaction is never mistaken for
    // a short one.
    static constexpr unsigned kProbeCapacity = 512;
    std::uint64_t m_probeSequence = 0;
    std::uint64_t m_probeFrame = 0;
    std::uint64_t m_probeStartQpc = 0;
    bool m_probeAttributionEnabled = false;
    bool m_probeFlushed = false;
    bool m_probeNative = false;
    const char* m_probeTechnique = nullptr;
    unsigned m_probeCount = 0;
    bool m_probeTruncated = false;
    JournalProbeRecord m_probeRecords[kProbeCapacity]{};
};

inline bool ProperShadersStateJournalRequiresBaselineRestart(
    const ProperShadersStateJournal* journal)
{
    return journal && journal->IsActive() && journal->HasFailed();
}

class ProperShadersNativeCaptureScope final
{
public:
    explicit ProperShadersNativeCaptureScope(ProperShadersStateJournal* journal)
        : m_journal(journal)
    {
        if (!m_journal || !m_journal->IsActive()) return;
        m_beginResult = m_journal->BeginNativeCaptureScope();
        m_active = SUCCEEDED(m_beginResult) && m_beginResult != S_FALSE;
    }

    ProperShadersNativeCaptureScope(const ProperShadersNativeCaptureScope&) = delete;
    ProperShadersNativeCaptureScope& operator=(
        const ProperShadersNativeCaptureScope&) = delete;
    ProperShadersNativeCaptureScope(ProperShadersNativeCaptureScope&&) = delete;
    ProperShadersNativeCaptureScope& operator=(
        ProperShadersNativeCaptureScope&&) = delete;

    ~ProperShadersNativeCaptureScope()
    {
        if (m_active) m_journal->EndNativeCaptureScope();
    }

    HRESULT BeginResult() const { return m_beginResult; }

    HRESULT Finish(HRESULT operationResult)
    {
        if (!m_active) return operationResult;
        const HRESULT endResult = m_journal->EndNativeCaptureScope();
        if (SUCCEEDED(endResult)) m_active = false;
        if (FAILED(operationResult)) return operationResult;
        return FAILED(endResult) ? endResult : operationResult;
    }

private:
    ProperShadersStateJournal* m_journal = nullptr;
    HRESULT m_beginResult = S_FALSE;
    bool m_active = false;
};
