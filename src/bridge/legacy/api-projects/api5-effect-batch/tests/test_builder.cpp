#include "api5_effect_batch.hpp"
#include <array>
#include <cassert>

int main() {
  api5::EffectStateBatchBuilder builder;
  std::array<float, 4> values{1.f, 2.f, 3.f, 4.f};
  D3D9GtaSaFloatConstantRange range{0u, 1u, values.data()};
  builder.vertexFloat(std::span{&range, 1});
  auto valid = builder.validate();
  assert(valid.has_value());

  api5::EffectStateBatchBuilder invalid;
  D3D9GtaSaRenderStateBinding bad{
    static_cast<D3DRENDERSTATETYPE>(0xFFFFFFFFu), 0u};
  invalid.renderStates(std::span{&bad, 1});
  assert(!invalid.validate().has_value());
}
