#pragma once

#include "d3d9_device_child.h"
#include "d3d9_device.h"
#include "d3d9_state.h"

#include "../util/util_bit.h"

#include <cstring>
#include <type_traits>

namespace dxvk {

  enum class D3D9CapturedStateFlag : uint32_t {
    VertexDecl,
    Indices,
    RenderStates,
    SamplerStates,
    VertexBuffers,
    Textures,
    VertexShader,
    PixelShader,
    Viewport,
    ScissorRect,
    ClipPlanes,
    VsConstants,
    PsConstants,
    StreamFreq,
    Transforms,
    TextureStages,
    Material,
    Lights,
    NPatchMode
  };

  using D3D9CapturedStateFlags = Flags<D3D9CapturedStateFlag>;

  struct D3D9StateCaptures {
    D3D9CapturedStateFlags flags;

    bit::bitset<RenderStateCount>                       renderStates;

    bit::bitset<SamplerCount>                           samplers;
    std::array<
      bit::bitset<SamplerStateCount>,
      SamplerCount>                                     samplerStates;

    bit::bitset<caps::MaxStreams>                       vertexBuffers;
    bit::bitset<SamplerCount>                           textures;
    bit::bitset<caps::MaxClipPlanes>                    clipPlanes;
    bit::bitset<caps::MaxStreams>                       streamFreq;
    bit::bitset<caps::MaxTransforms>                    transforms;
    bit::bitset<caps::TextureStageCount>                textureStages;
    std::array<
      bit::bitset<TextureStageStateCount>,
      caps::TextureStageCount>                          textureStageStates;

    struct {
      bit::bitset<caps::MaxFloatConstantsSoftware>      fConsts;
      bit::bitset<caps::MaxOtherConstantsSoftware>      iConsts;
      bit::bitset<caps::MaxOtherConstantsSoftware>      bConsts;
      uint32_t                                          fDwordCount = 0;
      uint32_t                                          iDwordCount = 0;
      uint32_t                                          bDwordCount = 0;
    } vsConsts;

    struct {
      bit::bitset<caps::MaxSM3FloatConstantsPS>         fConsts;
      bit::bitset<caps::MaxOtherConstants>              iConsts;
      bit::bitset<caps::MaxOtherConstants>              bConsts;
      uint32_t                                          fDwordCount = 0;
      uint32_t                                          iDwordCount = 0;
      uint32_t                                          bDwordCount = 0;
    } psConsts;

    bit::bitvector                                      lightChanges;
    bit::bitvector                                      lightEnabledChanges;
  };

  enum class D3D9StateBlockType : uint8_t {
    None,
    All,
    PixelState,
    VertexState,
    Unknown
  };

  inline D3D9StateBlockType ConvertStateBlockType(D3DSTATEBLOCKTYPE type) {
    switch (type) {
      case D3DSBT_ALL:         return D3D9StateBlockType::All;
      case D3DSBT_PIXELSTATE:  return D3D9StateBlockType::PixelState;
      case D3DSBT_VERTEXSTATE: return D3D9StateBlockType::VertexState;
      default:                 return D3D9StateBlockType::Unknown;
    }
  }

  using D3D9StateBlockBase = D3D9DeviceChild<IDirect3DStateBlock9>;
  class D3D9StateBlock : public D3D9StateBlockBase {

  public:

    D3D9StateBlock(
      D3D9DeviceEx*      pDevice,
      D3D9StateBlockType Type,
      bool               forcePrefilter = false,
      bool               countLosableResource = true,
      bool               journal = false);

    ~D3D9StateBlock();

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID  riid,
        void** ppvObject) final;

    HRESULT STDMETHODCALLTYPE Capture() final;
    HRESULT STDMETHODCALLTYPE Apply() final;

    void ResetJournal();

