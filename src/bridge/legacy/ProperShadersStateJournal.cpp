#include "ProperShadersStateJournal.h"
#include "GtaSaCompatApiVersions.h"

#include <cstring>
#include <intrin.h>
#include <new>

namespace
{
const GUID kEffectStateManagerIid =
{ 0x79aab587, 0x6dbc, 0x4fa7, { 0x82, 0xde, 0x37, 0xfa, 0x17, 0x81, 0xc5, 0xce } };

std::atomic<std::uint64_t> g_nativeBegins{ 0 };
std::atomic<std::uint64_t> g_nativeRestores{ 0 };
std::atomic<std::uint64_t> g_localFallbacks{ 0 };
std::atomic<std::uint64_t> g_nativeFailures{ 0 };
std::atomic<std::uint64_t> g_nativeCaptureEnables{ 0 };
std::atomic<std::uint64_t> g_nativeCaptureDisables{ 0 };
std::atomic<std::uint64_t> g_nextProbeSequence{ 1 };

std::uint64_t ReadProbeQpcTicks()
{
    LARGE_INTEGER value{};
    return QueryPerformanceCounter(&value)
        ? static_cast<std::uint64_t>(value.QuadPart)
        : 0;
}

DWORD ReadCurrentThreadIdFast()
{
#if defined(_M_IX86)
    return __readfsdword(0x24);
#else
    return GetCurrentThreadId();
#endif
}
}

bool g_properShadersNativeStateJournalPolicy = true;
bool g_properShadersJournalProbe = false;

void ProperShadersStateJournal::ProbeRecord(JournalProbeOp op, std::uint32_t key,
    const void* data, std::size_t bytes)
{
    if (m_probeCount >= kProbeCapacity) {
        m_probeTruncated = true;
        return;
    }
    // FNV-1a over the value bytes. A hash (not the bytes) is stored because the
    // probe only needs "did this write differ from last time", and a fixed-size
    // record keeps the buffer allocation-free on the render thread.
    std::uint64_t h = 0;
    // Attribution only needs operation/category counts. Keep the older value
    // hash probe available, but avoid hashing every constant payload during a
    // normal three-second attribution capture.
    if (g_properShadersJournalProbe) {
        h = 1469598103934665603ull;
        const unsigned char* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; p && i < bytes; ++i) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
    }
    JournalProbeRecord& r = m_probeRecords[m_probeCount++];
    r.op = static_cast<std::uint8_t>(op);
    r.key = key;
    r.hash = h;
    r.pass = m_hasCurrentPass ? m_currentPass : 0xFFFFFFFFu;
}

void ProperShadersStateJournal::FlushProbeRecords()
{
    if (m_probeFlushed) return;
    if (!g_properShadersJournalProbe && !m_probeAttributionEnabled) return;
    if (m_probeCount && g_properShadersJournalProbe) {
        JournalProbeObserveTransaction(
            m_probeTechnique, m_probeRecords, m_probeCount, m_probeTruncated);
    }
    if (m_probeAttributionEnabled && m_probeSequence) {
        JournalProbeTransactionInfo info{};
        info.sequence = m_probeSequence;
        info.frame = m_probeFrame;
        const std::uint64_t endQpc = ReadProbeQpcTicks();
        info.qpcTicks = endQpc >= m_probeStartQpc
            ? endQpc - m_probeStartQpc
            : 0;
        info.native = m_probeNative;
        JournalAttributionObserveTransaction(
            m_probeTechnique, m_probeRecords, m_probeCount,
            m_probeTruncated, info);
    }
    m_probeCount = 0;
    m_probeTruncated = false;
    m_probeTechnique = nullptr;
    m_probeFlushed = true;
}

ProperShadersStateJournal::ProperShadersStateJournal(IDirect3DDevice9* device)
    : m_device(device)
{
    if (m_device) {
        m_device->AddRef();
        if (g_properShadersNativeStateJournalPolicy &&
            SUCCEEDED(m_device->QueryInterface(
                __uuidof(ID3D9GtaSaCompatDevice6),
                reinterpret_cast<void**>(&m_nativeJournal)))) {
            m_stateBatch = static_cast<ID3D9GtaSaCompatDevice2*>(m_nativeJournal);
            m_stateBatch->AddRef();
        } else {
            m_device->QueryInterface(
                __uuidof(ID3D9GtaSaCompatDevice2),
                reinterpret_cast<void**>(&m_stateBatch));
        }
    }
    m_transforms.reserve(16);
    m_lights.reserve(8);
    m_lightEnables.reserve(8);
}

ProperShadersStateJournal::~ProperShadersStateJournal()
{
    if (m_active) Restore();
    ClearJournal();
    if (m_stateBatch) m_stateBatch->Release();
    if (m_nativeJournal) m_nativeJournal->Release();
    if (m_device) m_device->Release();
}

ProperShadersStateJournalDiagnostics ProperShadersStateJournal::GetDiagnostics()
{
    ProperShadersStateJournalDiagnostics diagnostics{};
    diagnostics.nativeBegins = g_nativeBegins.load(std::memory_order_relaxed);
    diagnostics.nativeRestores = g_nativeRestores.load(std::memory_order_relaxed);
    diagnostics.localFallbacks = g_localFallbacks.load(std::memory_order_relaxed);
    diagnostics.nativeFailures = g_nativeFailures.load(std::memory_order_relaxed);
    diagnostics.nativeCaptureEnables =
        g_nativeCaptureEnables.load(std::memory_order_relaxed);
    diagnostics.nativeCaptureDisables =
        g_nativeCaptureDisables.load(std::memory_order_relaxed);
    return diagnostics;
}

HRESULT ProperShadersStateJournal::QueryInterface(REFIID iid, void** object)
{
    if (!object) return E_POINTER;
    *object = nullptr;
    if (!IsEqualIID(iid, IID_IUnknown) && !IsEqualIID(iid, kEffectStateManagerIid)) {
        return E_NOINTERFACE;
    }
    *object = static_cast<ID3DXEffectStateManager*>(this);
    AddRef();
    return S_OK;
}

