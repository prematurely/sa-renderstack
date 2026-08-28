#pragma once

#include "d3d9_interfaces.h"

#include "../dxvk/dxvk_stats.h"

#include <array>
#include <atomic>
#include <mutex>
#include <vector>

namespace dxvk {

  class D3D9DeviceEx;

  enum class D3D9GtaSaStateAuditKind : uint32_t {
    RenderState,
    SamplerState,
    TextureStageState,
    Texture,
    VertexShader,
    PixelShader,
    VertexFloatConstants,
    VertexIntConstants,
    VertexBoolConstants,
    PixelFloatConstants,
    PixelIntConstants,
    PixelBoolConstants,
    StateBlockCreate,
    StateBlockBeginRecord,
    StateBlockEndRecord,
    StateBlockCapture,
    StateBlockApply,
    Count,
  };

  struct D3D9GtaSaStateAuditCounter {
    uint64_t Calls = 0u;
    uint64_t RedundantCalls = 0u;
    uint64_t Units = 0u;
    uint64_t RedundantUnits = 0u;
    uint64_t QpcTicks = 0u;
    uint64_t MaxQpcTicks = 0u;
  };

  enum class D3D9GtaSaDrawKind : uint32_t {
    Primitive,
    Indexed,
    PrimitiveUP,
    IndexedUP,
    Count,
  };

  enum class D3D9GtaSaBatchBreakReason : uint32_t {
    Command,
    DrawType,
    ChunkFull,
    Flush,
    Present,
    Count,
  };

  struct D3D9GtaSaBatchAudit {
    static constexpr uint32_t BatchHistogramMax = 256u;

    std::array<uint64_t,
      uint32_t(D3D9GtaSaDrawKind::Count)> DrawCalls = { };
    std::array<uint64_t,
      uint32_t(D3D9GtaSaBatchBreakReason::Count)> BatchBreaks = { };
    std::array<uint64_t, BatchHistogramMax + 1u> BatchSizeHistogram = { };
    uint64_t DirectDrawsContinued = 0u;
    uint64_t BatchCount = 0u;
    uint64_t BatchDraws = 0u;
    uint64_t MaxBatchSize = 0u;

    uint32_t GetP95BatchSize() const {
      if (BatchCount == 0u)
        return 0u;

      const uint64_t target = (BatchCount * 95u + 99u) / 100u;
      uint64_t cumulative = 0u;
      for (uint32_t i = 1u; i <= BatchHistogramMax; i++) {
        cumulative += BatchSizeHistogram[i];
        if (cumulative >= target)
          return i;
      }

      return BatchHistogramMax;
    }
  };

  enum class D3D9GtaSaPrepareDrawAuditKind : uint32_t {
    Hazard,
    BufferUpload,
    TextureUpload,
    MipGeneration,
    Fog,
    Framebuffer,
    ViewportScissor,
    Sampler,
    Texture,
    Blend,
    DepthStencil,
    Rasterizer,
    DepthBias,
    MultiSample,
    AlphaTest,
    ClipPlanes,
    VertexConstants,
    PixelConstants,
    FixedFunctionVertex,
    FixedFunctionPixel,
    InputLayout,
    SharedPixelData,
    DepthBounds,
    Specialization,
    VertexBuffers,
    IndexBuffer,
    PushData,
    PrimitiveType,
    Count,
  };

  struct D3D9GtaSaPrepareDrawAuditCounter {
    uint64_t Draws = 0u;
    uint64_t Units = 0u;
  };

  struct D3D9GtaSaPrepareDrawAudit {
    static constexpr uint32_t CategoryCount =
      uint32_t(D3D9GtaSaPrepareDrawAuditKind::Count);

    std::array<D3D9GtaSaPrepareDrawAuditCounter, CategoryCount> Counters = { };
    std::array<uint64_t, CategoryCount + 1u> CategoryCountHistogram = { };
    uint64_t Draws = 0u;
    uint64_t CleanDraws = 0u;
    uint64_t CategoryEvents = 0u;
    uint64_t MaxCategoriesPerDraw = 0u;

