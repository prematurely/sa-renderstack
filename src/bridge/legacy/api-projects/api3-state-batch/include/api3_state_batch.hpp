#pragma once
#include <sa_renderstack/backend_api.h>
#include <span>
#include <vector>
#include <expected>

namespace api3 {
class StateBatchBuilder {
public:
  std::expected<void, HRESULT> add_vertex_float(UINT start, std::span<const float> values) noexcept;
  std::expected<void, HRESULT> add_pixel_float(UINT start, std::span<const float> values) noexcept;
  std::expected<D3D9GtaSaStateBatch, HRESULT> view() const noexcept;
  HRESULT submit(ID3D9GtaSaCompatDevice2* device) const noexcept;
private:
  struct Range { UINT start{}; std::vector<float> values; };
  std::vector<Range> m_vs, m_ps;
  mutable std::vector<D3D9GtaSaFloatConstantRange> m_vsDesc, m_psDesc;
  static std::expected<void, HRESULT> add(std::vector<Range>&, UINT, std::span<const float>, UINT) noexcept;
  static bool valid(const Range&, UINT) noexcept;
};
}
