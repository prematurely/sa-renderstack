#pragma once

#include "../d3d9/d3d9_bridge_telemetry.h"
#include <atomic>
#include <exception>
#include <mutex>
#include <vector>

namespace dxvk {
  // Device-owned. No recorder lock is held across the operation being timed.
  class DxvkTelemetry {
  public:
    struct Token { uint64_t generation = 0, beginNs = 0; uint32_t threadId = 0; };
    bool enabled() const { return m_active.load(std::memory_order_acquire) != 0; }
    static uint64_t nowNs();
    HRESULT start(const D3D9BridgeTelemetryConfig* config);
    HRESULT stop(uint64_t captureId);
    HRESULT read(uint64_t captureId, D3D9BridgeTelemetryRead* output,
      D3D9BridgeTelemetryEvent* events, uint32_t capacity);
    uint64_t activeGeneration(uint64_t captureId);
    bool isGenerationActive(uint64_t captureId, uint64_t generation);
    Token begin();
    void finish(Token token, uint32_t kind, uint64_t identity, uint64_t bytes,
      int32_t result, uint32_t flags);
  private:
    std::atomic<uint64_t> m_active = { 0 };
    std::mutex m_mutex;
    std::vector<D3D9BridgeTelemetryEvent> m_events;
    uint64_t m_generation = 0, m_captureId = 0;
    uint64_t m_minDuration = 0, m_minBytes = 0;
    uint64_t m_started = 0, m_inFlight = 0, m_inFlightAtStop = 0;
    uint64_t m_dropped = 0, m_filtered = 0, m_lateDropped = 0;
    uint32_t m_head = 0, m_count = 0;
  };

  class DxvkTelemetryScope {
  public:
    DxvkTelemetryScope(DxvkTelemetry* recorder, uint32_t kind, uint64_t identity = 0,
        uint64_t bytes = 0, uint32_t flags = 0)
    : m_recorder(recorder), m_kind(kind), m_flags(flags), m_identity(identity), m_bytes(bytes) {
      if (m_recorder) m_token = m_recorder->begin();
      if (m_token.generation) m_uncaught = std::uncaught_exceptions();
    }
    ~DxvkTelemetryScope() {
      if (m_token.generation) {
        if (std::uncaught_exceptions() > m_uncaught && !(m_flags & D3D9_BRIDGE_TELEMETRY_FAILED))
          fail(-1);
        m_recorder->finish(m_token, m_kind, m_identity, m_bytes, m_result, m_flags);
      }
    }
    void identity(uint64_t value) { m_identity = value; }
    void bytes(uint64_t value) { m_bytes = value; }
    void fail(int32_t result) { m_result = result; m_flags |= D3D9_BRIDGE_TELEMETRY_FAILED; }
    void outcome(int32_t result) { m_result = result; if (result < 0) m_flags |= D3D9_BRIDGE_TELEMETRY_FAILED; }
    void flags(uint32_t value) { m_flags |= value; }
  private:
    DxvkTelemetry* m_recorder;
    DxvkTelemetry::Token m_token;
    uint32_t m_kind, m_flags;
    uint64_t m_identity, m_bytes;
    int32_t m_result = 0;
    int m_uncaught = 0;
  };
}
