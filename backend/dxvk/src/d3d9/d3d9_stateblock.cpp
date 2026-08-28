#include "d3d9_stateblock.h"

#include "d3d9_caps.h"
#include "d3d9_device.h"
#include "d3d9_vertex_declaration.h"
#include "d3d9_buffer.h"
#include "d3d9_shader.h"
#include "d3d9_texture.h"

#include "d3d9_util.h"

namespace dxvk {

  D3D9StateBlock::D3D9StateBlock(
          D3D9DeviceEx*      pDevice,
          D3D9StateBlockType Type,
          bool               forcePrefilter,
          bool               countLosableResource,
          bool               journal)
    : D3D9StateBlockBase      (pDevice)
    , m_deviceState           (pDevice->GetRawState())
    , m_prefilter             (forcePrefilter || pDevice->GetOptions()->gtaSaStateBlockPrefilter)
    , m_fastSkip              (pDevice->GetOptions()->gtaSaStateBlockFastSkip)
    , m_countLosableResource  (countLosableResource)
    , m_journal               (journal) {
    CaptureType(Type);
  }

  D3D9StateBlock::~D3D9StateBlock() {
    if (m_countLosableResource && !m_parent->IsD3D8Compatible())
      m_parent->DecrementLosableCounter();
  }


  void D3D9StateBlock::ResetJournal() {
    m_hasApplied = false;

    m_state.vertexDecl = nullptr;
    m_state.indices = nullptr;
    m_state.vertexShader = nullptr;
    m_state.pixelShader = nullptr;

    if (m_state.vertexBuffers) {
      for (auto& stream : m_state.vertexBuffers.get()) {
        stream.vertexBuffer = nullptr;
        stream.offset = 0u;
        stream.length = 0u;
        stream.stride = 0u;
      }
    }

    if (m_state.textures) {
      for (auto& texture : m_state.textures.get())
        TextureChangePrivate(texture, nullptr);
    }

    m_state.lights.clear();

    m_captures.flags = D3D9CapturedStateFlags();
    m_captures.renderStates.clearAll();
    m_captures.samplers.clearAll();
    for (auto& states : m_captures.samplerStates)
      states.clearAll();
    m_captures.vertexBuffers.clearAll();
    m_captures.textures.clearAll();
    m_captures.clipPlanes.clearAll();
    m_captures.streamFreq.clearAll();
    m_captures.transforms.clearAll();
    m_captures.textureStages.clearAll();
    for (auto& states : m_captures.textureStageStates)
      states.clearAll();
    m_captures.vsConsts.fConsts.clearAll();
    m_captures.vsConsts.iConsts.clearAll();
    m_captures.vsConsts.bConsts.clearAll();
    m_captures.vsConsts.fDwordCount = 0u;
    m_captures.vsConsts.iDwordCount = 0u;
    m_captures.vsConsts.bDwordCount = 0u;
    m_captures.psConsts.fConsts.clearAll();
    m_captures.psConsts.iConsts.clearAll();
    m_captures.psConsts.bConsts.clearAll();
    m_captures.psConsts.fDwordCount = 0u;
    m_captures.psConsts.iDwordCount = 0u;
    m_captures.psConsts.bDwordCount = 0u;
    m_captures.lightChanges.clearAll();
    m_captures.lightEnabledChanges.clearAll();
  }

