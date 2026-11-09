#include "test_check.hpp"
#include <array>
#include <cstdint>
#include <limits>

#if __has_include("../../BridgeMeasurement.h")
#include "../../BridgeMeasurement.h"
using namespace BridgeMeasurement;

void phases() {
  Controller c({5000, 10000, 3000, 50});
  CHECK(!c.Key(false, false, 0));
  CHECK(c.Key(false, true, 0));
  CHECK(c.phase() == Phase::Baseline);
  CHECK(!c.Key(false, true, 1));
  CHECK(!c.Key(true, true, 2));
  CHECK(c.Present(1'000'000'000ull, true) == Action::None);
  CHECK(c.Present(5'000'000'000ull, true) == Action::StartCpu);
  CHECK(c.phase() == Phase::Cpu);
  CHECK(c.stats(0).presents == 2);
  CHECK(c.stats(0).fps() == 0.4);
  CHECK(c.Present(15'000'000'000ull, true) == Action::None);
  CHECK(c.phase() == Phase::CpuFinishing);
  CHECK(c.stats(1).presents == 1);
  CHECK(c.stats(1).elapsedNs() == 10'000'000'000ull);
  c.CpuFinished(true);
  CHECK(c.Present(16'000'000'000ull, true) == Action::StartAttribution);
  CHECK(c.phase() == Phase::Attribution);
  CHECK(c.Present(19'000'000'000ull, true) == Action::FinishAttribution);
  CHECK(c.phase() == Phase::Complete);
  CHECK(c.stats(1).elapsedNs() == 10'000'000'000ull);
  CHECK(!c.Key(true, true, 20'000'000'000ull));
  CHECK(!c.Key(false, false, 20'000'000'001ull));
  CHECK(c.Key(true, false, 20'000'000'002ull));
  CHECK(c.captureId() == 2);
  CHECK(c.stats(0).presents == 0);
}

void failures() {
  Controller c({1, 1, 1, 50});
  CHECK(c.Key(true, false, 1));
  CHECK(c.Present(1'000'001, false) == Action::StartCpu);
  CHECK(c.stats(0).presents == 0);
  CHECK(c.stats(0).failedPresents == 1);
  c.CpuFinished(false);
  CHECK(c.Present(2'000'001, true) == Action::ReportError);
  CHECK(c.phase() == Phase::Error);
  CHECK(!c.active());
  c.Key(false, false, 3'000'001);
  CHECK(c.Key(false, true, 3'000'002));
  c.Interrupt();
  CHECK(c.phase() == Phase::Interrupted);
  CHECK(!c.active());
  Controller missing({1, 1, 1, 50});
  missing.SetAvailable(false, false);
  CHECK(missing.Key(false, true, 0));
  CHECK(missing.Present(1'000'000, true) == Action::FinishAttribution);
  CHECK(missing.phase() == Phase::Complete);
  CHECK(missing.unavailableMask() == 6);
  CHECK(missing.stats(1).presents == 0);
  CHECK(missing.stats(2).presents == 0);
}

void frame_math() {
  FrameStats stats;
  stats.Begin(1'000'000'000ull);
  CHECK(stats.Observe(1'020'000'000ull, true));
  CHECK(!stats.Observe(1'030'000'000ull, false));
  CHECK(stats.Observe(1'060'000'000ull, true));
  CHECK(!stats.Observe(1'050'000'000ull, true));
  stats.Finish(1'100'000'000ull);
  CHECK(stats.presents == 2);
  CHECK(stats.failedPresents == 1);
  CHECK(stats.invalidTimes == 1);
  CHECK(stats.fps() == 20.0);
  CHECK(stats.maxIntervalNs == 40'000'000ull);
  CHECK(stats.averageIntervalNs() == 30'000'000.0);
  CHECK(stats.elapsedNs() == 100'000'000ull);
  FrameTrace trace;
  trace.Add(10, 20, 1, Phase::Cpu);
  trace.Add(20, 90, 2, Phase::Cpu);
  CHECK(trace.Find(10)->frameId == 1);
  CHECK(trace.Find(19)->frameId == 1);
  CHECK(trace.Find(20)->frameId == 2);
  CHECK(trace.Find(90) == nullptr);
  CHECK(trace.Find(9) == nullptr);
  CHECK(!trace.IsSlow(15, 50));
  CHECK(trace.IsSlow(30, 50));
  for (std::uint64_t i = 0; i < FrameTrace::Capacity + 2; ++i)
    trace.Add(100 + i * 10, 110 + i * 10, i + 3, Phase::Cpu);
  CHECK(trace.size() == FrameTrace::Capacity);
  CHECK(trace.dropped() == 4);
}

void jitter_and_frames() {
  Jitter jitter(7);
  std::uint64_t total = 0;
  bool sawLow = false, sawHigh = false;
  for (int i = 0; i < 10000; ++i) {
    const auto interval = jitter.Next(5);
    CHECK(interval >= 4 && interval <= 6);
    total += interval;
    sawLow |= interval == 4;
    sawHigh |= interval == 6;
  }
  CHECK(sawLow && sawHigh);
  CHECK(total >= 49'000 && total <= 51'000);
  CHECK(jitter.Next(0) == 1);
  CHECK(jitter.Next(1) == 1);
  std::array<std::uint32_t, 16> words{};
  words[2] = 0x1018; words[3] = 0x2030;
  words[6] = 0; words[7] = 0x2050;
  const auto executable = [](std::uintptr_t a) { return a >= 0x2000 && a < 0x3000; };
  const auto chain = WalkCapturedFrames(0x1000, words.data(), sizeof(words), 0x1008, executable);
  CHECK(chain.count == 2);
  CHECK(chain.addresses[0] == 0x2030);
  CHECK(chain.addresses[1] == 0x2050);
  CHECK(chain.quality == ChainQuality::FrameChain);
  words[6] = 0x1008;
  CHECK(WalkCapturedFrames(0x1000, words.data(), sizeof(words), 0x1008, executable).quality == ChainQuality::Invalid);
  CHECK(WalkCapturedFrames(0x1000, words.data(), sizeof(words), 0x9000, executable).count == 0);
  words[3] = 0x100000;
  CHECK(WalkCapturedFrames(0x1000, words.data(), sizeof(words), 0x1008, executable).count == 0);
}
int main() {
  phases(); failures(); frame_math(); jitter_and_frames();
  return api_test::finish();
}
#else
int main() {
  CHECK(false && "production measurement controller is missing");
  return api_test::finish();
}
#endif