    void CaptureCurrentVertexDeclaration();
    void CaptureCurrentIndices();
    void CaptureCurrentRenderState(D3DRENDERSTATETYPE State);
    void CaptureCurrentStateSamplerState(
      DWORD StateSampler,
      D3DSAMPLERSTATETYPE Type);
    void CaptureCurrentStreamSource(UINT StreamNumber);
    void CaptureCurrentStreamSourceFreq(UINT StreamNumber);
    void CaptureCurrentStateTexture(DWORD StateSampler);
    void CaptureCurrentVertexShader();
    void CaptureCurrentPixelShader();
    void CaptureCurrentMaterial();
    HRESULT CaptureCurrentLight(DWORD Index);
    HRESULT CaptureCurrentLightEnable(DWORD Index);
    void CaptureCurrentStateTransform(uint32_t idx);
    void CaptureCurrentStateTextureStageState(
      DWORD Stage,
      D3D9TextureStageStateTypes Type);
    void CaptureCurrentViewport();
    void CaptureCurrentScissorRect();
    void CaptureCurrentClipPlane(DWORD Index);
    void CaptureCurrentVertexShaderConstantF(UINT StartRegister, UINT Count);
    void CaptureCurrentVertexShaderConstantI(UINT StartRegister, UINT Count);
    void CaptureCurrentVertexShaderConstantB(UINT StartRegister, UINT Count);
    void CaptureCurrentPixelShaderConstantF(UINT StartRegister, UINT Count);
    void CaptureCurrentPixelShaderConstantI(UINT StartRegister, UINT Count);
    void CaptureCurrentPixelShaderConstantB(UINT StartRegister, UINT Count);
    void CaptureCurrentNPatchMode();

    HRESULT SetVertexDeclaration(D3D9VertexDecl* pDecl);

    HRESULT SetIndices(D3D9IndexBuffer* pIndexData);

    HRESULT SetRenderState(D3DRENDERSTATETYPE State, DWORD Value);

    HRESULT SetStateSamplerState(
            DWORD               StateSampler,
            D3DSAMPLERSTATETYPE Type,
            DWORD               Value);

    HRESULT SetStreamSource(
            UINT               StreamNumber,
            D3D9VertexBuffer*  pStreamData,
            UINT               OffsetInBytes,
            UINT               Stride);

    HRESULT SetStreamSourceWithoutOffset(
            UINT               StreamNumber,
            D3D9VertexBuffer*  pStreamData,
            UINT               Stride);

    HRESULT SetStreamSourceFreq(UINT StreamNumber, UINT Setting);

    HRESULT SetStateTexture(DWORD StateSampler, IDirect3DBaseTexture9* pTexture);

    HRESULT SetVertexShader(D3D9VertexShader* pShader);

    HRESULT SetPixelShader(D3D9PixelShader* pShader);

    HRESULT SetMaterial(const D3DMATERIAL9* pMaterial);

    HRESULT SetLight(DWORD Index, const D3DLIGHT9* pLight);

    HRESULT LightEnable(DWORD Index, BOOL Enable);

    HRESULT SetStateTransform(uint32_t idx, const D3DMATRIX* pMatrix);

    HRESULT SetStateTextureStageState(
            DWORD                      Stage,
            D3D9TextureStageStateTypes Type,
            DWORD                      Value);

    HRESULT SetViewport(const D3DVIEWPORT9* pViewport);

    HRESULT SetScissorRect(const RECT* pRect);

    HRESULT SetClipPlane(DWORD Index, const float* pPlane);


    HRESULT SetVertexShaderConstantF(
            UINT   StartRegister,
      const float* pConstantData,
            UINT   Vector4fCount);

    HRESULT SetVertexShaderConstantI(
            UINT StartRegister,
      const int* pConstantData,
            UINT Vector4iCount);

    HRESULT SetVertexShaderConstantB(
            UINT  StartRegister,
      const BOOL* pConstantData,
            UINT  BoolCount);


    HRESULT SetPixelShaderConstantF(
            UINT   StartRegister,
      const float* pConstantData,
            UINT   Vector4fCount);

    HRESULT SetPixelShaderConstantI(
            UINT StartRegister,
      const int* pConstantData,
            UINT Vector4iCount);

    HRESULT SetPixelShaderConstantB(
            UINT  StartRegister,
      const BOOL* pConstantData,
            UINT  BoolCount);

    HRESULT SetNPatchMode(float nSegments);

    enum class D3D9StateFunction {
      Apply,
      Capture
    };

