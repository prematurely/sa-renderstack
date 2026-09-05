#pragma once
#include <sa_renderstack/backend_api.h>
#include <expected>
#include <string>
#include <vector>
#include <span>
namespace api5 {
class EffectStateBatchBuilder {
  D3D9GtaSaEffectStateBatch batch_{}; std::vector<D3D9GtaSaTransformBinding> transforms_; std::vector<D3D9GtaSaRenderStateBinding> states_; std::vector<D3D9GtaSaFloatConstantRange> vf_,pf_; std::vector<D3D9GtaSaIntConstantRange> vi_,pi_; std::vector<D3D9GtaSaBoolConstantRange> vb_,pb_;
 public:
  EffectStateBatchBuilder();
  EffectStateBatchBuilder& transforms(std::span<const D3D9GtaSaTransformBinding> v); EffectStateBatchBuilder& renderStates(std::span<const D3D9GtaSaRenderStateBinding> v);
  EffectStateBatchBuilder& vertexFloat(std::span<const D3D9GtaSaFloatConstantRange> v); EffectStateBatchBuilder& pixelFloat(std::span<const D3D9GtaSaFloatConstantRange> v);
  EffectStateBatchBuilder& vertexInt(std::span<const D3D9GtaSaIntConstantRange> v); EffectStateBatchBuilder& pixelInt(std::span<const D3D9GtaSaIntConstantRange> v); EffectStateBatchBuilder& vertexBool(std::span<const D3D9GtaSaBoolConstantRange> v); EffectStateBatchBuilder& pixelBool(std::span<const D3D9GtaSaBoolConstantRange> v);
  EffectStateBatchBuilder& flags(UINT f); EffectStateBatchBuilder& fvf(DWORD f); EffectStateBatchBuilder& material(const D3DMATERIAL9& m);
  std::expected<const D3D9GtaSaEffectStateBatch*,std::string> validate() noexcept; const D3D9GtaSaEffectStateBatch* get() const noexcept { return &batch_; }
}; }
