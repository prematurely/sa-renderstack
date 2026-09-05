#include "api3_state_batch.hpp"
#include <algorithm>
#include <limits>

namespace api3 {
std::expected<void, HRESULT> StateBatchBuilder::add(std::vector<Range>& out, UINT start, std::span<const float> values, UINT capacity) noexcept {
  if (start > capacity || values.size() % 4 != 0 || values.size()/4 > capacity - start || values.size() > std::numeric_limits<UINT>::max()) return std::unexpected(D3DERR_INVALIDCALL);
  if (values.empty()) return {};
  try {
    out.push_back(Range{start, std::vector<float>(values.begin(), values.end())});
    std::sort(out.begin(), out.end(), [](const Range& a, const Range& b){ return a.start < b.start; });
    std::vector<Range> merged;
    for (auto& r : out) {
      if (merged.empty() || merged.back().start + merged.back().values.size()/4 < r.start) { merged.push_back(std::move(r)); continue; }
      auto& dst = merged.back();
      UINT end = std::max(dst.start + static_cast<UINT>(dst.values.size()/4), r.start + static_cast<UINT>(r.values.size()/4));
      std::vector<float> values2(static_cast<size_t>(end - dst.start)*4);
      std::copy(dst.values.begin(), dst.values.end(), values2.begin());
      std::copy(r.values.begin(), r.values.end(), values2.begin() + static_cast<size_t>(r.start - dst.start) * 4);
      dst.values = std::move(values2);
    }
    out = std::move(merged);
    return {};
  } catch (...) { return std::unexpected(E_OUTOFMEMORY); }
}

std::expected<void, HRESULT> StateBatchBuilder::add_vertex_float(UINT s, std::span<const float> v) noexcept { return add(m_vs,s,v,256); }
std::expected<void, HRESULT> StateBatchBuilder::add_pixel_float(UINT s, std::span<const float> v) noexcept { return add(m_ps,s,v,224); }
bool StateBatchBuilder::valid(const Range& r, UINT cap) noexcept { return r.start <= cap && r.values.size()%4==0 && r.values.size()/4 <= cap-r.start; }
std::expected<D3D9GtaSaStateBatch, HRESULT> StateBatchBuilder::view() const noexcept {
  try {
    D3D9GtaSaStateBatch b{}; b.StructSize=sizeof(b); b.ApiVersion=3;
    if (m_vs.size()>16 || m_ps.size()>16) return std::unexpected(D3DERR_INVALIDCALL);
    for (const auto& r:m_vs) if(!valid(r,256)) return std::unexpected(D3DERR_INVALIDCALL);
    for (const auto& r:m_ps) if(!valid(r,224)) return std::unexpected(D3DERR_INVALIDCALL);
    m_vsDesc.clear(); m_psDesc.clear();
    for (const auto& r:m_vs) m_vsDesc.push_back({r.start,static_cast<UINT>(r.values.size()/4),r.values.data()});
    for (const auto& r:m_ps) m_psDesc.push_back({r.start,static_cast<UINT>(r.values.size()/4),r.values.data()});
    b.VertexFloatRangeCount=static_cast<UINT>(m_vsDesc.size()); b.VertexFloatRanges=m_vsDesc.data(); b.PixelFloatRangeCount=static_cast<UINT>(m_psDesc.size()); b.PixelFloatRanges=m_psDesc.data(); return b;
  } catch (...) { return std::unexpected(E_OUTOFMEMORY); }
}
HRESULT StateBatchBuilder::submit(ID3D9GtaSaCompatDevice2* device) const noexcept { if(!device) return E_POINTER; try { auto b=view(); if(!b) return b.error(); return device->SubmitStateBatch(&*b); } catch (...) { return E_FAIL; } }
}