ULONG ProperShadersStateJournal::AddRef()
{
    return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG ProperShadersStateJournal::Release()
{
    const ULONG refs = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (!refs) delete this;
    return refs;
}

HRESULT ProperShadersStateJournal::BeginTransaction(DWORD threadId, DWORD originalFlags)
{
    if (!m_device || !threadId || m_disabled) return E_FAIL;
    if (m_active) {
        FlushProbeRecords();
        Restore();
        m_disabled = true;
        return E_UNEXPECTED;
    }
    if (threadId != ReadCurrentThreadIdFast()) return E_UNEXPECTED;

    AdvanceGeneration();
    ClearJournal();
    // Probe: start a fresh record set for this transaction. The technique name
    // is supplied separately by the bridge via SetProbeTechnique.
    m_probeCount = 0;
    m_probeTruncated = false;
    m_probeFlushed = false;
    m_probeAttributionEnabled = JournalAttributionTryBeginTransaction();
    m_probeSequence = m_probeAttributionEnabled
        ? g_nextProbeSequence.fetch_add(1, std::memory_order_relaxed)
        : 0;
    m_probeFrame = m_probeAttributionEnabled
        ? JournalAttributionCurrentFrame()
        : 0;
    m_probeStartQpc = m_probeAttributionEnabled ? ReadProbeQpcTicks() : 0;
    m_probeNative = false;
    m_threadId = threadId;
    m_originalFlags = originalFlags;
    m_failure = D3D_OK;
    m_hasCurrentPass = false;
    m_passActive = false;
    m_currentPass = 0;
    m_nativeActive = false;
    m_nativeCaptureScopeDepth = 0;
    if (m_nativeJournal) {
        const HRESULT nativeResult = m_nativeJournal->BeginSelectiveStateJournal();
        if (SUCCEEDED(nativeResult)) {
            m_nativeActive = true;
            m_probeNative = true;
            g_nativeBegins.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_nativeFailures.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (!m_nativeActive) {
        g_localFallbacks.fetch_add(1, std::memory_order_relaxed);
    }
    m_active = true;
    return D3D_OK;
}

void ProperShadersStateJournal::Disable()
{
    if (m_active && m_threadId == ReadCurrentThreadIdFast()) Restore();
    m_disabled = true;
}

void ProperShadersStateJournal::SetCurrentPass(UINT pass)
{
    m_currentPass = pass;
    m_hasCurrentPass = true;
}

HRESULT ProperShadersStateJournal::CheckTransaction()
{
    if (!m_active) return S_FALSE;
    if (m_disabled || m_threadId != ReadCurrentThreadIdFast()) return Fail(E_UNEXPECTED);
    return D3D_OK;
}

HRESULT ProperShadersStateJournal::Fail(HRESULT failure)
{
    if (SUCCEEDED(failure)) failure = E_FAIL;
    if (SUCCEEDED(m_failure)) m_failure = failure;
    return failure;
}

HRESULT ProperShadersStateJournal::RecordSetResult(HRESULT result)
{
    return FAILED(result) && m_active ? Fail(result) : result;
}

HRESULT ProperShadersStateJournal::BeginNativeCapture()
{
    if (!m_nativeActive || !m_nativeJournal) return E_UNEXPECTED;
    if (m_nativeCaptureScopeDepth != 0) return D3D_OK;
    const HRESULT result = m_nativeJournal->SetStateJournalCaptureEnabled(TRUE);
    if (FAILED(result)) {
        g_nativeFailures.fetch_add(1, std::memory_order_relaxed);
        return Fail(result);
    }
    g_nativeCaptureEnables.fetch_add(1, std::memory_order_relaxed);
    return D3D_OK;
}

HRESULT ProperShadersStateJournal::EndNativeCapture(HRESULT operationResult)
{
    const HRESULT recordedResult = RecordSetResult(operationResult);
    if (m_nativeCaptureScopeDepth != 0) return recordedResult;
    const HRESULT disableResult = m_nativeJournal
        ? m_nativeJournal->SetStateJournalCaptureEnabled(FALSE)
        : E_UNEXPECTED;
    if (FAILED(disableResult)) {
        g_nativeFailures.fetch_add(1, std::memory_order_relaxed);
        Fail(disableResult);
    } else {
        g_nativeCaptureDisables.fetch_add(1, std::memory_order_relaxed);
    }
    return FAILED(recordedResult) ? recordedResult : disableResult;
}

HRESULT ProperShadersStateJournal::BeginNativeCaptureScope()
{
    if (!m_nativeActive || !m_nativeJournal) return S_FALSE;
    if (m_nativeCaptureScopeDepth != 0) {
        if (m_nativeCaptureScopeDepth == UINT_MAX) return Fail(E_UNEXPECTED);
        ++m_nativeCaptureScopeDepth;
        return D3D_OK;
    }

    const HRESULT result = m_nativeJournal->SetStateJournalCaptureEnabled(TRUE);
    if (FAILED(result)) {
        g_nativeFailures.fetch_add(1, std::memory_order_relaxed);
        return Fail(result);
    }
    m_nativeCaptureScopeDepth = 1;
    g_nativeCaptureEnables.fetch_add(1, std::memory_order_relaxed);
    return D3D_OK;
}

HRESULT ProperShadersStateJournal::EndNativeCaptureScope()
{
    if (m_nativeCaptureScopeDepth == 0) return S_FALSE;
    --m_nativeCaptureScopeDepth;
    if (m_nativeCaptureScopeDepth != 0) return D3D_OK;

    const HRESULT result = m_nativeJournal
        ? m_nativeJournal->SetStateJournalCaptureEnabled(FALSE)
        : E_UNEXPECTED;
    if (FAILED(result)) {
        m_nativeCaptureScopeDepth = 1;
        g_nativeFailures.fetch_add(1, std::memory_order_relaxed);
        Fail(result);
    } else {
        g_nativeCaptureDisables.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}

HRESULT ProperShadersStateJournal::ForceEndNativeCaptureScope()
{
    if (m_nativeCaptureScopeDepth == 0) return D3D_OK;
    m_nativeCaptureScopeDepth = 0;
    const HRESULT result = m_nativeJournal
        ? m_nativeJournal->SetStateJournalCaptureEnabled(FALSE)
        : E_UNEXPECTED;
    if (FAILED(result)) {
        g_nativeFailures.fetch_add(1, std::memory_order_relaxed);
        Fail(result);
    } else {
        g_nativeCaptureDisables.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}

void ProperShadersStateJournal::AdvanceGeneration()
{
    if (++m_generation != 0) return;
    m_generation = 1;
    std::memset(m_renderStateGeneration, 0, sizeof(m_renderStateGeneration));
    std::memset(m_textureGeneration, 0, sizeof(m_textureGeneration));
    std::memset(m_textureStageGeneration, 0, sizeof(m_textureStageGeneration));
    std::memset(m_samplerGeneration, 0, sizeof(m_samplerGeneration));
    std::memset(m_vsFloatGeneration, 0, sizeof(m_vsFloatGeneration));
    std::memset(m_vsIntGeneration, 0, sizeof(m_vsIntGeneration));
    std::memset(m_vsBoolGeneration, 0, sizeof(m_vsBoolGeneration));
    std::memset(m_psFloatGeneration, 0, sizeof(m_psFloatGeneration));
    std::memset(m_psIntGeneration, 0, sizeof(m_psIntGeneration));
    std::memset(m_psBoolGeneration, 0, sizeof(m_psBoolGeneration));
}

void ProperShadersStateJournal::ClearJournal()
{
    for (UINT i = 0; i < m_textureTouchedCount; ++i) {
        const UINT slot = m_textureTouched[i];
        if (m_textureValues[slot]) m_textureValues[slot]->Release();
        m_textureValues[slot] = nullptr;
    }
    if (m_vertexShader) m_vertexShader->Release();
    if (m_pixelShader) m_pixelShader->Release();
    m_vertexShader = nullptr;
    m_pixelShader = nullptr;

    m_transforms.clear();
    m_lights.clear();
    m_lightEnables.clear();
    m_materialCaptured = false;
    m_renderStateTouchedCount = 0;
    m_textureTouchedCount = 0;
    m_textureStageTouchedCount = 0;
    m_samplerTouchedCount = 0;
    m_nPatchCaptured = false;
    m_fvfCaptured = false;
    m_vertexShaderCaptured = false;
    m_pixelShaderCaptured = false;
    m_vsFloatSpanCount = 0;
    m_vsIntSpanCount = 0;
    m_vsBoolSpanCount = 0;
    m_psFloatSpanCount = 0;
    m_psIntSpanCount = 0;
    m_psBoolSpanCount = 0;
}

int ProperShadersStateJournal::TextureSlot(DWORD stage)
{
    if (stage < 16) return static_cast<int>(stage);
    if (stage >= D3DVERTEXTEXTURESAMPLER0 && stage <= D3DVERTEXTEXTURESAMPLER3) {
        return 16 + static_cast<int>(stage - D3DVERTEXTEXTURESAMPLER0);
    }
    return -1;
}

template <typename Entry>
HRESULT ProperShadersStateJournal::AppendEntry(
    std::vector<Entry>& entries, const Entry& entry)
{
    try {
        entries.push_back(entry);
        return D3D_OK;
    } catch (const std::bad_alloc&) {
        return Fail(E_OUTOFMEMORY);
    }
}

template <typename T, UINT RegisterCapacity, UINT Components, typename Getter>
HRESULT ProperShadersStateJournal::CaptureConstantRange(
    UINT startRegister,
    UINT registerCount,
    uint32_t (&generations)[RegisterCapacity],
    T (&values)[RegisterCapacity * Components],
    RegisterSpan (&spans)[RegisterCapacity],
    UINT& spanCount,
    Getter&& getter)
{
    if (!registerCount) return D3D_OK;
    if (startRegister >= RegisterCapacity || registerCount > RegisterCapacity - startRegister) {
        return Fail(D3DERR_INVALIDCALL);
    }

    const UINT end = startRegister + registerCount;
    UINT cursor = startRegister;
    while (cursor < end) {
        while (cursor < end && generations[cursor] == m_generation) ++cursor;
        if (cursor == end) break;
        const UINT runStart = cursor;
        while (cursor < end && generations[cursor] != m_generation) ++cursor;
        const UINT runCount = cursor - runStart;
        const HRESULT hr = getter(runStart, values + runStart * Components, runCount);
        if (FAILED(hr)) return Fail(hr);
        if (spanCount >= RegisterCapacity) return Fail(E_UNEXPECTED);
        spans[spanCount++] = { runStart, runCount };
        for (UINT i = runStart; i < cursor; ++i) generations[i] = m_generation;
    }
    return D3D_OK;
}

HRESULT ProperShadersStateJournal::SetTransform(
    D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix)
{
    const HRESULT transaction = CheckTransaction();
    if (transaction == S_FALSE) return m_device->SetTransform(state, matrix);
    if (FAILED(transaction)) return transaction;
    if (ProbeCaptureEnabled()) {
        ProbeRecord(JournalProbeOp::Transform, state, matrix, sizeof(D3DMATRIX));
    }
    if (m_nativeActive) {
        const HRESULT capture = BeginNativeCapture();
        if (FAILED(capture)) return capture;
        return EndNativeCapture(m_device->SetTransform(state, matrix));
    }

    bool captured = false;
    for (const auto& entry : m_transforms) {
        if (entry.state == state) {
            captured = true;
            break;
        }
    }
    if (!captured) {
        TransformEntry entry{};
        entry.state = state;
        HRESULT hr = m_device->GetTransform(state, &entry.value);
        if (FAILED(hr)) return Fail(hr);
        hr = AppendEntry(m_transforms, entry);
        if (FAILED(hr)) return hr;
    }
    return RecordSetResult(m_device->SetTransform(state, matrix));
}

HRESULT ProperShadersStateJournal::SetMaterial(const D3DMATERIAL9* material)
{
    const HRESULT transaction = CheckTransaction();
    if (transaction == S_FALSE) return m_device->SetMaterial(material);
    if (FAILED(transaction)) return transaction;
    if (ProbeCaptureEnabled()) {
        ProbeRecord(JournalProbeOp::Material, 0, material, sizeof(D3DMATERIAL9));
    }
    if (m_nativeActive) {
        const HRESULT capture = BeginNativeCapture();
        if (FAILED(capture)) return capture;
        return EndNativeCapture(m_device->SetMaterial(material));
    }
    if (!m_materialCaptured) {
        const HRESULT hr = m_device->GetMaterial(&m_material);
        if (FAILED(hr)) return Fail(hr);
        m_materialCaptured = true;
    }
    return RecordSetResult(m_device->SetMaterial(material));
}

HRESULT ProperShadersStateJournal::SetLight(DWORD index, const D3DLIGHT9* light)
{
    const HRESULT transaction = CheckTransaction();
    if (transaction == S_FALSE) return m_device->SetLight(index, light);
    if (FAILED(transaction)) return transaction;
    if (ProbeCaptureEnabled()) {
        ProbeRecord(JournalProbeOp::Light, index, light, sizeof(D3DLIGHT9));
    }
    if (m_nativeActive) {
        const HRESULT capture = BeginNativeCapture();
        if (FAILED(capture)) return capture;
        return EndNativeCapture(m_device->SetLight(index, light));
    }

    bool captured = false;
    for (const auto& entry : m_lights) {
        if (entry.index == index) {
            captured = true;
            break;
        }
    }
    if (!captured) {
        LightEntry entry{};
        entry.index = index;
        HRESULT hr = m_device->GetLight(index, &entry.value);
        if (FAILED(hr)) return Fail(hr);
        hr = AppendEntry(m_lights, entry);
        if (FAILED(hr)) return hr;
    }
    return RecordSetResult(m_device->SetLight(index, light));
}

HRESULT ProperShadersStateJournal::LightEnable(DWORD index, BOOL enable)
{
    const HRESULT transaction = CheckTransaction();
    if (transaction == S_FALSE) return m_device->LightEnable(index, enable);
    if (FAILED(transaction)) return transaction;
    if (ProbeCaptureEnabled()) {
        ProbeRecord(JournalProbeOp::LightEnable, index, &enable, sizeof(enable));
    }
    if (m_nativeActive) {
        const HRESULT capture = BeginNativeCapture();
        if (FAILED(capture)) return capture;
        return EndNativeCapture(m_device->LightEnable(index, enable));
    }

    bool captured = false;
    for (const auto& entry : m_lightEnables) {
        if (entry.index == index) {
            captured = true;
            break;
        }
    }
    if (!captured) {
        LightEnableEntry entry{};
        entry.index = index;
        HRESULT hr = m_device->GetLightEnable(index, &entry.value);
        if (FAILED(hr)) return Fail(hr);
        hr = AppendEntry(m_lightEnables, entry);
        if (FAILED(hr)) return hr;
    }
    return RecordSetResult(m_device->LightEnable(index, enable));
}

HRESULT ProperShadersStateJournal::SetRenderState(
    D3DRENDERSTATETYPE state, DWORD value)
{
    const HRESULT transaction = CheckTransaction();
    if (transaction == S_FALSE) return m_device->SetRenderState(state, value);
    if (FAILED(transaction)) return transaction;
    if (ProbeCaptureEnabled()) {
        ProbeRecord(JournalProbeOp::RenderState, state, &value, sizeof(value));
    }
    if (m_nativeActive) {
        const HRESULT capture = BeginNativeCapture();
        if (FAILED(capture)) return capture;
        return EndNativeCapture(m_device->SetRenderState(state, value));
    }

    const UINT index = static_cast<UINT>(state);
    if (index >= kRenderStateCount) return Fail(D3DERR_INVALIDCALL);
    if (m_renderStateGeneration[index] != m_generation) {
        const HRESULT hr = m_device->GetRenderState(state, &m_renderStateValues[index]);
        if (FAILED(hr)) return Fail(hr);
        m_renderStateGeneration[index] = m_generation;
        m_renderStateTouched[m_renderStateTouchedCount++] = static_cast<uint16_t>(index);
    }
    return RecordSetResult(m_device->SetRenderState(state, value));
}

HRESULT ProperShadersStateJournal::SetTexture(
    DWORD stage, IDirect3DBaseTexture9* texture)
{
    const HRESULT transaction = CheckTransaction();
    if (transaction == S_FALSE) return m_device->SetTexture(stage, texture);
    if (FAILED(transaction)) return transaction;
    if (ProbeCaptureEnabled()) {
        ProbeRecord(JournalProbeOp::Texture, stage, &texture, sizeof(texture));
    }
    if (m_nativeActive) {
        const HRESULT capture = BeginNativeCapture();
        if (FAILED(capture)) return capture;
        return EndNativeCapture(m_device->SetTexture(stage, texture));
    }

    const int slot = TextureSlot(stage);
    if (slot < 0) return Fail(D3DERR_INVALIDCALL);
    if (m_textureGeneration[slot] != m_generation) {
        const HRESULT hr = m_device->GetTexture(stage, &m_textureValues[slot]);
        if (FAILED(hr)) return Fail(hr);
        m_textureGeneration[slot] = m_generation;
        m_textureTouched[m_textureTouchedCount++] = static_cast<uint8_t>(slot);
    }
    return RecordSetResult(m_device->SetTexture(stage, texture));
}

HRESULT ProperShadersStateJournal::SubmitStateBatch(
    const D3D9GtaSaStateBatch* batch)
{
    constexpr UINT kMaxRanges = 16;
    if (!batch) return E_POINTER;
    if (!m_stateBatch) return E_NOINTERFACE;

    const HRESULT transaction = CheckTransaction();
    if (FAILED(transaction)) return transaction;
    if (batch->StructSize < sizeof(D3D9GtaSaStateBatch) ||
        batch->ApiVersion < GtaSaCompatApiVersions::kStateBatch ||
        batch->ApiVersion > D3D9_GTA_SA_COMPAT_API_VERSION ||
        batch->VertexFloatRangeCount > kMaxRanges ||
        batch->VertexBoolRangeCount > kMaxRanges ||
        batch->PixelFloatRangeCount > kMaxRanges ||
        batch->PixelBoolRangeCount > kMaxRanges ||
        batch->TextureBindingCount > kMaxRanges ||
        (batch->VertexFloatRangeCount && !batch->VertexFloatRanges) ||
        (batch->VertexBoolRangeCount && !batch->VertexBoolRanges) ||
        (batch->PixelFloatRangeCount && !batch->PixelFloatRanges) ||
        (batch->PixelBoolRangeCount && !batch->PixelBoolRanges) ||
        (batch->TextureBindingCount && !batch->TextureBindings)) {
        return transaction == S_FALSE ? D3DERR_INVALIDCALL : Fail(D3DERR_INVALIDCALL);
    }

    if (transaction == S_FALSE) {
        return m_stateBatch->SubmitStateBatch(batch);
    }
    if (m_nativeActive) {
        const HRESULT capture = BeginNativeCapture();
        if (FAILED(capture)) return capture;
        return EndNativeCapture(m_stateBatch->SubmitStateBatch(batch));
    }

    for (UINT i = 0; i < batch->VertexFloatRangeCount; ++i) {
        const auto& range = batch->VertexFloatRanges[i];
        HRESULT hr = CaptureConstantRange<float, kVertexFloatRegisterCount, 4>(
            range.StartRegister, range.RegisterCount,
            m_vsFloatGeneration, m_vsFloatValues, m_vsFloatSpans, m_vsFloatSpanCount,
            [this](UINT start, float* values, UINT count) {
                return m_device->GetVertexShaderConstantF(start, values, count);
            });
        if (FAILED(hr)) return hr;
    }

    for (UINT i = 0; i < batch->VertexBoolRangeCount; ++i) {
        const auto& range = batch->VertexBoolRanges[i];
        HRESULT hr = CaptureConstantRange<BOOL, kVertexBoolRegisterCount, 1>(
            range.StartRegister, range.RegisterCount,
            m_vsBoolGeneration, m_vsBoolValues, m_vsBoolSpans, m_vsBoolSpanCount,
            [this](UINT start, BOOL* values, UINT count) {
                return m_device->GetVertexShaderConstantB(start, values, count);
            });
        if (FAILED(hr)) return hr;
    }

    for (UINT i = 0; i < batch->PixelFloatRangeCount; ++i) {
        const auto& range = batch->PixelFloatRanges[i];
        HRESULT hr = CaptureConstantRange<float, kPixelFloatRegisterCount, 4>(
            range.StartRegister, range.RegisterCount,
            m_psFloatGeneration, m_psFloatValues, m_psFloatSpans, m_psFloatSpanCount,
            [this](UINT start, float* values, UINT count) {
                return m_device->GetPixelShaderConstantF(start, values, count);
            });
        if (FAILED(hr)) return hr;
    }

    for (UINT i = 0; i < batch->PixelBoolRangeCount; ++i) {
        const auto& range = batch->PixelBoolRanges[i];
        HRESULT hr = CaptureConstantRange<BOOL, kPixelBoolRegisterCount, 1>(
            range.StartRegister, range.RegisterCount,
            m_psBoolGeneration, m_psBoolValues, m_psBoolSpans, m_psBoolSpanCount,
            [this](UINT start, BOOL* values, UINT count) {
                return m_device->GetPixelShaderConstantB(start, values, count);
            });
        if (FAILED(hr)) return hr;
    }

    for (UINT i = 0; i < batch->TextureBindingCount; ++i) {
        const DWORD stage = batch->TextureBindings[i].Stage;
        const int slot = TextureSlot(stage);
        if (slot < 0) return Fail(D3DERR_INVALIDCALL);
        if (m_textureGeneration[slot] == m_generation) continue;

        const HRESULT hr = m_device->GetTexture(stage, &m_textureValues[slot]);
        if (FAILED(hr)) return Fail(hr);
        m_textureGeneration[slot] = m_generation;
        m_textureTouched[m_textureTouchedCount++] = static_cast<uint8_t>(slot);
    }

    return RecordSetResult(m_stateBatch->SubmitStateBatch(batch));
}

HRESULT ProperShadersStateJournal::SetTextureStageState(
    DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
    const HRESULT transaction = CheckTransaction();
    if (transaction == S_FALSE) return m_device->SetTextureStageState(stage, type, value);
    if (FAILED(transaction)) return transaction;
    if (ProbeCaptureEnabled()) {
        ProbeRecord(JournalProbeOp::TextureStage, (stage << 8) | type,
            &value, sizeof(value));
    }
    if (m_nativeActive) {
        const HRESULT capture = BeginNativeCapture();
        if (FAILED(capture)) return capture;
        return EndNativeCapture(m_device->SetTextureStageState(stage, type, value));
    }

    const UINT typeIndex = static_cast<UINT>(type);
    if (stage >= kTextureStageCount || typeIndex >= kTextureStageStateCount) {
        return Fail(D3DERR_INVALIDCALL);
    }
    if (m_textureStageGeneration[stage][typeIndex] != m_generation) {
        const HRESULT hr = m_device->GetTextureStageState(
            stage, type, &m_textureStageValues[stage][typeIndex]);
        if (FAILED(hr)) return Fail(hr);
        m_textureStageGeneration[stage][typeIndex] = m_generation;
        m_textureStageTouched[m_textureStageTouchedCount++] =
            static_cast<uint16_t>(stage * kTextureStageStateCount + typeIndex);
    }
    return RecordSetResult(m_device->SetTextureStageState(stage, type, value));
}

HRESULT ProperShadersStateJournal::SetSamplerState(
    DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value)
{
    const HRESULT transaction = CheckTransaction();
    if (transaction == S_FALSE) return m_device->SetSamplerState(sampler, type, value);
    if (FAILED(transaction)) return transaction;
    if (ProbeCaptureEnabled()) {
        ProbeRecord(JournalProbeOp::SamplerState, (sampler << 8) | type,
            &value, sizeof(value));
    }
    if (m_nativeActive) {
        const HRESULT capture = BeginNativeCapture();
        if (FAILED(capture)) return capture;
        return EndNativeCapture(m_device->SetSamplerState(sampler, type, value));
    }

    const int slot = TextureSlot(sampler);
    const UINT typeIndex = static_cast<UINT>(type);
    if (slot < 0 || typeIndex >= kSamplerStateCount) return Fail(D3DERR_INVALIDCALL);
    if (m_samplerGeneration[slot][typeIndex] != m_generation) {
        const HRESULT hr = m_device->GetSamplerState(
            sampler, type, &m_samplerValues[slot][typeIndex]);
        if (FAILED(hr)) return Fail(hr);
        m_samplerGeneration[slot][typeIndex] = m_generation;
        m_samplerTouched[m_samplerTouchedCount++] =
            static_cast<uint16_t>(slot * kSamplerStateCount + typeIndex);
    }
    return RecordSetResult(m_device->SetSamplerState(sampler, type, value));
}

HRESULT ProperShadersStateJournal::SetNPatchMode(float segments)
{
    const HRESULT transaction = CheckTransaction();
    if (transaction == S_FALSE) return m_device->SetNPatchMode(segments);
    if (FAILED(transaction)) return transaction;
    if (ProbeCaptureEnabled()) {
        ProbeRecord(JournalProbeOp::NPatch, 0, &segments, sizeof(segments));
    }
    if (m_nativeActive) {
        const HRESULT capture = BeginNativeCapture();
        if (FAILED(capture)) return capture;
        return EndNativeCapture(m_device->SetNPatchMode(segments));
    }
    if (!m_nPatchCaptured) {
        m_nPatchMode = m_device->GetNPatchMode();
        m_nPatchCaptured = true;
    }
    return RecordSetResult(m_device->SetNPatchMode(segments));
}

HRESULT ProperShadersStateJournal::SetFVF(DWORD fvf)
{
    const HRESULT transaction = CheckTransaction();
    if (transaction == S_FALSE) return m_device->SetFVF(fvf);
    if (FAILED(transaction)) return transaction;
    if (ProbeCaptureEnabled()) {
        ProbeRecord(JournalProbeOp::Fvf, 0, &fvf, sizeof(fvf));
    }
    if (m_nativeActive) {
        const HRESULT capture = BeginNativeCapture();
        if (FAILED(capture)) return capture;
        return EndNativeCapture(m_device->SetFVF(fvf));
    }
    if (!m_fvfCaptured) {
        const HRESULT hr = m_device->GetFVF(&m_fvf);
        if (FAILED(hr)) return Fail(hr);
        m_fvfCaptured = true;
    }
    return RecordSetResult(m_device->SetFVF(fvf));
}

HRESULT ProperShadersStateJournal::SetVertexShader(IDirect3DVertexShader9* shader)
{
    const HRESULT transaction = CheckTransaction();
    if (transaction == S_FALSE) return m_device->SetVertexShader(shader);
    if (FAILED(transaction)) return transaction;
    if (ProbeCaptureEnabled()) {
        ProbeRecord(JournalProbeOp::VertexShader, 0, &shader, sizeof(shader));
    }
    if (m_nativeActive) {
        const HRESULT capture = BeginNativeCapture();
        if (FAILED(capture)) return capture;
        return EndNativeCapture(m_device->SetVertexShader(shader));
    }
    if (!m_vertexShaderCaptured) {
        const HRESULT hr = m_device->GetVertexShader(&m_vertexShader);
        if (FAILED(hr)) return Fail(hr);
        m_vertexShaderCaptured = true;
    }
    return RecordSetResult(m_device->SetVertexShader(shader));
}

HRESULT ProperShadersStateJournal::SetVertexShaderConstantF(
    UINT startRegister, const float* data, UINT registerCount)
{
    const HRESULT transaction = CheckTransaction();
    if (transaction == S_FALSE) {
        return m_device->SetVertexShaderConstantF(startRegister, data, registerCount);
    }
    if (FAILED(transaction)) return transaction;
    if (ProbeCaptureEnabled()) {
        ProbeRecord(JournalProbeOp::VsConstF, (startRegister << 8) | (registerCount & 0xFF),
            data, registerCount * 4 * sizeof(float));
    }
    if (m_nativeActive) {
        const HRESULT capture = BeginNativeCapture();
        if (FAILED(capture)) return capture;
        return EndNativeCapture(
            m_device->SetVertexShaderConstantF(startRegister, data, registerCount));
    }
    HRESULT hr = CaptureConstantRange<float, kVertexFloatRegisterCount, 4>(
        startRegister, registerCount,
        m_vsFloatGeneration, m_vsFloatValues, m_vsFloatSpans, m_vsFloatSpanCount,
        [this](UINT start, float* values, UINT count) {
            return m_device->GetVertexShaderConstantF(start, values, count);
        });
    if (FAILED(hr)) return hr;
    return RecordSetResult(
        m_device->SetVertexShaderConstantF(startRegister, data, registerCount));
}

HRESULT ProperShadersStateJournal::SetVertexShaderConstantI(
    UINT startRegister, const int* data, UINT registerCount)
{
    const HRESULT transaction = CheckTransaction();
    if (transaction == S_FALSE) {
        return m_device->SetVertexShaderConstantI(startRegister, data, registerCount);
    }
    if (FAILED(transaction)) return transaction;
    if (ProbeCaptureEnabled()) {
        ProbeRecord(JournalProbeOp::VsConstI, (startRegister << 8) | (registerCount & 0xFF),
            data, registerCount * 4 * sizeof(int));
    }
    if (m_nativeActive) {
        const HRESULT capture = BeginNativeCapture();
        if (FAILED(capture)) return capture;
        return EndNativeCapture(
            m_device->SetVertexShaderConstantI(startRegister, data, registerCount));
    }
    HRESULT hr = CaptureConstantRange<int, kVertexIntRegisterCount, 4>(
        startRegister, registerCount,
        m_vsIntGeneration, m_vsIntValues, m_vsIntSpans, m_vsIntSpanCount,
        [this](UINT start, int* values, UINT count) {
            return m_device->GetVertexShaderConstantI(start, values, count);
        });
    if (FAILED(hr)) return hr;
    return RecordSetResult(
        m_device->SetVertexShaderConstantI(startRegister, data, registerCount));
}

HRESULT ProperShadersStateJournal::SetVertexShaderConstantB(
    UINT startRegister, const BOOL* data, UINT registerCount)
{
    const HRESULT transaction = CheckTransaction();
    if (transaction == S_FALSE) {
        return m_device->SetVertexShaderConstantB(startRegister, data, registerCount);
    }
    if (FAILED(transaction)) return transaction;
    if (ProbeCaptureEnabled()) {
        ProbeRecord(JournalProbeOp::VsConstB, (startRegister << 8) | (registerCount & 0xFF),
            data, registerCount * sizeof(BOOL));
    }
    if (m_nativeActive) {
        const HRESULT capture = BeginNativeCapture();
        if (FAILED(capture)) return capture;
        return EndNativeCapture(
            m_device->SetVertexShaderConstantB(startRegister, data, registerCount));
    }
    HRESULT hr = CaptureConstantRange<BOOL, kVertexBoolRegisterCount, 1>(
        startRegister, registerCount,
        m_vsBoolGeneration, m_vsBoolValues, m_vsBoolSpans, m_vsBoolSpanCount,
        [this](UINT start, BOOL* values, UINT count) {
            return m_device->GetVertexShaderConstantB(start, values, count);
        });
    if (FAILED(hr)) return hr;
    return RecordSetResult(
        m_device->SetVertexShaderConstantB(startRegister, data, registerCount));
}

HRESULT ProperShadersStateJournal::SetPixelShader(IDirect3DPixelShader9* shader)
{
    const HRESULT transaction = CheckTransaction();
    if (transaction == S_FALSE) return m_device->SetPixelShader(shader);
    if (FAILED(transaction)) return transaction;
    if (ProbeCaptureEnabled()) {
        ProbeRecord(JournalProbeOp::PixelShader, 0, &shader, sizeof(shader));
    }
    if (m_nativeActive) {
        const HRESULT capture = BeginNativeCapture();
        if (FAILED(capture)) return capture;
        return EndNativeCapture(m_device->SetPixelShader(shader));
    }
    if (!m_pixelShaderCaptured) {
        const HRESULT hr = m_device->GetPixelShader(&m_pixelShader);
        if (FAILED(hr)) return Fail(hr);
        m_pixelShaderCaptured = true;
    }
    return RecordSetResult(m_device->SetPixelShader(shader));
}

HRESULT ProperShadersStateJournal::SetPixelShaderConstantF(
    UINT startRegister, const float* data, UINT registerCount)
{
    const HRESULT transaction = CheckTransaction();
    if (transaction == S_FALSE) {
        return m_device->SetPixelShaderConstantF(startRegister, data, registerCount);
    }
    if (FAILED(transaction)) return transaction;
    if (ProbeCaptureEnabled()) {
        ProbeRecord(JournalProbeOp::PsConstF, (startRegister << 8) | (registerCount & 0xFF),
            data, registerCount * 4 * sizeof(float));
    }
    if (m_nativeActive) {
        const HRESULT capture = BeginNativeCapture();
        if (FAILED(capture)) return capture;
        return EndNativeCapture(
            m_device->SetPixelShaderConstantF(startRegister, data, registerCount));
    }
    HRESULT hr = CaptureConstantRange<float, kPixelFloatRegisterCount, 4>(
        startRegister, registerCount,
        m_psFloatGeneration, m_psFloatValues, m_psFloatSpans, m_psFloatSpanCount,
        [this](UINT start, float* values, UINT count) {
            return m_device->GetPixelShaderConstantF(start, values, count);
        });
    if (FAILED(hr)) return hr;
    return RecordSetResult(
        m_device->SetPixelShaderConstantF(startRegister, data, registerCount));
}

HRESULT ProperShadersStateJournal::SetPixelShaderConstantI(
    UINT startRegister, const int* data, UINT registerCount)
{
    const HRESULT transaction = CheckTransaction();
    if (transaction == S_FALSE) {
        return m_device->SetPixelShaderConstantI(startRegister, data, registerCount);
    }
    if (FAILED(transaction)) return transaction;
    if (ProbeCaptureEnabled()) {
        ProbeRecord(JournalProbeOp::PsConstI, (startRegister << 8) | (registerCount & 0xFF),
            data, registerCount * 4 * sizeof(int));
    }
    if (m_nativeActive) {
        const HRESULT capture = BeginNativeCapture();
        if (FAILED(capture)) return capture;
        return EndNativeCapture(
            m_device->SetPixelShaderConstantI(startRegister, data, registerCount));
    }
    HRESULT hr = CaptureConstantRange<int, kPixelIntRegisterCount, 4>(
        startRegister, registerCount,
        m_psIntGeneration, m_psIntValues, m_psIntSpans, m_psIntSpanCount,
        [this](UINT start, int* values, UINT count) {
            return m_device->GetPixelShaderConstantI(start, values, count);
        });
    if (FAILED(hr)) return hr;
    return RecordSetResult(
        m_device->SetPixelShaderConstantI(startRegister, data, registerCount));
}

HRESULT ProperShadersStateJournal::SetPixelShaderConstantB(
    UINT startRegister, const BOOL* data, UINT registerCount)
{
    const HRESULT transaction = CheckTransaction();
    if (transaction == S_FALSE) {
        return m_device->SetPixelShaderConstantB(startRegister, data, registerCount);
    }
    if (FAILED(transaction)) return transaction;
    if (ProbeCaptureEnabled()) {
        ProbeRecord(JournalProbeOp::PsConstB, (startRegister << 8) | (registerCount & 0xFF),
            data, registerCount * sizeof(BOOL));
    }
    if (m_nativeActive) {
        const HRESULT capture = BeginNativeCapture();
        if (FAILED(capture)) return capture;
        return EndNativeCapture(
            m_device->SetPixelShaderConstantB(startRegister, data, registerCount));
    }
    HRESULT hr = CaptureConstantRange<BOOL, kPixelBoolRegisterCount, 1>(
        startRegister, registerCount,
        m_psBoolGeneration, m_psBoolValues, m_psBoolSpans, m_psBoolSpanCount,
        [this](UINT start, BOOL* values, UINT count) {
            return m_device->GetPixelShaderConstantB(start, values, count);
        });
    if (FAILED(hr)) return hr;
    return RecordSetResult(
        m_device->SetPixelShaderConstantB(startRegister, data, registerCount));
}

HRESULT ProperShadersStateJournal::Restore()
{
    if (!m_active) return D3D_OK;

    // Restore is also used by fallback, disable, nested-Begin, and teardown
    // paths. Flush before measuring/replaying the saved state so an admitted
    // attribution transaction cannot remain in flight without a record set.
    FlushProbeRecords();

    const std::uint64_t restoreStartQpc =
        m_probeAttributionEnabled && m_probeSequence ? ReadProbeQpcTicks() : 0;
    const auto finishAttribution = [this, restoreStartQpc]() {
        if (!m_probeAttributionEnabled) return;
        if (m_probeSequence && restoreStartQpc) {
            const std::uint64_t endQpc = ReadProbeQpcTicks();
            JournalAttributionObserveRestore(
                m_probeSequence,
                endQpc >= restoreStartQpc ? endQpc - restoreStartQpc : 0);
        }
        JournalAttributionEndTransaction();
        m_probeAttributionEnabled = false;
    };

    if (m_nativeActive) {
        const HRESULT captureScopeResult = ForceEndNativeCaptureScope();
        m_nativeActive = false;
        g_nativeRestores.fetch_add(1, std::memory_order_relaxed);
        const HRESULT restoreResult = m_nativeJournal
            ? m_nativeJournal->RestoreStateJournal()
            : E_UNEXPECTED;
        const HRESULT result = FAILED(captureScopeResult)
            ? captureScopeResult
            : restoreResult;
        m_active = false;
        m_hasCurrentPass = false;
        m_passActive = false;
        if (FAILED(restoreResult)) {
            g_nativeFailures.fetch_add(1, std::memory_order_relaxed);
            Fail(restoreResult);
            m_disabled = true;
        }
        if (FAILED(captureScopeResult)) {
            Fail(captureScopeResult);
            m_disabled = true;
        }
        ClearJournal();
        finishAttribution();
        return result;
    }

    if (m_threadId != ReadCurrentThreadIdFast()) {
        m_disabled = true;
        finishAttribution();
        return Fail(E_UNEXPECTED);
    }

    HRESULT result = D3D_OK;
    const auto recordFailure = [&result](HRESULT hr) {
        if (FAILED(hr) && SUCCEEDED(result)) result = hr;
    };

    for (UINT i = m_psBoolSpanCount; i > 0; --i) {
        const RegisterSpan span = m_psBoolSpans[i - 1];
        recordFailure(m_device->SetPixelShaderConstantB(
            span.start, m_psBoolValues + span.start, span.count));
    }
    for (UINT i = m_psIntSpanCount; i > 0; --i) {
        const RegisterSpan span = m_psIntSpans[i - 1];
        recordFailure(m_device->SetPixelShaderConstantI(
            span.start, m_psIntValues + span.start * 4, span.count));
    }
    for (UINT i = m_psFloatSpanCount; i > 0; --i) {
        const RegisterSpan span = m_psFloatSpans[i - 1];
        recordFailure(m_device->SetPixelShaderConstantF(
            span.start, m_psFloatValues + span.start * 4, span.count));
    }
    if (m_pixelShaderCaptured) recordFailure(m_device->SetPixelShader(m_pixelShader));

    for (UINT i = m_vsBoolSpanCount; i > 0; --i) {
        const RegisterSpan span = m_vsBoolSpans[i - 1];
        recordFailure(m_device->SetVertexShaderConstantB(
            span.start, m_vsBoolValues + span.start, span.count));
    }
    for (UINT i = m_vsIntSpanCount; i > 0; --i) {
        const RegisterSpan span = m_vsIntSpans[i - 1];
        recordFailure(m_device->SetVertexShaderConstantI(
            span.start, m_vsIntValues + span.start * 4, span.count));
    }
    for (UINT i = m_vsFloatSpanCount; i > 0; --i) {
        const RegisterSpan span = m_vsFloatSpans[i - 1];
        recordFailure(m_device->SetVertexShaderConstantF(
            span.start, m_vsFloatValues + span.start * 4, span.count));
    }
    if (m_vertexShaderCaptured) recordFailure(m_device->SetVertexShader(m_vertexShader));
    if (m_fvfCaptured) recordFailure(m_device->SetFVF(m_fvf));
    if (m_nPatchCaptured) recordFailure(m_device->SetNPatchMode(m_nPatchMode));

    for (UINT i = m_samplerTouchedCount; i > 0; --i) {
        const UINT key = m_samplerTouched[i - 1];
        const UINT slot = key / kSamplerStateCount;
        const UINT type = key % kSamplerStateCount;
        const DWORD sampler = slot < 16
            ? slot
            : D3DVERTEXTEXTURESAMPLER0 + slot - 16;
        recordFailure(m_device->SetSamplerState(
            sampler, static_cast<D3DSAMPLERSTATETYPE>(type), m_samplerValues[slot][type]));
    }
    for (UINT i = m_textureStageTouchedCount; i > 0; --i) {
        const UINT key = m_textureStageTouched[i - 1];
        const UINT stage = key / kTextureStageStateCount;
        const UINT type = key % kTextureStageStateCount;
        recordFailure(m_device->SetTextureStageState(
            stage, static_cast<D3DTEXTURESTAGESTATETYPE>(type),
            m_textureStageValues[stage][type]));
    }
    for (UINT i = m_textureTouchedCount; i > 0; --i) {
        const UINT slot = m_textureTouched[i - 1];
        const DWORD stage = slot < 16
            ? slot
            : D3DVERTEXTEXTURESAMPLER0 + slot - 16;
        recordFailure(m_device->SetTexture(stage, m_textureValues[slot]));
    }
    for (UINT i = m_renderStateTouchedCount; i > 0; --i) {
        const UINT state = m_renderStateTouched[i - 1];
        recordFailure(m_device->SetRenderState(
            static_cast<D3DRENDERSTATETYPE>(state), m_renderStateValues[state]));
    }
    for (auto it = m_lightEnables.rbegin(); it != m_lightEnables.rend(); ++it) {
        recordFailure(m_device->LightEnable(it->index, it->value));
    }
    for (auto it = m_lights.rbegin(); it != m_lights.rend(); ++it) {
        recordFailure(m_device->SetLight(it->index, &it->value));
    }
    if (m_materialCaptured) recordFailure(m_device->SetMaterial(&m_material));
    for (auto it = m_transforms.rbegin(); it != m_transforms.rend(); ++it) {
        recordFailure(m_device->SetTransform(it->state, &it->value));
    }

    m_active = false;
    m_hasCurrentPass = false;
    m_passActive = false;
    if (FAILED(result)) {
        Fail(result);
        m_disabled = true;
    }
    ClearJournal();
    finishAttribution();
    return result;
}