  HRESULT STDMETHODCALLTYPE D3D9StateBlock::QueryInterface(
          REFIID  riid,
          void** ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown)
     || riid == __uuidof(IDirect3DStateBlock9)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(IDirect3DStateBlock9), riid)) {
      Logger::warn("D3D9StateBlock::QueryInterface: Unknown interface query");
      Logger::warn(str::format(riid));
    }

    return E_NOINTERFACE;
  }


  HRESULT STDMETHODCALLTYPE D3D9StateBlock::Capture() {
    D3D9DeviceLock lock = m_parent->LockDevice();

    // A state block can't capture state while another is being recorded.
    if (unlikely(m_parent->ShouldRecord()))
      return D3DERR_INVALIDCALL;

    const uint64_t auditBegin = m_parent->GetGtaSaCompat()->BeginStateAuditTiming();

    if (m_captures.flags.test(D3D9CapturedStateFlag::VertexDecl))
      SetVertexDeclaration(m_deviceState->vertexDecl.ptr());

    ApplyOrCapture<D3D9StateFunction::Capture, true>();

    m_hasApplied = false;

    m_parent->GetGtaSaCompat()->RecordStateAuditTiming(
      D3D9GtaSaStateAuditKind::StateBlockCapture, auditBegin);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9StateBlock::Apply() {
    D3D9DeviceLock lock = m_parent->LockDevice();

    // A state block can't be applied while another is being recorded.
    if (unlikely(m_parent->ShouldRecord()))
      return D3DERR_INVALIDCALL;

    const uint64_t auditBegin = m_parent->GetGtaSaCompat()->BeginStateAuditTiming();

    if (m_fastSkip && m_prefilter && !m_journal && m_hasApplied
     && m_lastAppliedStateSerial == m_parent->GetGtaSaStateSerial()) {
      m_parent->GetGtaSaCompat()->RecordStateBlockFastSkip();
      m_parent->GetGtaSaCompat()->RecordStateAuditTiming(
        D3D9GtaSaStateAuditKind::StateBlockApply, auditBegin);
      return D3D_OK;
    }

    if (m_captures.flags.test(D3D9CapturedStateFlag::VertexDecl)
     && (m_state.vertexDecl != nullptr || m_journal))
      m_parent->SetVertexDeclaration(m_state.vertexDecl.ptr());

    ApplyOrCapture<D3D9StateFunction::Apply, false>();

    m_lastAppliedStateSerial = m_parent->GetGtaSaStateSerial();
    m_hasApplied = true;

    m_parent->GetGtaSaCompat()->RecordStateAuditTiming(
      D3D9GtaSaStateAuditKind::StateBlockApply, auditBegin);

    return D3D_OK;
  }


  HRESULT D3D9StateBlock::SetVertexDeclaration(D3D9VertexDecl* pDecl) {
    m_state.vertexDecl = pDecl;

    m_captures.flags.set(D3D9CapturedStateFlag::VertexDecl);
    return D3D_OK;
  }


  HRESULT D3D9StateBlock::SetIndices(D3D9IndexBuffer* pIndexData) {
    m_state.indices = pIndexData;

    m_captures.flags.set(D3D9CapturedStateFlag::Indices);
    return D3D_OK;
  }


  HRESULT D3D9StateBlock::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) {
    m_state.renderStates[State] = Value;

    m_captures.flags.set(D3D9CapturedStateFlag::RenderStates);
    m_captures.renderStates.set(State, true);
    return D3D_OK;
  }


  HRESULT D3D9StateBlock::SetStateSamplerState(
          DWORD               StateSampler,
          D3DSAMPLERSTATETYPE Type,
          DWORD               Value) {
    m_state.samplerStates[StateSampler][Type] = Value;

    m_captures.flags.set(D3D9CapturedStateFlag::SamplerStates);
    m_captures.samplers.set(StateSampler, true);
    m_captures.samplerStates[StateSampler].set(Type, true);
    return D3D_OK;
  }


  HRESULT D3D9StateBlock::SetStreamSource(
          UINT                StreamNumber,
          D3D9VertexBuffer*   pStreamData,
          UINT                OffsetInBytes,
          UINT                Stride) {
    m_state.vertexBuffers[StreamNumber].vertexBuffer = pStreamData;

    m_state.vertexBuffers[StreamNumber].offset = OffsetInBytes;
    m_state.vertexBuffers[StreamNumber].stride = Stride;

    m_captures.flags.set(D3D9CapturedStateFlag::VertexBuffers);
    m_captures.vertexBuffers.set(StreamNumber, true);
    return D3D_OK;
  }


  HRESULT D3D9StateBlock::SetStreamSourceWithoutOffset(
          UINT                StreamNumber,
          D3D9VertexBuffer*   pStreamData,
          UINT                Stride) {
    m_state.vertexBuffers[StreamNumber].vertexBuffer = pStreamData;

    m_state.vertexBuffers[StreamNumber].stride = Stride;

    m_captures.flags.set(D3D9CapturedStateFlag::VertexBuffers);
    m_captures.vertexBuffers.set(StreamNumber, true);
    return D3D_OK;
  }


  HRESULT D3D9StateBlock::SetStreamSourceFreq(UINT StreamNumber, UINT Setting) {
    m_state.streamFreq[StreamNumber] = Setting;

    m_captures.flags.set(D3D9CapturedStateFlag::StreamFreq);
    m_captures.streamFreq.set(StreamNumber, true);
    return D3D_OK;
  }


  HRESULT D3D9StateBlock::SetStateTexture(DWORD StateSampler, IDirect3DBaseTexture9* pTexture) {
    TextureChangePrivate(m_state.textures[StateSampler], pTexture);

    m_captures.flags.set(D3D9CapturedStateFlag::Textures);
    m_captures.textures.set(StateSampler, true);
    return D3D_OK;
  }


  HRESULT D3D9StateBlock::SetVertexShader(D3D9VertexShader* pShader) {
    m_state.vertexShader = pShader;

    m_captures.flags.set(D3D9CapturedStateFlag::VertexShader);
    return D3D_OK;
  }


  HRESULT D3D9StateBlock::SetPixelShader(D3D9PixelShader* pShader) {
    m_state.pixelShader = pShader;

    m_captures.flags.set(D3D9CapturedStateFlag::PixelShader);
    return D3D_OK;
  }


  HRESULT D3D9StateBlock::SetMaterial(const D3DMATERIAL9* pMaterial) {
    m_state.material = *pMaterial;

    m_captures.flags.set(D3D9CapturedStateFlag::Material);
    return D3D_OK;
  }


  HRESULT D3D9StateBlock::SetLight(DWORD Index, const D3DLIGHT9* pLight) {
    if (Index >= m_state.lights.size())
      m_state.lights.resize(Index + 1);

    auto& light = m_state.lights[Index];
    light.isValid = true;
    light.light = *pLight;

    m_captures.lightChanges.set(Index, true);
    m_captures.flags.set(D3D9CapturedStateFlag::Lights);
    return D3D_OK;
  }


  HRESULT D3D9StateBlock::LightEnable(DWORD Index, BOOL Enable) {
    if (unlikely(Index >= m_state.lights.size()))
      m_state.lights.resize(Index + 1);

    // Apply default light on enable, but not disable.
    // No idea if this is correct, but matches the old logic.
    auto& light = m_state.lights[Index];
    light.isEnabled = bool(Enable);

    if (Enable)
      light.isValid = true;

    m_captures.lightEnabledChanges.set(Index, true);
    m_captures.flags.set(D3D9CapturedStateFlag::Lights);
    return D3D_OK;
  }


  HRESULT D3D9StateBlock::SetStateTransform(uint32_t idx, const D3DMATRIX* pMatrix) {
    m_state.transforms[idx] = ConvertMatrix(pMatrix);

    m_captures.flags.set(D3D9CapturedStateFlag::Transforms);
    m_captures.transforms.set(idx, true);
    return D3D_OK;
  }


  HRESULT D3D9StateBlock::SetStateTextureStageState(
          DWORD                      Stage,
          D3D9TextureStageStateTypes Type,
          DWORD                      Value) {
    Stage = std::min(Stage, DWORD(caps::TextureStageCount - 1));
    Type = std::min(Type, D3D9TextureStageStateTypes(DXVK_TSS_COUNT - 1));

    m_state.textureStages[Stage][Type] = Value;

    m_captures.flags.set(D3D9CapturedStateFlag::TextureStages);
    m_captures.textureStages.set(Stage, true);
    m_captures.textureStageStates[Stage].set(Type, true);
    return D3D_OK;
  }


  HRESULT D3D9StateBlock::SetViewport(const D3DVIEWPORT9* pViewport) {
    m_state.viewport = *pViewport;

    m_captures.flags.set(D3D9CapturedStateFlag::Viewport);
    return D3D_OK;
  }


  HRESULT D3D9StateBlock::SetScissorRect(const RECT* pRect) {
    m_state.scissorRect = *pRect;

    m_captures.flags.set(D3D9CapturedStateFlag::ScissorRect);
    return D3D_OK;
  }


  HRESULT D3D9StateBlock::SetClipPlane(DWORD Index, const float* pPlane) {
    for (uint32_t i = 0; i < 4; i++)
      m_state.clipPlanes[Index].coeff[i] = pPlane[i];

    m_captures.flags.set(D3D9CapturedStateFlag::ClipPlanes);
    m_captures.clipPlanes.set(Index, true);
    return D3D_OK;
  }


  HRESULT D3D9StateBlock::SetVertexShaderConstantF(
          UINT   StartRegister,
    const float* pConstantData,
          UINT   Vector4fCount) {
    return SetShaderConstants<
      D3D9ShaderType::VertexShader,
      D3D9ConstantType::Float>(
        StartRegister,
        pConstantData,
        Vector4fCount);
  }


  HRESULT D3D9StateBlock::SetVertexShaderConstantI(
          UINT StartRegister,
    const int* pConstantData,
          UINT Vector4iCount) {
    return SetShaderConstants<
      D3D9ShaderType::VertexShader,
      D3D9ConstantType::Int>(
        StartRegister,
        pConstantData,
        Vector4iCount);
  }


  HRESULT D3D9StateBlock::SetVertexShaderConstantB(
          UINT  StartRegister,
    const BOOL* pConstantData,
          UINT  BoolCount) {
    return SetShaderConstants<
      D3D9ShaderType::VertexShader,
      D3D9ConstantType::Bool>(
        StartRegister,
        pConstantData,
        BoolCount);
  }


  HRESULT D3D9StateBlock::SetPixelShaderConstantF(
          UINT   StartRegister,
    const float* pConstantData,
          UINT   Vector4fCount) {
    return SetShaderConstants<
      D3D9ShaderType::PixelShader,
      D3D9ConstantType::Float>(
        StartRegister,
        pConstantData,
        Vector4fCount);
  }


  HRESULT D3D9StateBlock::SetPixelShaderConstantI(
          UINT StartRegister,
    const int* pConstantData,
          UINT Vector4iCount) {
    return SetShaderConstants<
      D3D9ShaderType::PixelShader,
      D3D9ConstantType::Int>(
        StartRegister,
        pConstantData,
        Vector4iCount);
  }


  HRESULT D3D9StateBlock::SetPixelShaderConstantB(
          UINT  StartRegister,
    const BOOL* pConstantData,
          UINT  BoolCount) {
    return SetShaderConstants<
      D3D9ShaderType::PixelShader,
      D3D9ConstantType::Bool>(
        StartRegister,
        pConstantData,
        BoolCount);
  }


  HRESULT D3D9StateBlock::SetNPatchMode(float nSegments) {
    m_state.nPatchSegments = nSegments;
    m_captures.flags.set(D3D9CapturedStateFlag::NPatchMode);
    return D3D_OK;
  }


  void D3D9StateBlock::CaptureCurrentVertexDeclaration() {
    if (!m_captures.flags.test(D3D9CapturedStateFlag::VertexDecl))
      SetVertexDeclaration(m_deviceState->vertexDecl.ptr());
  }


  void D3D9StateBlock::CaptureCurrentIndices() {
    if (!m_captures.flags.test(D3D9CapturedStateFlag::Indices))
      SetIndices(m_deviceState->indices.ptr());
  }


  void D3D9StateBlock::CaptureCurrentRenderState(D3DRENDERSTATETYPE State) {
    if (!m_captures.renderStates.get(State))
      SetRenderState(State, m_deviceState->renderStates[State]);
  }


  void D3D9StateBlock::CaptureCurrentStateSamplerState(
          DWORD               StateSampler,
          D3DSAMPLERSTATETYPE Type) {
    if (!m_captures.samplerStates[StateSampler].get(Type)) {
      SetStateSamplerState(
        StateSampler, Type, m_deviceState->samplerStates[StateSampler][Type]);
    }
  }


  void D3D9StateBlock::CaptureCurrentStreamSource(UINT StreamNumber) {
    if (m_captures.vertexBuffers.get(StreamNumber))
      return;

    const auto& stream = m_deviceState->vertexBuffers[StreamNumber];
    SetStreamSource(
      StreamNumber, stream.vertexBuffer.ptr(), stream.offset, stream.stride);
  }


  void D3D9StateBlock::CaptureCurrentStreamSourceFreq(UINT StreamNumber) {
    if (!m_captures.streamFreq.get(StreamNumber))
      SetStreamSourceFreq(StreamNumber, m_deviceState->streamFreq[StreamNumber]);
  }


  void D3D9StateBlock::CaptureCurrentStateTexture(DWORD StateSampler) {
    if (!m_captures.textures.get(StateSampler))
      SetStateTexture(StateSampler, m_deviceState->textures[StateSampler]);
  }


  void D3D9StateBlock::CaptureCurrentVertexShader() {
    if (!m_captures.flags.test(D3D9CapturedStateFlag::VertexShader))
      SetVertexShader(m_deviceState->vertexShader.ptr());
  }


  void D3D9StateBlock::CaptureCurrentPixelShader() {
    if (!m_captures.flags.test(D3D9CapturedStateFlag::PixelShader))
      SetPixelShader(m_deviceState->pixelShader.ptr());
  }


  void D3D9StateBlock::CaptureCurrentMaterial() {
    if (!m_captures.flags.test(D3D9CapturedStateFlag::Material))
      SetMaterial(&m_deviceState->material.get());
  }


  HRESULT D3D9StateBlock::CaptureCurrentLight(DWORD Index) {
    if (Index < m_captures.lightChanges.bitCount()
     && m_captures.lightChanges.get(Index))
      return D3D_OK;

    if (Index >= m_state.lights.size())
      m_state.lights.resize(Index + 1);

    if (Index < m_deviceState->lights.size())
      m_state.lights[Index] = m_deviceState->lights[Index];

    m_captures.lightChanges.set(Index, true);
    m_captures.flags.set(D3D9CapturedStateFlag::Lights);
    return D3D_OK;
  }


  HRESULT D3D9StateBlock::CaptureCurrentLightEnable(DWORD Index) {
    if (Index < m_captures.lightEnabledChanges.bitCount()
     && m_captures.lightEnabledChanges.get(Index))
      return D3D_OK;

    const HRESULT lightResult = CaptureCurrentLight(Index);
    if (FAILED(lightResult))
      return lightResult;

    const bool wasEnabled = Index < m_deviceState->lights.size()
      && m_deviceState->lights[Index].isValid
      && m_deviceState->lights[Index].isEnabled;
    return LightEnable(Index, BOOL(wasEnabled));
  }


  void D3D9StateBlock::CaptureCurrentStateTransform(uint32_t idx) {
    if (!m_captures.transforms.get(idx)) {
      SetStateTransform(
        idx, reinterpret_cast<const D3DMATRIX*>(&m_deviceState->transforms[idx]));
    }
  }


  void D3D9StateBlock::CaptureCurrentStateTextureStageState(
          DWORD                      Stage,
          D3D9TextureStageStateTypes Type) {
    if (!m_captures.textureStageStates[Stage].get(Type)) {
      SetStateTextureStageState(
        Stage, Type, m_deviceState->textureStages[Stage][Type]);
    }
  }


  void D3D9StateBlock::CaptureCurrentViewport() {
    if (!m_captures.flags.test(D3D9CapturedStateFlag::Viewport))
      SetViewport(&m_deviceState->viewport);
  }


  void D3D9StateBlock::CaptureCurrentScissorRect() {
    if (!m_captures.flags.test(D3D9CapturedStateFlag::ScissorRect))
      SetScissorRect(&m_deviceState->scissorRect);
  }


  void D3D9StateBlock::CaptureCurrentClipPlane(DWORD Index) {
    if (!m_captures.clipPlanes.get(Index))
      SetClipPlane(Index, m_deviceState->clipPlanes[Index].coeff);
  }


  template <
    D3D9ShaderType   ShaderType,
    D3D9ConstantType ConstantType>
  void D3D9StateBlock::CaptureCurrentShaderConstants(
          UINT StartRegister,
          UINT Count) {
    auto capture = [this, StartRegister, Count] (auto& captures, auto& current, auto& saved) {
      const UINT endRegister = StartRegister + Count;

      if constexpr (ConstantType == D3D9ConstantType::Bool) {
        m_captures.flags.set(ShaderType == D3D9ShaderType::VertexShader
          ? D3D9CapturedStateFlag::VsConstants
          : D3D9CapturedStateFlag::PsConstants);

        for (UINT reg = StartRegister; reg < endRegister; reg++) {
          if (captures.bConsts.get(reg))
            continue;

          const uint32_t dword = reg / 32u;
          const uint32_t mask = 1u << (reg % 32u);
          captures.bConsts.set(reg, true);
          captures.bDwordCount = std::max(captures.bDwordCount, dword + 1u);
          saved.bConsts[dword] &= ~mask;
          saved.bConsts[dword] |= current.bConsts[dword] & mask;
        }
      } else {
        auto captureRanges = [&] (auto& captured, auto&& captureRange) {
          UINT reg = StartRegister;
          while (reg < endRegister) {
            while (reg < endRegister && captured.get(reg))
              reg++;
            const UINT rangeStart = reg;
            while (reg < endRegister && !captured.get(reg))
              reg++;
            const UINT rangeCount = reg - rangeStart;
            if (rangeCount)
              captureRange(rangeStart, rangeCount);
          }
        };

        if constexpr (ConstantType == D3D9ConstantType::Float) {
          captureRanges(captures.fConsts, [&] (UINT rangeStart, UINT rangeCount) {
            const float* data = reinterpret_cast<const float*>(
              &current.fConsts[rangeStart]);
            if constexpr (ShaderType == D3D9ShaderType::VertexShader)
              SetVertexShaderConstantF(rangeStart, data, rangeCount);
            else
              SetPixelShaderConstantF(rangeStart, data, rangeCount);
          });
        } else {
          captureRanges(captures.iConsts, [&] (UINT rangeStart, UINT rangeCount) {
            const int* data = reinterpret_cast<const int*>(
              &current.iConsts[rangeStart]);
            if constexpr (ShaderType == D3D9ShaderType::VertexShader)
              SetVertexShaderConstantI(rangeStart, data, rangeCount);
            else
              SetPixelShaderConstantI(rangeStart, data, rangeCount);
          });
        }
      }
    };

    if constexpr (ShaderType == D3D9ShaderType::VertexShader) {
      capture(
        m_captures.vsConsts,
        m_deviceState->vsConsts.get(),
        m_state.vsConsts.get());
    } else {
      capture(
        m_captures.psConsts,
        m_deviceState->psConsts.get(),
        m_state.psConsts.get());
    }
  }


  void D3D9StateBlock::CaptureCurrentVertexShaderConstantF(UINT StartRegister, UINT Count) {
    CaptureCurrentShaderConstants<
      D3D9ShaderType::VertexShader,
      D3D9ConstantType::Float>(StartRegister, Count);
  }


  void D3D9StateBlock::CaptureCurrentVertexShaderConstantI(UINT StartRegister, UINT Count) {
    CaptureCurrentShaderConstants<
      D3D9ShaderType::VertexShader,
      D3D9ConstantType::Int>(StartRegister, Count);
  }


  void D3D9StateBlock::CaptureCurrentVertexShaderConstantB(UINT StartRegister, UINT Count) {
    CaptureCurrentShaderConstants<
      D3D9ShaderType::VertexShader,
      D3D9ConstantType::Bool>(StartRegister, Count);
  }


  void D3D9StateBlock::CaptureCurrentPixelShaderConstantF(UINT StartRegister, UINT Count) {
    CaptureCurrentShaderConstants<
      D3D9ShaderType::PixelShader,
      D3D9ConstantType::Float>(StartRegister, Count);
  }


  void D3D9StateBlock::CaptureCurrentPixelShaderConstantI(UINT StartRegister, UINT Count) {
    CaptureCurrentShaderConstants<
      D3D9ShaderType::PixelShader,
      D3D9ConstantType::Int>(StartRegister, Count);
  }


  void D3D9StateBlock::CaptureCurrentPixelShaderConstantB(UINT StartRegister, UINT Count) {
    CaptureCurrentShaderConstants<
      D3D9ShaderType::PixelShader,
      D3D9ConstantType::Bool>(StartRegister, Count);
  }


  void D3D9StateBlock::CaptureCurrentNPatchMode() {
    if (!m_captures.flags.test(D3D9CapturedStateFlag::NPatchMode))
      SetNPatchMode(m_deviceState->nPatchSegments);
  }


  HRESULT D3D9StateBlock::SetVertexBoolBitfield(uint32_t idx, uint32_t mask, uint32_t bits) {
    m_state.vsConsts->bConsts[idx] &= ~mask;
    m_state.vsConsts->bConsts[idx] |= bits & mask;
    return D3D_OK;
  }


  HRESULT D3D9StateBlock::SetPixelBoolBitfield(uint32_t idx, uint32_t mask, uint32_t bits) {
    m_state.psConsts->bConsts[idx] &= ~mask;
    m_state.psConsts->bConsts[idx] |= bits & mask;
    return D3D_OK;
  }


  void D3D9StateBlock::CapturePixelRenderStates() {
    m_captures.flags.set(D3D9CapturedStateFlag::RenderStates);

    m_captures.renderStates.set(D3DRS_ZENABLE, true);
    m_captures.renderStates.set(D3DRS_FILLMODE, true);
    m_captures.renderStates.set(D3DRS_SHADEMODE, true);
    m_captures.renderStates.set(D3DRS_ZWRITEENABLE, true);
    m_captures.renderStates.set(D3DRS_ALPHATESTENABLE, true);
    m_captures.renderStates.set(D3DRS_LASTPIXEL, true);
    m_captures.renderStates.set(D3DRS_SRCBLEND, true);
    m_captures.renderStates.set(D3DRS_DESTBLEND, true);
    m_captures.renderStates.set(D3DRS_ZFUNC, true);
    m_captures.renderStates.set(D3DRS_ALPHAREF, true);
    m_captures.renderStates.set(D3DRS_ALPHAFUNC, true);
    m_captures.renderStates.set(D3DRS_DITHERENABLE, true);
    m_captures.renderStates.set(D3DRS_FOGSTART, true);
    m_captures.renderStates.set(D3DRS_FOGEND, true);
    m_captures.renderStates.set(D3DRS_FOGDENSITY, true);
    m_captures.renderStates.set(D3DRS_ALPHABLENDENABLE, true);
    m_captures.renderStates.set(D3DRS_DEPTHBIAS, true);
    m_captures.renderStates.set(D3DRS_STENCILENABLE, true);
    m_captures.renderStates.set(D3DRS_STENCILFAIL, true);
    m_captures.renderStates.set(D3DRS_STENCILZFAIL, true);
    m_captures.renderStates.set(D3DRS_STENCILPASS, true);
    m_captures.renderStates.set(D3DRS_STENCILFUNC, true);
    m_captures.renderStates.set(D3DRS_STENCILREF, true);
    m_captures.renderStates.set(D3DRS_STENCILMASK, true);
    m_captures.renderStates.set(D3DRS_STENCILWRITEMASK, true);
    m_captures.renderStates.set(D3DRS_TEXTUREFACTOR, true);
    m_captures.renderStates.set(D3DRS_WRAP0, true);
    m_captures.renderStates.set(D3DRS_WRAP1, true);
    m_captures.renderStates.set(D3DRS_WRAP2, true);
    m_captures.renderStates.set(D3DRS_WRAP3, true);
    m_captures.renderStates.set(D3DRS_WRAP4, true);
    m_captures.renderStates.set(D3DRS_WRAP5, true);
    m_captures.renderStates.set(D3DRS_WRAP6, true);
    m_captures.renderStates.set(D3DRS_WRAP7, true);
    m_captures.renderStates.set(D3DRS_WRAP8, true);
    m_captures.renderStates.set(D3DRS_WRAP9, true);
    m_captures.renderStates.set(D3DRS_WRAP10, true);
    m_captures.renderStates.set(D3DRS_WRAP11, true);
    m_captures.renderStates.set(D3DRS_WRAP12, true);
    m_captures.renderStates.set(D3DRS_WRAP13, true);
    m_captures.renderStates.set(D3DRS_WRAP14, true);
    m_captures.renderStates.set(D3DRS_WRAP15, true);
    m_captures.renderStates.set(D3DRS_COLORWRITEENABLE, true);
    m_captures.renderStates.set(D3DRS_BLENDOP, true);
    m_captures.renderStates.set(D3DRS_SCISSORTESTENABLE, true);
    m_captures.renderStates.set(D3DRS_SLOPESCALEDEPTHBIAS, true);
    m_captures.renderStates.set(D3DRS_ANTIALIASEDLINEENABLE, true);
    m_captures.renderStates.set(D3DRS_TWOSIDEDSTENCILMODE, true);
    m_captures.renderStates.set(D3DRS_CCW_STENCILFAIL, true);
    m_captures.renderStates.set(D3DRS_CCW_STENCILZFAIL, true);
    m_captures.renderStates.set(D3DRS_CCW_STENCILPASS, true);
    m_captures.renderStates.set(D3DRS_CCW_STENCILFUNC, true);
    m_captures.renderStates.set(D3DRS_COLORWRITEENABLE1, true);
    m_captures.renderStates.set(D3DRS_COLORWRITEENABLE2, true);
    m_captures.renderStates.set(D3DRS_COLORWRITEENABLE3, true);
    m_captures.renderStates.set(D3DRS_BLENDFACTOR, true);
    m_captures.renderStates.set(D3DRS_SRGBWRITEENABLE, true);
    m_captures.renderStates.set(D3DRS_SEPARATEALPHABLENDENABLE, true);
    m_captures.renderStates.set(D3DRS_SRCBLENDALPHA, true);
    m_captures.renderStates.set(D3DRS_DESTBLENDALPHA, true);
    m_captures.renderStates.set(D3DRS_BLENDOPALPHA, true);
  }


  void D3D9StateBlock::CapturePixelSamplerStates() {
    m_captures.flags.set(D3D9CapturedStateFlag::SamplerStates);

    for (uint32_t i = 0; i < FirstVSSamplerSlot; i++) {
      m_captures.samplers.set(i, true);

      m_captures.samplerStates[i].set(D3DSAMP_ADDRESSU, true);
      m_captures.samplerStates[i].set(D3DSAMP_ADDRESSV, true);
      m_captures.samplerStates[i].set(D3DSAMP_ADDRESSW, true);
      m_captures.samplerStates[i].set(D3DSAMP_BORDERCOLOR, true);
      m_captures.samplerStates[i].set(D3DSAMP_MAGFILTER, true);
      m_captures.samplerStates[i].set(D3DSAMP_MINFILTER, true);
      m_captures.samplerStates[i].set(D3DSAMP_MIPFILTER, true);
      m_captures.samplerStates[i].set(D3DSAMP_MIPMAPLODBIAS, true);
      m_captures.samplerStates[i].set(D3DSAMP_MAXMIPLEVEL, true);
      m_captures.samplerStates[i].set(D3DSAMP_MAXANISOTROPY, true);
      m_captures.samplerStates[i].set(D3DSAMP_SRGBTEXTURE, true);
      m_captures.samplerStates[i].set(D3DSAMP_ELEMENTINDEX, true);
    }
  }


  void D3D9StateBlock::CapturePixelShaderStates() {
    m_captures.flags.set(D3D9CapturedStateFlag::PixelShader);
    m_captures.flags.set(D3D9CapturedStateFlag::PsConstants);

    m_captures.psConsts.fConsts.setAll();
    m_captures.psConsts.iConsts.setAll();
    m_captures.psConsts.bConsts.setAll();
    m_captures.psConsts.fDwordCount = m_captures.psConsts.fConsts.dwordCount();
    m_captures.psConsts.iDwordCount = m_captures.psConsts.iConsts.dwordCount();
    m_captures.psConsts.bDwordCount = m_captures.psConsts.bConsts.dwordCount();
  }


  void D3D9StateBlock::CaptureVertexRenderStates() {
    m_captures.flags.set(D3D9CapturedStateFlag::RenderStates);

    m_captures.renderStates.set(D3DRS_CULLMODE, true);
    m_captures.renderStates.set(D3DRS_FOGENABLE, true);
    m_captures.renderStates.set(D3DRS_FOGCOLOR, true);
    m_captures.renderStates.set(D3DRS_FOGTABLEMODE, true);
    m_captures.renderStates.set(D3DRS_FOGSTART, true);
    m_captures.renderStates.set(D3DRS_FOGEND, true);
    m_captures.renderStates.set(D3DRS_FOGDENSITY, true);
    m_captures.renderStates.set(D3DRS_RANGEFOGENABLE, true);
    m_captures.renderStates.set(D3DRS_AMBIENT, true);
    m_captures.renderStates.set(D3DRS_COLORVERTEX, true);
    m_captures.renderStates.set(D3DRS_FOGVERTEXMODE, true);
    m_captures.renderStates.set(D3DRS_CLIPPING, true);
    m_captures.renderStates.set(D3DRS_LIGHTING, true);
    m_captures.renderStates.set(D3DRS_LOCALVIEWER, true);
    m_captures.renderStates.set(D3DRS_EMISSIVEMATERIALSOURCE, true);
    m_captures.renderStates.set(D3DRS_AMBIENTMATERIALSOURCE, true);
    m_captures.renderStates.set(D3DRS_DIFFUSEMATERIALSOURCE, true);
    m_captures.renderStates.set(D3DRS_SPECULARMATERIALSOURCE, true);
    m_captures.renderStates.set(D3DRS_VERTEXBLEND, true);
    m_captures.renderStates.set(D3DRS_CLIPPLANEENABLE, true);
    m_captures.renderStates.set(D3DRS_POINTSIZE, true);
    m_captures.renderStates.set(D3DRS_POINTSIZE_MIN, true);
    m_captures.renderStates.set(D3DRS_POINTSPRITEENABLE, true);
    m_captures.renderStates.set(D3DRS_POINTSCALEENABLE, true);
    m_captures.renderStates.set(D3DRS_POINTSCALE_A, true);
    m_captures.renderStates.set(D3DRS_POINTSCALE_B, true);
    m_captures.renderStates.set(D3DRS_POINTSCALE_C, true);
    m_captures.renderStates.set(D3DRS_MULTISAMPLEANTIALIAS, true);
    m_captures.renderStates.set(D3DRS_MULTISAMPLEMASK, true);
    m_captures.renderStates.set(D3DRS_PATCHEDGESTYLE, true);
    m_captures.renderStates.set(D3DRS_POINTSIZE_MAX, true);
    m_captures.renderStates.set(D3DRS_INDEXEDVERTEXBLENDENABLE, true);
    m_captures.renderStates.set(D3DRS_TWEENFACTOR, true);
    m_captures.renderStates.set(D3DRS_POSITIONDEGREE, true);
    m_captures.renderStates.set(D3DRS_NORMALDEGREE, true);
    m_captures.renderStates.set(D3DRS_MINTESSELLATIONLEVEL, true);
    m_captures.renderStates.set(D3DRS_MAXTESSELLATIONLEVEL, true);
    m_captures.renderStates.set(D3DRS_ADAPTIVETESS_X, true);
    m_captures.renderStates.set(D3DRS_ADAPTIVETESS_Y, true);
    m_captures.renderStates.set(D3DRS_ADAPTIVETESS_Z, true);
    m_captures.renderStates.set(D3DRS_ADAPTIVETESS_W, true);
    m_captures.renderStates.set(D3DRS_ENABLEADAPTIVETESSELLATION, true);
    m_captures.renderStates.set(D3DRS_NORMALIZENORMALS, true);
    m_captures.renderStates.set(D3DRS_SPECULARENABLE, true);
    m_captures.renderStates.set(D3DRS_SHADEMODE, true);
  }


  void D3D9StateBlock::CaptureVertexSamplerStates() {
    m_captures.flags.set(D3D9CapturedStateFlag::SamplerStates);

    for (uint32_t i = FirstVSSamplerSlot; i < SamplerCount; i++) {
      m_captures.samplers.set(i, true);
      m_captures.samplerStates[i].set(D3DSAMP_DMAPOFFSET, true);
    }
  }


  void D3D9StateBlock::CaptureVertexShaderStates() {
    m_captures.flags.set(D3D9CapturedStateFlag::VertexShader);
    m_captures.flags.set(D3D9CapturedStateFlag::VsConstants);

    const auto& layout = m_parent->GetVertexConstantLayout();
    m_captures.vsConsts.fConsts.setN(layout.floatCount);
    m_captures.vsConsts.iConsts.setN(layout.intCount);
    m_captures.vsConsts.bConsts.setN(layout.boolCount);
    m_captures.vsConsts.fDwordCount = (layout.floatCount + 31) / 32;
    m_captures.vsConsts.iDwordCount = (layout.intCount + 31) / 32;
    m_captures.vsConsts.bDwordCount = (layout.boolCount + 31) / 32;
  }


  void D3D9StateBlock::CaptureType(D3D9StateBlockType Type) {
    if (Type == D3D9StateBlockType::PixelState || Type == D3D9StateBlockType::All) {
      CapturePixelRenderStates();
      CapturePixelSamplerStates();
      CapturePixelShaderStates();

      m_captures.flags.set(D3D9CapturedStateFlag::TextureStages);
      m_captures.textureStages.setAll();
      for (auto& stage : m_captures.textureStageStates)
        stage.setAll();
    }

    if (Type == D3D9StateBlockType::VertexState || Type == D3D9StateBlockType::All) {
      CaptureVertexRenderStates();
      CaptureVertexSamplerStates();
      CaptureVertexShaderStates();

      m_captures.flags.set(D3D9CapturedStateFlag::VertexDecl);
      m_captures.flags.set(D3D9CapturedStateFlag::StreamFreq);
      m_captures.flags.set(D3D9CapturedStateFlag::Lights);
      m_captures.lightChanges.setN(m_deviceState->lights.size());
      m_captures.lightEnabledChanges.setN(m_deviceState->lights.size());

      for (uint32_t i = 0; i < caps::MaxStreams; i++)
        m_captures.streamFreq.set(i, true);
    }

    if (Type == D3D9StateBlockType::All) {
      m_captures.flags.set(D3D9CapturedStateFlag::Textures);
      m_captures.textures.setAll();

      m_captures.flags.set(D3D9CapturedStateFlag::VertexBuffers);
      m_captures.vertexBuffers.setAll();

      m_captures.flags.set(D3D9CapturedStateFlag::Indices);
      m_captures.flags.set(D3D9CapturedStateFlag::Viewport);
      m_captures.flags.set(D3D9CapturedStateFlag::ScissorRect);

      m_captures.flags.set(D3D9CapturedStateFlag::ClipPlanes);
      m_captures.clipPlanes.setAll();

      m_captures.flags.set(D3D9CapturedStateFlag::Transforms);
      m_captures.transforms.setAll();

      m_captures.flags.set(D3D9CapturedStateFlag::Material);
    }

    if (Type != D3D9StateBlockType::None) {
      if (m_captures.flags.test(D3D9CapturedStateFlag::VertexDecl))
        SetVertexDeclaration(m_deviceState->vertexDecl.ptr());

      ApplyOrCapture<D3D9StateFunction::Capture, false>();
    }
  }

}
