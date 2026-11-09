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
  Options timing{};
  std::uint32_t intervalMs = 5;
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
struct RuntimeSnapshot {
  Phase phase = Phase::Ready;
  std::uint64_t captureId = 0;
  std::uint32_t unavailableMask = 0;
  FrameStats phaseStats[3]{};
  std::uint64_t cpuSamples = 0, cpuMissed = 0, cpuDropped = 0;
  std::uint64_t suspendedNs = 0, frameTraceDropped = 0;
  bool workerBusy = false;
  bool hudRequested = false, hudIntegrationAvailable = false;
  char reportPath[MAX_PATH]{}, lastError[128]{};
};

void ConfigureRuntime(const RuntimeConfig&);
bool RuntimeEnabled() noexcept;
// Acquire combined audit ownership before the backend can poll a first F7.
// The first real Present selects the capture thread, not device creation.
void RuntimePrepareDevice(IDirect3DDevice9* rawDevice);
void RuntimePresent(IDirect3DDevice9* rawDevice, HRESULT);
void RuntimeInterrupt(IDirect3DDevice9* rawDevice, const char* reason, bool releaseDevice = false);
void RuntimeShutdown();
bool RuntimeGetSnapshot(RuntimeSnapshot*);

} // namespace BridgeMeasurement
