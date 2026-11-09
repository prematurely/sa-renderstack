#include "api3_state_batch.hpp"
#include <algorithm>
#include <limits>

namespace api3 {
std::expected<void, HRESULT> StateBatchBuilder::add(std::vector<Range>& out, UINT start, std::span<const float> values, UINT capacity) noexcept {
  if (start > capacity || values.size() % 4 != 0 || values.size()/4 > capacity - start || values.size() > std::numeric_limits<UINT>::max()) return std::unexpected(D3DERR_INVALIDCALL);
  if (values.empty()) return {};
  try {
    Range pending{start, std::vector<float>(values.begin(), values.end())};
    std::vector<Range> next;
    next.reserve(out.size() + 1);
    bool inserted = false;
    for (const auto& range : out) {
      if (inserted) {
        next.push_back(range);
        continue;
      }
      const UINT rangeEnd = range.start + static_cast<UINT>(range.values.size() / 4);
      const UINT pendingEnd = pending.start + static_cast<UINT>(pending.values.size() / 4);
      if (rangeEnd < pending.start) {
        next.push_back(range);
      } else if (pendingEnd < range.start) {
        next.push_back(std::move(pending));
        next.push_back(range);
        inserted = true;
      } else {
        const UINT first = std::min(range.start, pending.start);
        const UINT end = std::max(rangeEnd, pendingEnd);
        std::vector<float> combined(static_cast<size_t>(end - first) * 4);
        std::copy(range.values.begin(), range.values.end(),
          combined.begin() + static_cast<size_t>(range.start - first) * 4);
        // Existing ranges are disjoint. The pending range contains the newest
        // write, so it wins every overlap regardless of register address order.
        std::copy(pending.values.begin(), pending.values.end(),
          combined.begin() + static_cast<size_t>(pending.start - first) * 4);
        pending = Range{first, std::move(combined)};
      }
    }
    if (!inserted) next.push_back(std::move(pending));
    // Preserve the previous builder if allocation/copying fails.
    out.swap(next);
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