    template <typename Dst, typename Src, bool IgnoreStreamOffset>
    void ApplyOrCapture(Dst* dst, const Src* src) {
      if (m_captures.flags.test(D3D9CapturedStateFlag::StreamFreq)) {
        for (uint32_t idx : bit::BitMask(m_captures.streamFreq.dword(0))) {
          bool unchanged = false;
          if constexpr (std::is_same_v<Dst, D3D9DeviceEx>) {
            if (m_prefilter)
              unchanged = m_deviceState->streamFreq[idx] == src->streamFreq[idx];
          }

          if (!unchanged)
            dst->SetStreamSourceFreq(idx, src->streamFreq[idx]);
        }
      }

      if (m_captures.flags.test(D3D9CapturedStateFlag::Indices)) {
        bool unchanged = false;
        if constexpr (std::is_same_v<Dst, D3D9DeviceEx>) {
          if (m_prefilter)
            unchanged = m_deviceState->indices.ptr() == src->indices.ptr();
        }

        if (!unchanged)
          dst->SetIndices(src->indices.ptr());
      }

      if (m_captures.flags.test(D3D9CapturedStateFlag::RenderStates)) {
        for (uint32_t i = 0; i < m_captures.renderStates.dwordCount(); i++) {
          for (uint32_t rs : bit::BitMask(m_captures.renderStates.dword(i))) {
            uint32_t idx = i * 32 + rs;
            const DWORD value = src->renderStates[idx];

            bool unchanged = false;
            if (m_prefilter) {
              if constexpr (std::is_same_v<Dst, D3D9DeviceEx>)
                unchanged = m_deviceState->renderStates[idx] == value;
              else
                unchanged = m_state.renderStates[idx] == value;
            }

            if (!unchanged)
              dst->SetRenderState(D3DRENDERSTATETYPE(idx), value);
          }
        }
      }

      if (m_captures.flags.test(D3D9CapturedStateFlag::SamplerStates)) {
        for (uint32_t samplerIdx : bit::BitMask(m_captures.samplers.dword(0))) {
          for (uint32_t stateIdx : bit::BitMask(m_captures.samplerStates[samplerIdx].dword(0))) {
            const DWORD value = src->samplerStates[samplerIdx][stateIdx];

            bool unchanged = false;
            if (m_prefilter) {
              if constexpr (std::is_same_v<Dst, D3D9DeviceEx>)
                unchanged = m_deviceState->samplerStates[samplerIdx][stateIdx] == value;
              else
                unchanged = m_state.samplerStates[samplerIdx][stateIdx] == value;
            }

            if (!unchanged)
              dst->SetStateSamplerState(samplerIdx, D3DSAMPLERSTATETYPE(stateIdx), value);
          }
        }
      }

      if (m_captures.flags.test(D3D9CapturedStateFlag::VertexBuffers)) {
        for (uint32_t idx : bit::BitMask(m_captures.vertexBuffers.dword(0))) {
          const auto& vbo = src->vertexBuffers[idx];
          bool unchanged = false;
          if constexpr (std::is_same_v<Dst, D3D9DeviceEx>) {
            if (m_prefilter) {
              const auto& current = m_deviceState->vertexBuffers[idx];
              unchanged = current.vertexBuffer.ptr() == vbo.vertexBuffer.ptr()
                && current.stride == vbo.stride;
              if constexpr (!IgnoreStreamOffset)
                unchanged = unchanged && current.offset == vbo.offset;
            }
          }

          if constexpr (!IgnoreStreamOffset) {
            if (!unchanged) {
              dst->SetStreamSource(
                idx,
                vbo.vertexBuffer.ptr(),
                vbo.offset,
                vbo.stride);
            }
          } else if (!unchanged) {
            // For whatever reason, D3D9 doesn't capture the stream offset
            dst->SetStreamSourceWithoutOffset(
              idx,
              vbo.vertexBuffer.ptr(),
              vbo.stride);
          }
        }
      }

      if (m_captures.flags.test(D3D9CapturedStateFlag::Material)) {
        bool unchanged = false;
        if constexpr (std::is_same_v<Dst, D3D9DeviceEx>) {
          if (m_prefilter)
            unchanged = std::memcmp(&m_deviceState->material,
              &src->material, sizeof(D3DMATERIAL9)) == 0;
        }

        if (!unchanged)
          dst->SetMaterial(&src->material);
      }

      if (m_captures.flags.test(D3D9CapturedStateFlag::Textures)) {
        for (uint32_t idx : bit::BitMask(m_captures.textures.dword(0))) {
          auto* texture = src->textures[idx];

          bool unchanged = false;
          if (m_prefilter) {
            if constexpr (std::is_same_v<Dst, D3D9DeviceEx>)
              unchanged = m_deviceState->textures[idx] == texture;
            else
              unchanged = m_state.textures[idx] == texture;
          }

          if (!unchanged)
            dst->SetStateTexture(idx, texture);
        }
      }

      if (m_captures.flags.test(D3D9CapturedStateFlag::VertexShader)) {
        auto* shader = src->vertexShader.ptr();
        bool unchanged = false;
        if (m_prefilter) {
          if constexpr (std::is_same_v<Dst, D3D9DeviceEx>)
            unchanged = m_deviceState->vertexShader.ptr() == shader;
          else
            unchanged = m_state.vertexShader.ptr() == shader;
        }
        if (!unchanged)
          dst->SetVertexShader(shader);
      }

      if (m_captures.flags.test(D3D9CapturedStateFlag::PixelShader)) {
        auto* shader = src->pixelShader.ptr();
        bool unchanged = false;
        if (m_prefilter) {
          if constexpr (std::is_same_v<Dst, D3D9DeviceEx>)
            unchanged = m_deviceState->pixelShader.ptr() == shader;
          else
            unchanged = m_state.pixelShader.ptr() == shader;
        }
        if (!unchanged)
          dst->SetPixelShader(shader);
      }

      if (m_captures.flags.test(D3D9CapturedStateFlag::Transforms)) {
        for (uint32_t i = 0; i < m_captures.transforms.dwordCount(); i++) {
          for (uint32_t trans : bit::BitMask(m_captures.transforms.dword(i))) {
            uint32_t idx = i * 32 + trans;

            bool unchanged = false;
            if constexpr (std::is_same_v<Dst, D3D9DeviceEx>) {
              if (m_prefilter)
                unchanged = m_deviceState->transforms[idx] == src->transforms[idx];
            }

            if (!unchanged)
              dst->SetStateTransform(idx, reinterpret_cast<const D3DMATRIX*>(&src->transforms[idx]));
          }
        }
      }

      if (m_captures.flags.test(D3D9CapturedStateFlag::TextureStages)) {
        for (uint32_t stageIdx : bit::BitMask(m_captures.textureStages.dword(0))) {
          for (uint32_t stateIdx : bit::BitMask(m_captures.textureStageStates[stageIdx].dword(0))) {
            const DWORD value = src->textureStages[stageIdx][stateIdx];

            bool unchanged = false;
            if (m_prefilter) {
              if constexpr (std::is_same_v<Dst, D3D9DeviceEx>)
                unchanged = m_deviceState->textureStages[stageIdx][stateIdx] == value;
              else
                unchanged = m_state.textureStages[stageIdx][stateIdx] == value;
            }

            if (!unchanged)
              dst->SetStateTextureStageState(stageIdx, D3D9TextureStageStateTypes(stateIdx), value);
          }
        }
      }

      if (m_captures.flags.test(D3D9CapturedStateFlag::Viewport)) {
        bool unchanged = false;
        if constexpr (std::is_same_v<Dst, D3D9DeviceEx>) {
          if (m_prefilter)
            unchanged = m_deviceState->viewport == src->viewport;
        }

        if (!unchanged)
          dst->SetViewport(&src->viewport);
      }

      if (m_captures.flags.test(D3D9CapturedStateFlag::ScissorRect)) {
        bool unchanged = false;
        if constexpr (std::is_same_v<Dst, D3D9DeviceEx>) {
          if (m_prefilter)
            unchanged = m_deviceState->scissorRect == src->scissorRect;
        }

        if (!unchanged)
          dst->SetScissorRect(&src->scissorRect);
      }

      if (m_captures.flags.test(D3D9CapturedStateFlag::ClipPlanes)) {
        for (uint32_t idx : bit::BitMask(m_captures.clipPlanes.dword(0))) {
          bool unchanged = false;
          if constexpr (std::is_same_v<Dst, D3D9DeviceEx>) {
            if (m_prefilter)
              unchanged = m_deviceState->clipPlanes[idx] == src->clipPlanes[idx];
          }

          if (!unchanged)
            dst->SetClipPlane(idx, src->clipPlanes[idx].coeff);
        }
      }

      if (!m_prefilter) {
        if (m_captures.flags.test(D3D9CapturedStateFlag::VsConstants)) {
          for (uint32_t i = 0; i < m_captures.vsConsts.fConsts.dwordCount(); i++) {
            for (uint32_t consts : bit::BitMask(m_captures.vsConsts.fConsts.dword(i))) {
              uint32_t idx = i * 32 + consts;

              dst->SetVertexShaderConstantF(idx, reinterpret_cast<const float*>(&src->vsConsts->fConsts[idx]), 1);
            }
          }

          for (uint32_t i = 0; i < m_captures.vsConsts.iConsts.dwordCount(); i++) {
            for (uint32_t consts : bit::BitMask(m_captures.vsConsts.iConsts.dword(i))) {
              uint32_t idx = i * 32 + consts;

              dst->SetVertexShaderConstantI(idx, reinterpret_cast<const int*>(&src->vsConsts->iConsts[idx]), 1);
            }
          }

          if (m_captures.vsConsts.bConsts.any()) {
            for (uint32_t i = 0; i < m_captures.vsConsts.bConsts.dwordCount(); i++)
              dst->SetVertexBoolBitfield(i, m_captures.vsConsts.bConsts.dword(i), src->vsConsts->bConsts[i]);
          }
        }

        if (m_captures.flags.test(D3D9CapturedStateFlag::PsConstants)) {
          for (uint32_t i = 0; i < m_captures.psConsts.fConsts.dwordCount(); i++) {
            for (uint32_t consts : bit::BitMask(m_captures.psConsts.fConsts.dword(i))) {
              uint32_t idx = i * 32 + consts;

              dst->SetPixelShaderConstantF(idx, reinterpret_cast<const float*>(&src->psConsts->fConsts[idx]), 1);
            }
          }

          for (uint32_t i = 0; i < m_captures.psConsts.iConsts.dwordCount(); i++) {
            for (uint32_t consts : bit::BitMask(m_captures.psConsts.iConsts.dword(i))) {
              uint32_t idx = i * 32 + consts;

              dst->SetPixelShaderConstantI(idx, reinterpret_cast<const int*>(&src->psConsts->iConsts[idx]), 1);
            }
          }

          if (m_captures.psConsts.bConsts.any()) {
            for (uint32_t i = 0; i < m_captures.psConsts.bConsts.dwordCount(); i++)
              dst->SetPixelBoolBitfield(i, m_captures.psConsts.bConsts.dword(i), src->psConsts->bConsts[i]);
          }
        }
      } else {
        auto applyConstantRanges = [] (auto& captures, uint32_t dwordCount, auto&& applyRange) {
          const uint32_t registerCount = dwordCount * 32;
          uint32_t rangeStart = 0;

          while (rangeStart < registerCount) {
            while (rangeStart < registerCount && !captures.get(rangeStart))
              rangeStart++;

            if (rangeStart == registerCount)
              break;

            uint32_t rangeEnd = rangeStart + 1;
            while (rangeEnd < registerCount && captures.get(rangeEnd))
              rangeEnd++;

            applyRange(rangeStart, rangeEnd - rangeStart);
            rangeStart = rangeEnd;
          }
        };

        if (m_captures.flags.test(D3D9CapturedStateFlag::VsConstants)) {
          applyConstantRanges(m_captures.vsConsts.fConsts, m_captures.vsConsts.fDwordCount,
            [&] (uint32_t start, uint32_t count) {
              const auto* source = &src->vsConsts->fConsts[start];
              const auto* current = std::is_same_v<Dst, D3D9DeviceEx>
                ? &m_deviceState->vsConsts->fConsts[start]
                : &m_state.vsConsts->fConsts[start];
              if (std::memcmp(current, source, count * sizeof(*source)) != 0)
                dst->SetVertexShaderConstantF(start, reinterpret_cast<const float*>(source), count);
            });

          applyConstantRanges(m_captures.vsConsts.iConsts, m_captures.vsConsts.iDwordCount,
            [&] (uint32_t start, uint32_t count) {
              const auto* source = &src->vsConsts->iConsts[start];
              const auto* current = std::is_same_v<Dst, D3D9DeviceEx>
                ? &m_deviceState->vsConsts->iConsts[start]
                : &m_state.vsConsts->iConsts[start];
              if (std::memcmp(current, source, count * sizeof(*source)) != 0)
                dst->SetVertexShaderConstantI(start, reinterpret_cast<const int*>(source), count);
            });

          if (m_captures.vsConsts.bDwordCount) {
            for (uint32_t i = 0; i < m_captures.vsConsts.bDwordCount; i++) {
              const uint32_t mask = m_captures.vsConsts.bConsts.dword(i);
              const uint32_t bits = src->vsConsts->bConsts[i];
              const uint32_t current = std::is_same_v<Dst, D3D9DeviceEx>
                ? m_deviceState->vsConsts->bConsts[i]
                : m_state.vsConsts->bConsts[i];
              if ((current & mask) != (bits & mask))
                dst->SetVertexBoolBitfield(i, mask, bits);
            }
          }
        }

        if (m_captures.flags.test(D3D9CapturedStateFlag::PsConstants)) {
          applyConstantRanges(m_captures.psConsts.fConsts, m_captures.psConsts.fDwordCount,
            [&] (uint32_t start, uint32_t count) {
              const auto* source = &src->psConsts->fConsts[start];
              const auto* current = std::is_same_v<Dst, D3D9DeviceEx>
                ? &m_deviceState->psConsts->fConsts[start]
                : &m_state.psConsts->fConsts[start];
              if (std::memcmp(current, source, count * sizeof(*source)) != 0)
                dst->SetPixelShaderConstantF(start, reinterpret_cast<const float*>(source), count);
            });

          applyConstantRanges(m_captures.psConsts.iConsts, m_captures.psConsts.iDwordCount,
            [&] (uint32_t start, uint32_t count) {
              const auto* source = &src->psConsts->iConsts[start];
              const auto* current = std::is_same_v<Dst, D3D9DeviceEx>
                ? &m_deviceState->psConsts->iConsts[start]
                : &m_state.psConsts->iConsts[start];
              if (std::memcmp(current, source, count * sizeof(*source)) != 0)
                dst->SetPixelShaderConstantI(start, reinterpret_cast<const int*>(source), count);
            });

          if (m_captures.psConsts.bDwordCount) {
            for (uint32_t i = 0; i < m_captures.psConsts.bDwordCount; i++) {
              const uint32_t mask = m_captures.psConsts.bConsts.dword(i);
              const uint32_t bits = src->psConsts->bConsts[i];
              const uint32_t current = std::is_same_v<Dst, D3D9DeviceEx>
                ? m_deviceState->psConsts->bConsts[i]
                : m_state.psConsts->bConsts[i];
              if ((current & mask) != (bits & mask))
                dst->SetPixelBoolBitfield(i, mask, bits);
            }
          }
        }
      }

      if (m_captures.flags.test(D3D9CapturedStateFlag::Lights)) {
        for (uint32_t i = 0; i < m_captures.lightChanges.dwordCount(); i++) {
          for (uint32_t light : bit::BitMask(m_captures.lightChanges.dword(i))) {
            const uint32_t idx = i * 32 + light;

            if (idx < src->lights.size() && src->lights[idx].isValid)
            {
              bool unchanged = false;
              if constexpr (std::is_same_v<Dst, D3D9DeviceEx>) {
                if (m_prefilter && idx < m_deviceState->lights.size()) {
                  const auto& current = m_deviceState->lights[idx];
                  unchanged = current.isValid
                    && std::memcmp(&current.light, &src->lights[idx].light,
                      sizeof(D3DLIGHT9)) == 0;
                }
              }

              if (!unchanged)
                dst->SetLight(idx, &src->lights[idx].light);
            }
          }
        }

        for (uint32_t i = 0; i < m_captures.lightEnabledChanges.dwordCount(); i++) {
          for (uint32_t consts : bit::BitMask(m_captures.lightEnabledChanges.dword(i))) {
            uint32_t idx = i * 32 + consts;

            if (idx < src->lights.size()) {
              bool unchanged = false;
              if constexpr (std::is_same_v<Dst, D3D9DeviceEx>) {
                if (m_prefilter && idx < m_deviceState->lights.size()) {
                  const auto& current = m_deviceState->lights[idx];
                  unchanged = current.isValid == src->lights[idx].isValid
                    && current.isEnabled == src->lights[idx].isEnabled;
                }
              }

              if (!unchanged)
                dst->LightEnable(idx, src->lights[idx].isEnabled);
            }
          }
        }

        if constexpr (std::is_same_v<Dst, D3D9DeviceEx>) {
          if (m_journal) {
            for (uint32_t i = 0; i < m_captures.lightChanges.dwordCount(); i++) {
              for (uint32_t light : bit::BitMask(m_captures.lightChanges.dword(i))) {
                const uint32_t idx = i * 32 + light;

                if (idx >= src->lights.size() || !src->lights[idx].isValid)
                  dst->RestoreGtaSaUndefinedLight(idx);
              }
            }
          }
        }
      }

      if (m_captures.flags.test(D3D9CapturedStateFlag::NPatchMode))
        dst->SetNPatchMode(src->nPatchSegments);
    }

