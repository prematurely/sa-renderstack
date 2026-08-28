#include "d3d9_gta_sa_compat.h"

#include "d3d9_device.h"

#include "../util/util_win32_compat.h"

#include <algorithm>
#include <cstring>

namespace dxvk {

  D3D9GtaSaCompatDevice::D3D9GtaSaCompatDevice(
          D3D9DeviceEx* device,
          bool          enabled,
          bool          diagnostics)
  : m_device(device)
  , m_enabled(enabled)
  , m_diagnostics(diagnostics) {
    if (!m_enabled)
      return;

    m_flags.store(
      D3D9_GTA_SA_COMPAT_ACTIVE
    | D3D9_GTA_SA_COMPAT_VULKAN_BACKEND
    | D3D9_GTA_SA_COMPAT_VULKAN_INTEROP
    | D3D9_GTA_SA_COMPAT_FRAME_LIFECYCLE
    | D3D9_GTA_SA_COMPAT_PASS_REGISTRY
    | D3D9_GTA_SA_COMPAT_COMMAND_RECORD
    | D3D9_GTA_SA_COMPAT_STATE_BATCH
    | D3D9_GTA_SA_COMPAT_STATE_JOURNAL
    | D3D9_GTA_SA_COMPAT_EFFECT_STATE_BATCH
    | D3D9_GTA_SA_COMPAT_STATE_DRAW_BATCH
    | D3D9_GTA_SA_COMPAT_SELECTIVE_STATE_JOURNAL);

    if (m_diagnostics) {
      LARGE_INTEGER frequency = { };
      if (QueryPerformanceFrequency(&frequency))
        m_stateAuditQpcFrequency = frequency.QuadPart;
      Logger::info("GTA SA compatibility runtime v7 enabled");
    }
  }


  ULONG STDMETHODCALLTYPE D3D9GtaSaCompatDevice::AddRef() {
    return m_device->AddRef();
  }


  ULONG STDMETHODCALLTYPE D3D9GtaSaCompatDevice::Release() {
    return m_device->Release();
  }


