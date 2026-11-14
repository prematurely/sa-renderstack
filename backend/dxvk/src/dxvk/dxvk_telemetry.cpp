#include "dxvk_telemetry.h"
#include <algorithm>
#include <new>

namespace dxvk {
  uint64_t DxvkTelemetry::nowNs() {
    static const uint64_t frequency = [] { LARGE_INTEGER v{}; QueryPerformanceFrequency(&v); return uint64_t(v.QuadPart); }();
    LARGE_INTEGER ticks{}; QueryPerformanceCounter(&ticks);
    if (!frequency || ticks.QuadPart < 0) return 0;
    uint64_t t = uint64_t(ticks.QuadPart);
    return (t / frequency) * 1000000000ull + (t % frequency) * 1000000000ull / frequency;
  }
  HRESULT DxvkTelemetry::start(const D3D9BridgeTelemetryConfig* c) {
    if (!c) return E_POINTER;
    if (c->StructSize != sizeof(*c) || c->Version != D3D9_BRIDGE_TELEMETRY_VERSION ||
        !c->CaptureId || !c->MaxEvents || c->MaxEvents > D3D9_BRIDGE_TELEMETRY_MAX_EVENTS || c->Reserved)
      return D3DERR_INVALIDCALL;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_active.load(std::memory_order_relaxed) || c->CaptureId <= m_captureId) return D3DERR_INVALIDCALL;
    try { m_events.resize(c->MaxEvents); } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    if (++m_generation == 0) ++m_generation;
    m_captureId=c->CaptureId; m_minDuration=c->MinDurationNs; m_minBytes=c->MinBytes;
    m_started=m_inFlight=m_inFlightAtStop=m_dropped=m_filtered=m_lateDropped=0;
    m_head=m_count=0;
    m_active.store(m_generation,std::memory_order_release);
    return S_OK;
  }
  HRESULT DxvkTelemetry::stop(uint64_t id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!id || id != m_captureId) return D3DERR_INVALIDCALL;
    if (!m_active.load(std::memory_order_relaxed)) return S_FALSE;
    m_inFlightAtStop=m_inFlight;
    m_active.store(0,std::memory_order_release);
    return S_OK;
  }
  HRESULT DxvkTelemetry::read(uint64_t id, D3D9BridgeTelemetryRead* out,
      D3D9BridgeTelemetryEvent* events, uint32_t capacity) {
    if (!out) return E_POINTER;
    if (out->StructSize != sizeof(*out) || out->Version != D3D9_BRIDGE_TELEMETRY_VERSION ||
        capacity>D3D9_BRIDGE_TELEMETRY_MAX_EVENTS || (capacity && !events)) return D3DERR_INVALIDCALL;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!id || id != m_captureId) return D3DERR_INVALIDCALL;
    D3D9BridgeTelemetryRead result{};
    result.StructSize=sizeof(result); result.Version=D3D9_BRIDGE_TELEMETRY_VERSION;
    result.CaptureId=id; result.Dropped=m_dropped; result.Filtered=m_filtered;
    result.Started=m_started; result.InFlight=m_inFlight; result.InFlightAtStop=m_inFlightAtStop;
    result.LateDropped=m_lateDropped; result.Capacity=uint32_t(m_events.size());
    result.Active=m_active.load(std::memory_order_relaxed) != 0;
    result.Written=std::min(capacity,m_count);
    for (uint32_t i=0; i<result.Written; i++) events[i]=m_events[(m_head+i)%m_events.size()];
    m_head=(m_head+result.Written)%m_events.size(); m_count-=result.Written;
    result.Remaining=m_count; *out=result; return S_OK;
  }
  uint64_t DxvkTelemetry::activeGeneration(uint64_t id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return id && id==m_captureId ? m_active.load(std::memory_order_relaxed) : 0;
  }
  bool DxvkTelemetry::isGenerationActive(uint64_t id, uint64_t generation) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return id && id==m_captureId && generation && m_active.load(std::memory_order_relaxed)==generation;
  }
  DxvkTelemetry::Token DxvkTelemetry::begin() {
    uint64_t gen=m_active.load(std::memory_order_acquire);
    if (!gen) return {};
    std::lock_guard<std::mutex> lock(m_mutex);
    if (gen!=m_active.load(std::memory_order_relaxed)) return {};
    ++m_started; ++m_inFlight;
    return {gen,nowNs(),GetCurrentThreadId()};
  }
  void DxvkTelemetry::finish(Token token, uint32_t kind, uint64_t identity,
      uint64_t bytes, int32_t result, uint32_t flags) {
    uint64_t end=nowNs();
    std::lock_guard<std::mutex> lock(m_mutex);
    if (token.generation != m_generation) return;
    --m_inFlight;
    if (!m_active.load(std::memory_order_relaxed)) { ++m_lateDropped; return; }
    if (!(flags&D3D9_BRIDGE_TELEMETRY_FAILED) && end-token.beginNs<m_minDuration && bytes<m_minBytes) {
      ++m_filtered; return;
    }
    if (m_count==m_events.size()) { ++m_dropped; return; }
    auto& e=m_events[(m_head+m_count)%m_events.size()];
    e={m_captureId,token.beginNs,end,identity,bytes,kind,token.threadId,result,flags}; ++m_count;
  }
}