    uint32_t GetP95CategoryCount() const {
      if (Draws == 0u)
        return 0u;

      const uint64_t target = (Draws * 95u + 99u) / 100u;
      uint64_t cumulative = 0u;
      for (uint32_t i = 0u; i <= CategoryCount; i++) {
        cumulative += CategoryCountHistogram[i];
        if (cumulative >= target)
          return i;
      }

      return CategoryCount;
    }
  };

  struct D3D9GtaSaDeferredShaderBindingAuditCounter {
    uint64_t Writes = 0u;
    uint64_t Flushes = 0u;
    uint64_t Binds = 0u;
    uint64_t Coalesced = 0u;
  };

  class D3D9GtaSaCompatDevice final : public ID3D9GtaSaCompatDevice6 {

  public:

    D3D9GtaSaCompatDevice(
            D3D9DeviceEx*         device,
            bool                  enabled,
            bool                  diagnostics);

    ULONG STDMETHODCALLTYPE AddRef();

    ULONG STDMETHODCALLTYPE Release();

    HRESULT STDMETHODCALLTYPE QueryInterface(
            REFIID                riid,
            void**                ppvObject);

    HRESULT STDMETHODCALLTYPE GetStatus(
            D3D9GtaSaCompatStatus* pStatus);

    HRESULT STDMETHODCALLTYPE GetVulkanInterop(
            ID3D9VkInteropDevice** ppInterop);

    HRESULT STDMETHODCALLTYPE RegisterVulkanPass(
      const D3D9GtaSaVulkanPassDesc* pDesc,
            UINT64*                   pToken);

    HRESULT STDMETHODCALLTYPE UnregisterVulkanPass(
            UINT64                    token);

    HRESULT STDMETHODCALLTYPE SubmitStateBatch(
      const D3D9GtaSaStateBatch*      pBatch);

    HRESULT STDMETHODCALLTYPE BeginStateJournal();

    HRESULT STDMETHODCALLTYPE RestoreStateJournal();

    HRESULT STDMETHODCALLTYPE SubmitEffectStateBatch(
      const D3D9GtaSaEffectStateBatch* pBatch);

    HRESULT STDMETHODCALLTYPE SubmitStateDrawBatch(
      const D3D9GtaSaStateDrawBatch* pBatch);

    HRESULT STDMETHODCALLTYPE BeginSelectiveStateJournal();

    HRESULT STDMETHODCALLTYPE SetStateJournalCaptureEnabled(
            BOOL Enable);

    bool IsEnabled() const {
      return m_enabled;
    }

    void OnDeviceReady(
      const D3DPRESENT_PARAMETERS* params);

    void OnResetBegin();

    void OnResetEnd(
            HRESULT                result,
      const D3DPRESENT_PARAMETERS* params);

    void OnPresent();

    void OnDeviceDestroy();

    bool IsStateAuditActive() const {
      return m_stateAuditActive;
    }

    void RecordStateAudit(
            D3D9GtaSaStateAuditKind kind,
            bool                    redundant = false,
            uint32_t                units = 1u) {
      if (!m_stateAuditActive)
        return;

      auto& counter = m_stateAuditCounters[uint32_t(kind)];
      counter.Calls += 1u;
      counter.Units += units;

      if (redundant) {
        counter.RedundantCalls += 1u;
        counter.RedundantUnits += units;
      }
    }

    uint64_t BeginStateAuditTiming() const;

    void RecordStateAuditTiming(
            D3D9GtaSaStateAuditKind kind,
            uint64_t                beginQpc);

    void RecordStateBlockFastSkip() {
      if (m_stateAuditActive)
        m_stateBlockFastSkips += 1u;
    }

    void RecordD3D9Draw(
            D3D9GtaSaDrawKind kind,
            bool              continued);

    void RecordD3D9BatchComplete(
            uint32_t                    batchSize,
            D3D9GtaSaBatchBreakReason   reason);