  HRESULT STDMETHODCALLTYPE D3D9GtaSaCompatDevice::QueryInterface(
          REFIID riid,
          void** ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown)
     || riid == __uuidof(ID3D9GtaSaCompatDevice)
     || riid == __uuidof(ID3D9GtaSaCompatDevice1)
      || riid == __uuidof(ID3D9GtaSaCompatDevice2)
      || riid == __uuidof(ID3D9GtaSaCompatDevice3)
      || riid == __uuidof(ID3D9GtaSaCompatDevice4)
      || riid == __uuidof(ID3D9GtaSaCompatDevice5)
      || riid == __uuidof(ID3D9GtaSaCompatDevice6)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    return m_device->QueryInterface(riid, ppvObject);
  }


  HRESULT STDMETHODCALLTYPE D3D9GtaSaCompatDevice::GetStatus(
          D3D9GtaSaCompatStatus* pStatus) {
    if (pStatus == nullptr)
      return E_POINTER;

    if (pStatus->StructSize < sizeof(D3D9GtaSaCompatStatus))
      return D3DERR_INVALIDCALL;

    D3D9GtaSaCompatStatus status = { };
    status.StructSize       = sizeof(status);
    status.ApiVersion       = D3D9_GTA_SA_COMPAT_API_VERSION;
    status.Flags            = m_flags.load();
    status.BackBufferWidth  = m_backBufferWidth.load();
    status.BackBufferHeight = m_backBufferHeight.load();
    status.BackBufferFormat = D3DFORMAT(m_backBufferFormat.load());
    status.PresentInterval  = m_presentInterval.load();
    status.PresentCount     = m_presentCount.load();
    status.ResetCount       = m_resetCount.load();
    status.FailedResetCount = m_failedResetCount.load();

    *pStatus = status;
    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9GtaSaCompatDevice::GetVulkanInterop(
          ID3D9VkInteropDevice** ppInterop) {
    if (ppInterop == nullptr)
      return E_POINTER;

    *ppInterop = nullptr;

    if (!m_enabled)
      return E_NOINTERFACE;

    return m_device->QueryInterface(
      __uuidof(ID3D9VkInteropDevice),
      reinterpret_cast<void**>(ppInterop));
  }


  HRESULT STDMETHODCALLTYPE D3D9GtaSaCompatDevice::RegisterVulkanPass(
    const D3D9GtaSaVulkanPassDesc* pDesc,
          UINT64*                   pToken) {
    if (pDesc == nullptr || pToken == nullptr)
      return E_POINTER;

    *pToken = 0u;

    if (!m_enabled)
      return E_NOINTERFACE;

    if (pDesc->StructSize < sizeof(D3D9GtaSaVulkanPassDesc)
     || pDesc->ApiVersion < 2u
     || pDesc->ApiVersion > D3D9_GTA_SA_COMPAT_API_VERSION
     || pDesc->Stage != D3D9_GTA_SA_VULKAN_PASS_AFTER_BLIT
     || !(pDesc->Flags & D3D9_GTA_SA_VULKAN_PASS_RESTORES_LAYOUTS)
     || pDesc->Record == nullptr)
      return D3DERR_INVALIDCALL;

    VulkanPassEntry entry;
    entry.Token = m_nextPassToken.fetch_add(1u);
    entry.Sequence = m_nextPassSequence.fetch_add(1u);
    entry.Priority = pDesc->Priority;
    entry.Stage = pDesc->Stage;
    entry.Flags = pDesc->Flags;
    entry.Record = pDesc->Record;
    entry.UserData = pDesc->UserData;
    std::memcpy(entry.Name.data(), pDesc->Name, entry.Name.size());
    entry.Name.back() = '\0';

    {
      std::lock_guard lock(m_passMutex);
      m_passes.push_back(entry);
      std::stable_sort(m_passes.begin(), m_passes.end(),
        [] (const VulkanPassEntry& a, const VulkanPassEntry& b) {
          if (a.Priority != b.Priority)
            return a.Priority < b.Priority;
          return a.Sequence < b.Sequence;
        });
      m_passCount.fetch_add(1u, std::memory_order_relaxed);
    }

    *pToken = entry.Token;

    if (m_diagnostics)
      Logger::info(str::format("GTA SA Vulkan pass registered: ", entry.Name.data(),
        ", priority=", entry.Priority, ", token=", entry.Token));

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9GtaSaCompatDevice::SubmitStateBatch(
    const D3D9GtaSaStateBatch* pBatch) {
    if (!m_enabled)
      return E_NOINTERFACE;

    return m_device->SubmitGtaSaStateBatch(pBatch);
  }


  HRESULT STDMETHODCALLTYPE D3D9GtaSaCompatDevice::BeginStateJournal() {
    if (!m_enabled)
      return E_NOINTERFACE;

    return m_device->BeginGtaSaStateJournal();
  }


  HRESULT STDMETHODCALLTYPE D3D9GtaSaCompatDevice::RestoreStateJournal() {
    if (!m_enabled)
      return E_NOINTERFACE;

    return m_device->RestoreGtaSaStateJournal();
  }


  HRESULT STDMETHODCALLTYPE D3D9GtaSaCompatDevice::SubmitEffectStateBatch(
    const D3D9GtaSaEffectStateBatch* pBatch) {
    if (!m_enabled)
      return E_NOINTERFACE;

    return m_device->SubmitGtaSaEffectStateBatch(pBatch);
  }


  HRESULT STDMETHODCALLTYPE D3D9GtaSaCompatDevice::SubmitStateDrawBatch(
    const D3D9GtaSaStateDrawBatch* pBatch) {
    if (!m_enabled)
      return E_NOINTERFACE;

    return m_device->SubmitGtaSaStateDrawBatch(pBatch);
  }


  HRESULT STDMETHODCALLTYPE D3D9GtaSaCompatDevice::BeginSelectiveStateJournal() {
    if (!m_enabled)
      return E_NOINTERFACE;

    return m_device->BeginGtaSaStateJournal(true);
  }


  HRESULT STDMETHODCALLTYPE D3D9GtaSaCompatDevice::SetStateJournalCaptureEnabled(
          BOOL Enable) {
    if (!m_enabled)
      return E_NOINTERFACE;

    return m_device->SetGtaSaStateJournalCaptureEnabled(Enable);
  }


  HRESULT STDMETHODCALLTYPE D3D9GtaSaCompatDevice::UnregisterVulkanPass(
          UINT64 token) {
    if (!m_enabled)
      return E_NOINTERFACE;

    // Keep the callback target alive until any in-flight recording has ended.
    // Record callbacks must not call registry methods themselves.
    std::lock_guard executionLock(m_passExecutionMutex);
    std::lock_guard lock(m_passMutex);

    auto entry = std::find_if(m_passes.begin(), m_passes.end(),
      [token] (const VulkanPassEntry& pass) { return pass.Token == token; });

    if (entry == m_passes.end())
      return D3DERR_NOTFOUND;

    if (entry->Enabled)
      m_passCount.fetch_sub(1u, std::memory_order_relaxed);

    if (m_diagnostics)
      Logger::info(str::format("GTA SA Vulkan pass unregistered: ", entry->Name.data(),
        ", token=", entry->Token));

    m_passes.erase(entry);
    return D3D_OK;
  }


  void D3D9GtaSaCompatDevice::OnDeviceReady(
    const D3DPRESENT_PARAMETERS* params) {
    if (!m_enabled)
      return;

    UpdatePresentationState(params);
    m_flags.fetch_or(D3D9_GTA_SA_COMPAT_DEVICE_READY);

    if (!m_diagnostics)
      return;

    const auto& device = m_device->GetDXVKDevice();
    const auto queue = device->queues().graphics;

    Logger::info(str::format(
      "GTA SA Vulkan device ready: queue family ", queue.queueFamily,
      ", queue index ", queue.queueIndex,
      ", backbuffer ", m_backBufferWidth.load(), "x", m_backBufferHeight.load(),
      ", format ", m_backBufferFormat.load()));
  }


  void D3D9GtaSaCompatDevice::OnResetBegin() {
    if (!m_enabled)
      return;

    m_device->DiscardGtaSaStateJournal();

    if (m_stateAuditActive)
      FinishStateAudit("device-reset");

    m_resetCount.fetch_add(1u);
    m_flags.fetch_and(~UINT(D3D9_GTA_SA_COMPAT_DEVICE_READY));
  }


  void D3D9GtaSaCompatDevice::OnResetEnd(
          HRESULT result,
    const D3DPRESENT_PARAMETERS* params) {
    if (!m_enabled)
      return;

    if (SUCCEEDED(result)) {
      UpdatePresentationState(params);
      m_flags.fetch_or(D3D9_GTA_SA_COMPAT_DEVICE_READY);
    } else {
      m_failedResetCount.fetch_add(1u);
    }

    if (m_diagnostics)
      Logger::info(str::format("GTA SA device reset result: 0x", std::hex, uint32_t(result)));
  }


  void D3D9GtaSaCompatDevice::OnPresent() {
    if (!m_enabled)
      return;

    m_presentCount.fetch_add(1u, std::memory_order_relaxed);

    if (!m_diagnostics)
      return;

    if (m_stateAuditActive) {
      m_stateAuditFrames += 1u;
      if (GetTickCount64() - m_stateAuditStartMs >= 3000u)
        FinishStateAudit("duration-complete");
    }

    const bool triggerDown = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    if (!triggerDown) {
      m_stateAuditTriggerWasDown = false;
    } else if (!m_stateAuditTriggerWasDown) {
      m_stateAuditTriggerWasDown = true;
      if (!m_stateAuditActive)
        StartStateAudit();
    }
  }


  void D3D9GtaSaCompatDevice::OnDeviceDestroy() {
    if (m_enabled)
      m_device->DiscardGtaSaStateJournal();

    if (m_stateAuditActive)
      FinishStateAudit("device-destroyed");

    std::lock_guard executionLock(m_passExecutionMutex);

    {
      std::lock_guard lock(m_passMutex);
      if (!m_passes.empty() && m_diagnostics)
        Logger::warn(str::format("GTA SA compatibility runtime released ",
          m_passes.size(), " registered Vulkan pass(es)"));
      m_passes.clear();
      m_passCount.store(0u, std::memory_order_relaxed);
    }

    if (m_enabled && m_diagnostics) {
      Logger::info(str::format(
        "GTA SA compatibility runtime stopped: presents=", m_presentCount.load(),
        ", resets=", m_resetCount.load(),
        ", failed resets=", m_failedResetCount.load()));
    }
  }


  uint64_t D3D9GtaSaCompatDevice::BeginStateAuditTiming() const {
    if (!m_stateAuditActive || m_stateAuditQpcFrequency <= 0)
      return 0u;

    LARGE_INTEGER value = { };
    QueryPerformanceCounter(&value);
    return uint64_t(value.QuadPart);
  }


  void D3D9GtaSaCompatDevice::RecordStateAuditTiming(
          D3D9GtaSaStateAuditKind kind,
          uint64_t                beginQpc) {
    if (!m_stateAuditActive || beginQpc == 0u)
      return;

    LARGE_INTEGER value = { };
    QueryPerformanceCounter(&value);
    const uint64_t endQpc = uint64_t(value.QuadPart);
    const uint64_t elapsed = endQpc >= beginQpc ? endQpc - beginQpc : 0u;

    RecordStateAudit(kind);
    auto& counter = m_stateAuditCounters[uint32_t(kind)];
    counter.QpcTicks += elapsed;
    counter.MaxQpcTicks = std::max(counter.MaxQpcTicks, elapsed);
  }


  void D3D9GtaSaCompatDevice::RecordD3D9Draw(
          D3D9GtaSaDrawKind kind,
          bool              continued) {
    if (!m_stateAuditActive)
      return;

    const uint32_t index = uint32_t(kind);
    if (index >= uint32_t(D3D9GtaSaDrawKind::Count))
      return;

    m_d3d9BatchAudit.DrawCalls[index] += 1u;
    if (continued)
      m_d3d9BatchAudit.DirectDrawsContinued += 1u;
  }


  void D3D9GtaSaCompatDevice::RecordD3D9BatchComplete(
          uint32_t                   batchSize,
          D3D9GtaSaBatchBreakReason  reason) {
    if (!m_stateAuditActive || batchSize == 0u)
      return;

    const uint32_t reasonIndex = uint32_t(reason);
    if (reasonIndex < uint32_t(D3D9GtaSaBatchBreakReason::Count))
      m_d3d9BatchAudit.BatchBreaks[reasonIndex] += 1u;

    const uint32_t bucket = std::min(
      batchSize, D3D9GtaSaBatchAudit::BatchHistogramMax);
    m_d3d9BatchAudit.BatchSizeHistogram[bucket] += 1u;
    m_d3d9BatchAudit.BatchCount += 1u;
    m_d3d9BatchAudit.BatchDraws += batchSize;
    m_d3d9BatchAudit.MaxBatchSize = std::max(
      m_d3d9BatchAudit.MaxBatchSize, uint64_t(batchSize));
  }


  void D3D9GtaSaCompatDevice::RecordPrepareDrawAudit(
          uint32_t categoryMask,
          uint32_t bufferUploadUnits,
          uint32_t textureUploadUnits,
          uint32_t mipGenerationUnits,
          uint32_t samplerUnits,
          uint32_t textureUnits) {
    if (!m_stateAuditActive)
      return;

    auto getUnits = [
      bufferUploadUnits,
      textureUploadUnits,
      mipGenerationUnits,
      samplerUnits,
      textureUnits
    ] (D3D9GtaSaPrepareDrawAuditKind kind) {
      switch (kind) {
        case D3D9GtaSaPrepareDrawAuditKind::BufferUpload:
          return bufferUploadUnits;
        case D3D9GtaSaPrepareDrawAuditKind::TextureUpload:
          return textureUploadUnits;
        case D3D9GtaSaPrepareDrawAuditKind::MipGeneration:
          return mipGenerationUnits;
        case D3D9GtaSaPrepareDrawAuditKind::Sampler:
          return samplerUnits;
        case D3D9GtaSaPrepareDrawAuditKind::Texture:
          return textureUnits;
        default:
          return 1u;
      }
    };

    uint32_t categoryCount = 0u;
    for (uint32_t i = 0u; i < D3D9GtaSaPrepareDrawAudit::CategoryCount; i++) {
      if ((categoryMask & (1u << i)) == 0u)
        continue;

      auto& counter = m_prepareDrawAudit.Counters[i];
      counter.Draws += 1u;
      counter.Units += getUnits(D3D9GtaSaPrepareDrawAuditKind(i));
      categoryCount += 1u;
    }

    m_prepareDrawAudit.Draws += 1u;
    m_prepareDrawAudit.CategoryEvents += categoryCount;
    m_prepareDrawAudit.MaxCategoriesPerDraw = std::max(
      m_prepareDrawAudit.MaxCategoriesPerDraw, uint64_t(categoryCount));
    m_prepareDrawAudit.CategoryCountHistogram[categoryCount] += 1u;

    if (categoryCount == 0u)
      m_prepareDrawAudit.CleanDraws += 1u;
  }


  void D3D9GtaSaCompatDevice::StartStateAudit() {
    for (auto& counter : m_stateAuditCounters)
      counter = { };

    m_d3d9BatchAudit = { };
    m_prepareDrawAudit = { };
    m_deferredShaderBindingAudit = { };
    m_stateBlockFastSkips = 0u;
    m_stateAuditDxvkCounters = m_device->GetDXVKDevice()->getStatCounters();
    m_stateAuditSamplerBindingCacheHits =
      m_device->GetGtaSaSamplerBindingCacheHits();
    m_stateAuditTextureBindingCacheHits =
      m_device->GetGtaSaTextureBindingCacheHits();
    m_stateAuditResourceBindingCacheInvalidations =
      m_device->GetGtaSaResourceBindingCacheInvalidations();
    m_stateAuditFrames = 0u;
    m_stateAuditStartMs = GetTickCount64();
    m_stateAuditActive = true;
    Logger::info("GTA SA D3D9 state audit started: key=F7 durationMs=3000 mode=read-only");
  }


  void D3D9GtaSaCompatDevice::FinishStateAudit(const char* reason) {
    if (!m_stateAuditActive)
      return;

    const UINT64 elapsedMs = GetTickCount64() - m_stateAuditStartMs;
    m_stateAuditActive = false;

    static const char* names[] = {
      "renderState",
      "samplerState",
      "textureStageState",
      "texture",
      "vertexShader",
      "pixelShader",
      "vertexFloatConstants",
      "vertexIntConstants",
      "vertexBoolConstants",
      "pixelFloatConstants",
      "pixelIntConstants",
      "pixelBoolConstants",
      "stateBlockCreate",
      "stateBlockBeginRecord",
      "stateBlockEndRecord",
      "stateBlockCapture",
      "stateBlockApply",
    };
    static_assert(sizeof(names) / sizeof(names[0]) == uint32_t(D3D9GtaSaStateAuditKind::Count));

    uint64_t totalCalls = 0u;
    uint64_t totalRedundantCalls = 0u;
    for (const auto& counter : m_stateAuditCounters) {
      totalCalls += counter.Calls;
      totalRedundantCalls += counter.RedundantCalls;
    }

    const uint64_t directDraws =
      m_d3d9BatchAudit.DrawCalls[uint32_t(D3D9GtaSaDrawKind::Primitive)]
      + m_d3d9BatchAudit.DrawCalls[uint32_t(D3D9GtaSaDrawKind::Indexed)];
    const uint32_t batchP95 = m_d3d9BatchAudit.GetP95BatchSize();

    const DxvkStatCounters dxvkCounters =
      m_device->GetDXVKDevice()->getStatCounters().diff(m_stateAuditDxvkCounters);

    const uint64_t samplerBindingCacheHits =
      m_device->GetGtaSaSamplerBindingCacheHits()
      - m_stateAuditSamplerBindingCacheHits;
    const uint64_t textureBindingCacheHits =
      m_device->GetGtaSaTextureBindingCacheHits()
      - m_stateAuditTextureBindingCacheHits;
    const uint64_t resourceBindingCacheInvalidations =
      m_device->GetGtaSaResourceBindingCacheInvalidations()
      - m_stateAuditResourceBindingCacheInvalidations;

    Logger::info(str::format(
      "GTA SA D3D9 batch audit: draws=",
      m_d3d9BatchAudit.DrawCalls[uint32_t(D3D9GtaSaDrawKind::Primitive)]
        + m_d3d9BatchAudit.DrawCalls[uint32_t(D3D9GtaSaDrawKind::Indexed)]
        + m_d3d9BatchAudit.DrawCalls[uint32_t(D3D9GtaSaDrawKind::PrimitiveUP)]
        + m_d3d9BatchAudit.DrawCalls[uint32_t(D3D9GtaSaDrawKind::IndexedUP)],
      " dp=", m_d3d9BatchAudit.DrawCalls[uint32_t(D3D9GtaSaDrawKind::Primitive)],
      " dip=", m_d3d9BatchAudit.DrawCalls[uint32_t(D3D9GtaSaDrawKind::Indexed)],
      " dpup=", m_d3d9BatchAudit.DrawCalls[uint32_t(D3D9GtaSaDrawKind::PrimitiveUP)],
      " dipup=", m_d3d9BatchAudit.DrawCalls[uint32_t(D3D9GtaSaDrawKind::IndexedUP)],
      " directDraws=", directDraws,
      " continued=", m_d3d9BatchAudit.DirectDrawsContinued,
      " producerBatches=", m_d3d9BatchAudit.BatchCount,
      " producerBatchDraws=", m_d3d9BatchAudit.BatchDraws,
      " producerAvgBatch=", m_d3d9BatchAudit.BatchCount
        ? double(m_d3d9BatchAudit.BatchDraws) / double(m_d3d9BatchAudit.BatchCount)
        : 0.0,
      " producerP95Batch=", batchP95,
      " producerMaxBatch=", m_d3d9BatchAudit.MaxBatchSize,
      " breakCommand=", m_d3d9BatchAudit.BatchBreaks[
        uint32_t(D3D9GtaSaBatchBreakReason::Command)],
      " breakDrawType=", m_d3d9BatchAudit.BatchBreaks[
        uint32_t(D3D9GtaSaBatchBreakReason::DrawType)],
      " breakChunkFull=", m_d3d9BatchAudit.BatchBreaks[
        uint32_t(D3D9GtaSaBatchBreakReason::ChunkFull)],
      " breakFlush=", m_d3d9BatchAudit.BatchBreaks[
        uint32_t(D3D9GtaSaBatchBreakReason::Flush)],
      " breakPresent=", m_d3d9BatchAudit.BatchBreaks[
        uint32_t(D3D9GtaSaBatchBreakReason::Present)]));

    Logger::info(str::format(
      "GTA SA DXVK batch audit: cmdDrawCalls=",
      dxvkCounters.getCtr(DxvkStatCounter::CmdDrawCalls),
      " cmdDrawsMerged=", dxvkCounters.getCtr(DxvkStatCounter::CmdDrawsMerged),
      " multiDrawCalls=", dxvkCounters.getCtr(DxvkStatCounter::CmdDrawMultiCalls),
      " multiDraws=", dxvkCounters.getCtr(DxvkStatCounter::CmdDrawMultiDraws),
      " barrierBatches=", dxvkCounters.getCtr(
        DxvkStatCounter::CmdDrawMultiBarrierBatches),
      " barrierDraws=", dxvkCounters.getCtr(
        DxvkStatCounter::CmdDrawMultiBarrierDraws)));

    Logger::info(str::format(
      "GTA SA resource binding cache: enabled=",
      m_device->GetOptions()->gtaSaResourceBindingCache,
      " samplerHits=", samplerBindingCacheHits,
      " textureHits=", textureBindingCacheHits,
      " invalidations=", resourceBindingCacheInvalidations));

    Logger::info(str::format(
      "GTA SA D3D9 PrepareDraw audit: draws=", m_prepareDrawAudit.Draws,
      " dirtyDraws=", m_prepareDrawAudit.Draws - m_prepareDrawAudit.CleanDraws,
      " cleanDraws=", m_prepareDrawAudit.CleanDraws,
      " avgCategoriesPerDraw=", m_prepareDrawAudit.Draws
        ? double(m_prepareDrawAudit.CategoryEvents) / double(m_prepareDrawAudit.Draws)
        : 0.0,
      " p95CategoriesPerDraw=", m_prepareDrawAudit.GetP95CategoryCount(),
      " maxCategoriesPerDraw=", m_prepareDrawAudit.MaxCategoriesPerDraw));

    static const char* prepareDrawNames[] = {
      "hazard",
      "bufferUpload",
      "textureUpload",
      "mipGeneration",
      "fog",
      "framebuffer",
      "viewportScissor",
      "sampler",
      "texture",
      "blend",
      "depthStencil",
      "rasterizer",
      "depthBias",
      "multiSample",
      "alphaTest",
      "clipPlanes",
      "vertexConstants",
      "pixelConstants",
      "fixedFunctionVertex",
      "fixedFunctionPixel",
      "inputLayout",
      "sharedPixelData",
      "depthBounds",
      "specialization",
      "vertexBuffers",
      "indexBuffer",
      "pushData",
      "primitiveType",
    };
    static_assert(sizeof(prepareDrawNames) / sizeof(prepareDrawNames[0])
      == D3D9GtaSaPrepareDrawAudit::CategoryCount);

    for (uint32_t i = 0u; i < D3D9GtaSaPrepareDrawAudit::CategoryCount; i++) {
      const auto& counter = m_prepareDrawAudit.Counters[i];
      if (counter.Draws == 0u)
        continue;

      Logger::info(str::format(
        "GTA SA D3D9 PrepareDraw category: kind=", prepareDrawNames[i],
        " draws=", counter.Draws,
        " drawsPerFrame=", m_stateAuditFrames
          ? double(counter.Draws) / double(m_stateAuditFrames)
          : 0.0,
        " drawPct=", m_prepareDrawAudit.Draws
          ? 100.0 * double(counter.Draws) / double(m_prepareDrawAudit.Draws)
          : 0.0,
        " units=", counter.Units,
        " unitsPerFrame=", m_stateAuditFrames
          ? double(counter.Units) / double(m_stateAuditFrames)
          : 0.0));
    }

    const auto& deferredVs = m_deferredShaderBindingAudit[
      uint32_t(D3D9ShaderType::VertexShader)];
    const auto& deferredPs = m_deferredShaderBindingAudit[
      uint32_t(D3D9ShaderType::PixelShader)];
    if (deferredVs.Writes != 0u || deferredPs.Writes != 0u
     || deferredVs.Flushes != 0u || deferredPs.Flushes != 0u) {
      Logger::info(str::format(
        "GTA SA D3D9 deferred shader audit: vsWrites=", deferredVs.Writes,
        " vsFlushes=", deferredVs.Flushes,
        " vsBinds=", deferredVs.Binds,
        " vsCoalesced=", deferredVs.Coalesced,
        " psWrites=", deferredPs.Writes,
        " psFlushes=", deferredPs.Flushes,
        " psBinds=", deferredPs.Binds,
        " psCoalesced=", deferredPs.Coalesced));
    }

    Logger::info(str::format(
      "GTA SA D3D9 state audit complete: reason=", reason ? reason : "unknown",
      " elapsedMs=", elapsedMs,
      " frames=", m_stateAuditFrames,
      " calls=", totalCalls,
      " redundantCalls=", totalRedundantCalls,
      " redundantPct=", totalCalls
        ? 100.0 * double(totalRedundantCalls) / double(totalCalls)
        : 0.0));

    Logger::info(str::format(
      "GTA SA D3D9 state-block fast skip: appliesSkipped=",
      m_stateBlockFastSkips,
      " skipsPerFrame=", m_stateAuditFrames
        ? double(m_stateBlockFastSkips) / double(m_stateAuditFrames)
        : 0.0));

    for (uint32_t i = 0u; i < uint32_t(D3D9GtaSaStateAuditKind::Count); i++) {
      const auto& counter = m_stateAuditCounters[i];
      if (counter.Calls == 0u)
        continue;

      const double totalMs = m_stateAuditQpcFrequency > 0
        ? 1000.0 * double(counter.QpcTicks) / double(m_stateAuditQpcFrequency)
        : 0.0;
      const double maxUs = m_stateAuditQpcFrequency > 0
        ? 1000000.0 * double(counter.MaxQpcTicks) / double(m_stateAuditQpcFrequency)
        : 0.0;

      Logger::info(str::format(
        "GTA SA D3D9 state audit counter: kind=", names[i],
        " calls=", counter.Calls,
        " callsPerFrame=", m_stateAuditFrames
          ? double(counter.Calls) / double(m_stateAuditFrames)
          : 0.0,
        " redundantCalls=", counter.RedundantCalls,
        " redundantPct=", counter.Calls
          ? 100.0 * double(counter.RedundantCalls) / double(counter.Calls)
          : 0.0,
        " units=", counter.Units,
        " redundantUnits=", counter.RedundantUnits,
        " totalMs=", totalMs,
        " maxUs=", maxUs));
    }
  }


  void D3D9GtaSaCompatDevice::RunVulkanPasses(
          UINT64                    frameId,
          VkCommandBuffer           commandBuffer,
          VkImage                   sourceImage,
          VkImageView               sourceImageView,
          VkImageLayout             sourceImageLayout,
          VkFormat                  sourceFormat,
          VkExtent2D                sourceExtent,
          VkImageSubresourceRange   sourceSubresources,
          VkImageUsageFlags         sourceUsage,
          VkImage                   outputImage,
          VkImageView               outputImageView,
          VkImageLayout             outputImageLayout,
          VkFormat                  outputFormat,
          VkExtent2D                outputExtent,
          VkImageSubresourceRange   outputSubresources,
          VkImageUsageFlags         outputUsage) {
    if (!m_enabled || !HasVulkanPasses() || commandBuffer == VK_NULL_HANDLE)
      return;

    const auto& device = m_device->GetDXVKDevice();
    D3D9GtaSaVulkanFrameContext frame = { };
    frame.StructSize = sizeof(frame);
    frame.ApiVersion = D3D9_GTA_SA_COMPAT_API_VERSION;
    frame.Stage = D3D9_GTA_SA_VULKAN_PASS_AFTER_BLIT;
    frame.FrameId = frameId;
    frame.Instance = device->instance()->handle();
    frame.PhysicalDevice = device->adapter()->handle();
    frame.Device = device->handle();
    frame.CommandBuffer = commandBuffer;
    frame.SourceImage = sourceImage;
    frame.SourceImageView = sourceImageView;
    frame.SourceImageLayout = sourceImageLayout;
    frame.SourceFormat = sourceFormat;
    frame.SourceExtent = sourceExtent;
    frame.SourceSubresources = sourceSubresources;
    frame.SourceUsage = sourceUsage;
    frame.OutputImage = outputImage;
    frame.OutputImageView = outputImageView;
    frame.OutputImageLayout = outputImageLayout;
    frame.OutputFormat = outputFormat;
    frame.OutputExtent = outputExtent;
    frame.OutputSubresources = outputSubresources;
    frame.OutputUsage = outputUsage;

    // Unregister waits on this lock before returning, so a module can safely
    // release the callback and user-data storage after unregistering its pass.
    std::lock_guard executionLock(m_passExecutionMutex);

    std::vector<VulkanPassEntry> passes;
    {
      std::lock_guard lock(m_passMutex);
      passes = m_passes;
    }

    for (const auto& pass : passes) {
      if (!pass.Enabled || pass.Stage != frame.Stage)
        continue;

      HRESULT result = E_FAIL;
      try {
        result = pass.Record(pass.UserData, &frame);
      } catch (...) {
        result = E_FAIL;
      }

      if (FAILED(result)) {
        std::lock_guard lock(m_passMutex);
        auto entry = std::find_if(m_passes.begin(), m_passes.end(),
          [&pass] (const VulkanPassEntry& candidate) {
            return candidate.Token == pass.Token;
          });

        if (entry != m_passes.end() && entry->Enabled) {
          entry->Enabled = false;
          m_passCount.fetch_sub(1u, std::memory_order_relaxed);
          Logger::err(str::format("GTA SA Vulkan pass disabled after failure: ",
            entry->Name.data(), ", result=", uint32_t(result)));
        }
      }
    }
  }


  void D3D9GtaSaCompatDevice::UpdatePresentationState(
    const D3DPRESENT_PARAMETERS* params) {
    if (params == nullptr)
      return;

    m_backBufferWidth.store(params->BackBufferWidth);
    m_backBufferHeight.store(params->BackBufferHeight);
    m_backBufferFormat.store(UINT(params->BackBufferFormat));
    m_presentInterval.store(params->PresentationInterval);
  }

}
