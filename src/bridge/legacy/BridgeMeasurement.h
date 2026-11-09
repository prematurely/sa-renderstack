#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

// Pure measurement policy. Timestamps are monotonic nanoseconds; recorded
// intervals describe successful Presents, never CPU/GPU execution duration.
namespace BridgeMeasurement {

enum class Phase : std::uint32_t {
  Ready, Baseline, Cpu, CpuFinishing, Attribution, Complete, Interrupted, Error,
  AttributionFinishing
};
enum class Action { None, StartCpu, StartAttribution, FinishAttribution, ReportError };
inline const char* PhaseName(Phase phase) {
  switch (phase) {
    case Phase::Ready: return "Ready";
    case Phase::Baseline: return "Baseline";
    case Phase::Cpu: return "CPU sampling (overhead)";
    case Phase::CpuFinishing: return "Processing CPU samples";
    case Phase::Attribution: return "State attribution (overhead)";
    case Phase::Complete: return "Complete";
    case Phase::Interrupted: return "Interrupted";
    case Phase::Error: return "Error";
    case Phase::AttributionFinishing: return "Waiting for final transactions";
  }
  return "Unknown";
}

struct FrameStats {
  std::uint64_t presents = 0, failedPresents = 0, invalidTimes = 0;
  std::uint64_t startNs = 0, endNs = 0, lastPresentNs = 0, lastObservedNs = 0;
  std::uint64_t totalIntervalNs = 0, maxIntervalNs = 0;
  void Begin(std::uint64_t now) {
    *this = {};
    startNs = endNs = lastPresentNs = lastObservedNs = now;
  }
  bool Observe(std::uint64_t now, bool success) {
    if (now < lastObservedNs) { ++invalidTimes; return false; }
    lastObservedNs = endNs = now;
    if (!success) { ++failedPresents; return false; }
    const auto interval = now - lastPresentNs;
    totalIntervalNs += interval;
    maxIntervalNs = std::max(maxIntervalNs, interval);
    lastPresentNs = now;
    ++presents;
    return true;
  }
  void Finish(std::uint64_t now) { endNs = std::max(endNs, now); }
  std::uint64_t elapsedNs() const { return endNs >= startNs ? endNs - startNs : 0; }
  double fps() const { return elapsedNs() ? 1e9 * double(presents) / double(elapsedNs()) : 0.0; }
  double averageIntervalNs() const { return presents ? double(totalIntervalNs) / double(presents) : 0.0; }
};

struct FrameRecord {
  std::uint64_t beginNs = 0, endNs = 0, frameId = 0;
  Phase phase = Phase::Ready;
};
class FrameTrace {
public:
  static constexpr std::size_t Capacity = 4096;
  void Clear() { m_count = m_dropped = 0; }
  void Add(std::uint64_t begin, std::uint64_t end, std::uint64_t id, Phase phase) {
    if (end <= begin) return;
    if (m_count == Capacity) { ++m_dropped; return; }
    m_rows[m_count++] = {begin, end, id, phase};
  }
  const FrameRecord* Find(std::uint64_t time) const {
    const auto first = m_rows.begin(), last = first + m_count;
    const auto it = std::upper_bound(first, last, time,
      [](std::uint64_t value, const FrameRecord& row) { return value < row.beginNs; });
    if (it == first) return nullptr;
    const auto& row = *(it - 1);
    return time >= row.beginNs && time < row.endNs ? &row : nullptr;
  }
  bool IsSlow(std::uint64_t time, std::uint64_t thresholdNs) const {
    const auto* row = Find(time);
    return row && row->endNs - row->beginNs > thresholdNs;
  }
  const FrameRecord* data() const { return m_rows.data(); }
  std::size_t size() const { return m_count; }
  std::uint64_t dropped() const { return m_dropped; }
private:
  std::array<FrameRecord, Capacity> m_rows{};
  std::size_t m_count = 0;
  std::uint64_t m_dropped = 0;
};

struct Options {
  std::uint32_t baselineMs = 5000, cpuMs = 10000, attributionMs = 3000, slowFrameMs = 50;
};
class Controller {
public:
  explicit Controller(Options options = {}) : m_options(options) { }
  void Configure(Options options) { if (!active()) m_options = options; }
  void SetAvailable(bool cpu, bool attribution) { m_cpuAvailable = cpu; m_attributionAvailable = attribution; }
  bool Key(bool f7, bool f8, std::uint64_t now) {
    const bool down = f7 || f8;
    const bool edge = down && !m_keyDown;
    m_keyDown = down;
    if (!edge || active()) return false;
    ++m_capture;
    if (!m_capture) ++m_capture;
    m_stats = {};
    m_trace.Clear();
    m_frame = 0;
    m_cpuDone = m_cpuFailed = false;
    m_unavailable = (!m_cpuAvailable ? 2u : 0u) | (!m_attributionAvailable ? 4u : 0u);
    Enter(Phase::Baseline, now);
    return true;
  }
  void SuppressKeys(bool f7, bool f8) { m_keyDown = f7 || f8; }
  Action Present(std::uint64_t now, bool success) {
    if (!active()) return Action::None;
    if ((m_phase == Phase::Cpu || m_phase == Phase::CpuFinishing) && m_cpuFailed) {
      m_phase = Phase::Error;
      return Action::ReportError;
    }
    if (m_phase == Phase::CpuFinishing) {
      if (!m_cpuDone) return Action::None;
      return BeginAttribution(now);
    }
    const unsigned index = phaseIndex();
    const auto before = m_stats[index].lastPresentNs;
    if (m_stats[index].Observe(now, success))
      m_trace.Add(before, now, ++m_frame, m_phase);
    if (m_stats[index].elapsedNs() < std::uint64_t(durationMs()) * 1'000'000ull)
      return Action::None;
    m_stats[index].Finish(now);
    if (m_phase == Phase::Baseline) {
      if (!m_cpuAvailable) return BeginAttribution(now);
      Enter(Phase::Cpu, now);
      return Action::StartCpu;
    }
    if (m_phase == Phase::Cpu) {
      m_phase = Phase::CpuFinishing;
      if (m_cpuDone) return BeginAttribution(now);
      return Action::None;
    }
    m_phase = Phase::Complete;
    return Action::FinishAttribution;
  }
  void CpuFinished(bool success) { m_cpuDone = success; m_cpuFailed = !success; }
  void Interrupt() { if (active()) m_phase = Phase::Interrupted; }
  Phase phase() const { return m_phase; }
  bool active() const {
    return m_phase == Phase::Baseline || m_phase == Phase::Cpu ||
      m_phase == Phase::CpuFinishing || m_phase == Phase::Attribution;
  }
  std::uint64_t captureId() const { return m_capture; }
  std::uint32_t unavailableMask() const { return m_unavailable; }
  const FrameStats& stats(unsigned index) const { return m_stats[std::min(index, 2u)]; }
  const FrameTrace& trace() const { return m_trace; }
  std::uint32_t durationMs() const {
    if (m_phase == Phase::Baseline) return m_options.baselineMs;
    if (m_phase == Phase::Cpu) return m_options.cpuMs;
    if (m_phase == Phase::Attribution) return m_options.attributionMs;
    return 0;
  }
  unsigned phaseIndex() const {
    return m_phase == Phase::Baseline ? 0u : (m_phase == Phase::Attribution ? 2u : 1u);
  }
private:
  void Enter(Phase phase, std::uint64_t now) {
    m_phase = phase;
    m_stats[phaseIndex()].Begin(now);
  }
  Action BeginAttribution(std::uint64_t now) {
    if (!m_attributionAvailable) { m_phase = Phase::Complete; return Action::FinishAttribution; }
    Enter(Phase::Attribution, now);
    return Action::StartAttribution;
  }
  Options m_options;
  std::array<FrameStats, 3> m_stats{};
  FrameTrace m_trace;
  Phase m_phase = Phase::Ready;
  std::uint64_t m_capture = 0, m_frame = 0;
  std::uint32_t m_unavailable = 0;
  bool m_keyDown = false, m_cpuAvailable = true, m_attributionAvailable = true;
  bool m_cpuDone = false, m_cpuFailed = false;
};

class Jitter {
public:
  explicit Jitter(std::uint32_t seed) : m_state(seed ? seed : 0x9e3779b9u) { }
  std::uint32_t Next(std::uint32_t intervalMs) {
    if (intervalMs <= 1) return 1;
    m_state ^= m_state << 13;
    m_state ^= m_state >> 17;
    m_state ^= m_state << 5;
    const auto spread = std::max(1u, intervalMs / 4u);
    return std::uint32_t(std::max<std::int64_t>(1,
      std::int64_t(intervalMs) + std::int64_t(m_state % (2u * spread + 1u)) - spread));
  }
private:
  std::uint32_t m_state;
};

enum class ChainQuality { Unavailable, FrameChain, Partial, Invalid, Truncated };
struct CapturedChain {
  std::array<std::uintptr_t, 16> addresses{};
  std::uint32_t count = 0;
  ChainQuality quality = ChainQuality::Unavailable;
};
// Structural frame-chain validation on a bounded snapshot, not a full x86
// unwinder. Optimized/FPO frames may be missing and must remain unresolved.
template<typename Executable>
CapturedChain WalkCapturedFrames(std::uintptr_t stackBase, const void* bytes,
    std::size_t byteCount, std::uintptr_t frame, Executable executable) {
  CapturedChain result;
  if (!bytes || byteCount < 8 || !frame) return result;
  for (std::size_t i = 0; i < result.addresses.size(); ++i) {
    if (frame < stackBase || frame - stackBase > byteCount - 8 || (frame & 3u)) {
      result.quality = result.count ? ChainQuality::Partial : ChainQuality::Unavailable;
      return result;
    }
    std::uint32_t pair[2]{};
    std::memcpy(pair, static_cast<const std::uint8_t*>(bytes) + (frame - stackBase), sizeof(pair));
    if (!pair[1] || !executable(std::uintptr_t(pair[1]) - 1u)) {
      result.quality = ChainQuality::Invalid;
      return result;
    }
    result.addresses[result.count++] = pair[1];
    if (!pair[0]) { result.quality = ChainQuality::FrameChain; return result; }
    if (pair[0] <= frame) { result.quality = ChainQuality::Invalid; return result; }
    frame = pair[0];
  }
  result.quality = ChainQuality::Truncated;
  return result;
}

} // namespace BridgeMeasurement