    void RecordPrepareDrawAudit(
            uint32_t                    categoryMask,
            uint32_t                    bufferUploadUnits,
            uint32_t                    textureUploadUnits,
            uint32_t                    mipGenerationUnits,
            uint32_t                    samplerUnits,
            uint32_t                    textureUnits);

    void RecordDeferredShaderBindingWrite(D3D9ShaderType shaderType) {
      if (!m_stateAuditActive)
        return;

      m_deferredShaderBindingAudit[uint32_t(shaderType)].Writes += 1u;
    }

    void RecordDeferredShaderBindingResolve(
            D3D9ShaderType shaderType,
            bool           bound) {
      if (!m_stateAuditActive)
        return;

      auto& counter = m_deferredShaderBindingAudit[uint32_t(shaderType)];
      counter.Flushes += 1u;
      if (bound)
        counter.Binds += 1u;
      else
        counter.Coalesced += 1u;
    }

    bool HasVulkanPasses() const {
      return m_passCount.load(std::memory_order_relaxed) != 0u;
    }

    void RunVulkanPasses(
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
            VkImageUsageFlags         outputUsage);

  private:

    void UpdatePresentationState(
      const D3DPRESENT_PARAMETERS* params);

    void StartStateAudit();

    void FinishStateAudit(const char* reason);

    struct VulkanPassEntry {
      UINT64                      Token = 0u;
      UINT64                      Sequence = 0u;
      INT                         Priority = 0;
      UINT                        Stage = 0u;
      UINT                        Flags = 0u;
      std::array<char, 64>        Name = { };
      D3D9GtaSaRecordVulkanPass   Record = nullptr;
      void*                       UserData = nullptr;
      bool                        Enabled = true;
    };

    D3D9DeviceEx*        m_device;
    const bool           m_enabled;
    const bool           m_diagnostics;

    std::atomic<UINT>    m_flags              = { 0u };
    std::atomic<UINT>    m_backBufferWidth    = { 0u };
    std::atomic<UINT>    m_backBufferHeight   = { 0u };
    std::atomic<UINT>    m_backBufferFormat   = { D3DFMT_UNKNOWN };
    std::atomic<UINT>    m_presentInterval    = { 0u };
    std::atomic<UINT64>  m_presentCount       = { 0u };
    std::atomic<UINT64>  m_resetCount         = { 0u };
    std::atomic<UINT64>  m_failedResetCount   = { 0u };
    std::atomic<UINT64>  m_nextPassToken      = { 1u };
    std::atomic<UINT64>  m_nextPassSequence   = { 1u };
    std::atomic<UINT>    m_passCount          = { 0u };

    bool                        m_stateAuditActive         = false;
    bool                        m_stateAuditTriggerWasDown = false;
    UINT64                      m_stateAuditStartMs        = 0u;
    UINT64                      m_stateAuditFrames         = 0u;
    int64_t                     m_stateAuditQpcFrequency   = 0;
    std::array<D3D9GtaSaStateAuditCounter,
      uint32_t(D3D9GtaSaStateAuditKind::Count)> m_stateAuditCounters = { };
    D3D9GtaSaBatchAudit         m_d3d9BatchAudit = { };
    D3D9GtaSaPrepareDrawAudit   m_prepareDrawAudit = { };
    std::array<D3D9GtaSaDeferredShaderBindingAuditCounter,
      uint32_t(D3D9ShaderType::PixelShader) + 1u> m_deferredShaderBindingAudit = { };
    DxvkStatCounters             m_stateAuditDxvkCounters = { };
    uint64_t                     m_stateAuditSamplerBindingCacheHits = 0u;
    uint64_t                     m_stateAuditTextureBindingCacheHits = 0u;
    uint64_t                     m_stateAuditResourceBindingCacheInvalidations = 0u;
    uint64_t                     m_stateBlockFastSkips = 0u;

    std::mutex                   m_passExecutionMutex;
    std::mutex                   m_passMutex;
    std::vector<VulkanPassEntry> m_passes;
  };

}
