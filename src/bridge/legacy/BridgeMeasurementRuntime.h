#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d9.h>
#include <string>
#include <vector>
#include "BridgeMeasurement.h"

namespace BridgeMeasurement {

struct RuntimeCpuAddress { std::uintptr_t address = 0; std::uint64_t samples = 0; };
struct RuntimeCpuStack {
  std::uintptr_t ip = 0;
  std::array<std::uintptr_t, 3> callers{};
  std::uint64_t samples = 0;
  ChainQuality quality = ChainQuality::Unavailable;
};
struct RuntimeCpuModule {
  std::uintptr_t base = 0;
  std::uint32_t size = 0, timestamp = 0;
  std::string path;
};
struct RuntimeCpuReport {
  std::uint64_t captureId = 0, elapsedMs = 0, presents = 0, samples = 0, missed = 0;
  double fps = 0;
  std::vector<RuntimeCpuAddress> addresses;
  std::vector<RuntimeCpuStack> stacks;
  std::vector<RuntimeCpuModule> modules;
};

struct RuntimeConfig {
  bool enabled = false, hud = true, journal = true, cpu = true;
  bool telemetry = true, csCpu = true, context = true;
  Options timing{};
  std::uint32_t intervalMs = 5, csIntervalMs = 10;
  const char* gameDirectory = nullptr;
  HANDLE stopEvent = nullptr;
  // The host owns returned thread handles and must join them before unload or
  // reconfiguration. Runtime never closes them or waits under its state lock.
  HANDLE (*startWorker)(LPTHREAD_START_ROUTINE, void*) = nullptr;
  bool (*keyDown)(int) = nullptr;
  void (*log)(const char*) = nullptr;
  bool (*readFrame)(std::uint32_t&) = nullptr;
  void (*appendCpuReport)(const char*, const RuntimeCpuReport&) = nullptr;
};

inline constexpr std::uint32_t RuntimeCpuUnavailable = 2;
inline constexpr std::uint32_t RuntimeAttributionUnavailable = 4;
inline constexpr std::uint32_t RuntimeBackendUnavailable = 8;
inline constexpr std::uint32_t RuntimeJournalUnavailable = 16;
inline constexpr std::uint32_t RuntimeTelemetryUnavailable = 32;
inline constexpr std::uint32_t RuntimeCsCpuUnavailable = 64;
inline constexpr std::uint32_t RuntimeContextUnavailable = 128;
struct RuntimeSnapshot {
  Phase phase = Phase::Ready;
  std::uint64_t captureId = 0;
  std::uint32_t unavailableMask = 0;
  FrameStats phaseStats[3]{};
  std::uint64_t cpuSamples = 0, cpuMissed = 0, cpuDropped = 0;
  std::uint64_t suspendedNs = 0, frameTraceDropped = 0;
  std::uint64_t csSamples = 0, csMissed = 0, csDropped = 0, csSuspendedNs = 0;
  std::uint64_t mainThreadCycles = 0, csThreadCycles = 0;
  // Before report completion, events counts full binary records already spooled;
  // queued records are pending, not retained or dropped. After report completion,
  // events counts records in the finalized report. Dropped then includes unread
  // or unfinalized binary records, in addition to backend and storage omissions.
  std::uint64_t telemetryEvents = 0, telemetryFiltered = 0, telemetryDropped = 0;
  std::uint64_t phaseMainCpuNs[3]{}, phaseCsCpuNs[3]{}, memorySnapshots = 0;
  std::uint32_t threadActivityMask = 0;
  bool workerBusy = false;
  bool hudRequested = false, hudIntegrationAvailable = false;
  char reportPath[MAX_PATH]{}, lastError[128]{};
  // Appended private snapshot fields preserve the earlier fixture layout.
  std::uint64_t telemetryUnread = 0;
  bool telemetryReportFinished = false;
  std::uint32_t telemetryStorageErrors = 0;
};

void ConfigureRuntime(const RuntimeConfig&);
bool RuntimeEnabled() noexcept;
// Acquire combined audit ownership before the backend can poll a first F7.
// The first real Present selects the capture thread, not device creation.
void RuntimePrepareDevice(IDirect3DDevice9* rawDevice);
void RuntimePresent(IDirect3DDevice9* rawDevice, HRESULT, HWND windowOverride = nullptr);
void RuntimeInterrupt(IDirect3DDevice9* rawDevice, const char* reason, bool releaseDevice = false);
void RuntimeShutdown();
bool RuntimeGetSnapshot(RuntimeSnapshot*);

} // namespace BridgeMeasurement