    template <D3D9StateFunction Func, bool IgnoreStreamOffset>
    void ApplyOrCapture() {
      if      constexpr (Func == D3D9StateFunction::Apply)
        ApplyOrCapture<D3D9DeviceEx, D3D9CapturableState, IgnoreStreamOffset>(m_parent, &m_state);
      else if constexpr (Func == D3D9StateFunction::Capture)
        ApplyOrCapture<D3D9StateBlock, D3D9DeviceState, IgnoreStreamOffset>(this, m_deviceState);
    }

    template <
      D3D9ShaderType   ShaderType,
      D3D9ConstantType ConstantType,
      typename         T>
    HRESULT SetShaderConstants(
            UINT  StartRegister,
      const T*    pConstantData,
            UINT  Count) {
      auto SetHelper = [&](auto& setCaptures) {
        if constexpr (ShaderType == D3D9ShaderType::VertexShader)
          m_captures.flags.set(D3D9CapturedStateFlag::VsConstants);
        else
          m_captures.flags.set(D3D9CapturedStateFlag::PsConstants);

        for (uint32_t i = 0; i < Count; i++) {
          uint32_t reg = StartRegister + i;
          if constexpr (ConstantType == D3D9ConstantType::Float) {
            setCaptures.fConsts.set(reg, true);
            setCaptures.fDwordCount = std::max(setCaptures.fDwordCount, reg / 32 + 1);
          } else if constexpr (ConstantType == D3D9ConstantType::Int) {
            setCaptures.iConsts.set(reg, true);
            setCaptures.iDwordCount = std::max(setCaptures.iDwordCount, reg / 32 + 1);
          } else if constexpr (ConstantType == D3D9ConstantType::Bool) {
            setCaptures.bConsts.set(reg, true);
            setCaptures.bDwordCount = std::max(setCaptures.bDwordCount, reg / 32 + 1);
          }
        }

        UpdateStateConstants<ShaderType, ConstantType, T>(
          &m_state, StartRegister, pConstantData, Count);

        return D3D_OK;
      };

      return ShaderType == D3D9ShaderType::VertexShader
        ? SetHelper(m_captures.vsConsts)
        : SetHelper(m_captures.psConsts);
    }

    HRESULT SetVertexBoolBitfield(uint32_t idx, uint32_t mask, uint32_t bits);
    HRESULT SetPixelBoolBitfield (uint32_t idx, uint32_t mask, uint32_t bits);

  private:

    void CapturePixelRenderStates();
    void CapturePixelSamplerStates();
    void CapturePixelShaderStates();

    void CaptureVertexRenderStates();
    void CaptureVertexSamplerStates();
    void CaptureVertexShaderStates();

    void CaptureType(D3D9StateBlockType State);

    template <
      D3D9ShaderType   ShaderType,
      D3D9ConstantType ConstantType>
    void CaptureCurrentShaderConstants(UINT StartRegister, UINT Count);

    D3D9CapturableState  m_state;
    D3D9StateCaptures    m_captures;

    D3D9DeviceState*     m_deviceState;
    const bool           m_prefilter;
    const bool           m_fastSkip;
    const bool           m_countLosableResource;
    const bool           m_journal;
    bool                  m_hasApplied = false;
    uint64_t              m_lastAppliedStateSerial = 0u;

  };

}
