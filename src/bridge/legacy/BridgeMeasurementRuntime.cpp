#include "BridgeMeasurementRuntime.h"
#include "MeasurementSymbols.h"
#include "ProperShadersStateJournal.h"
#include "../../../backend/dxvk/src/d3d9/d3d9_bridge_diagnostics.h"
#include "../../../backend/dxvk/src/d3d9/d3d9_bridge_thread_stats.h"
#include "../../../backend/dxvk/src/d3d9/d3d9_bridge_telemetry.h"

#include <tlhelp32.h>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include "MeasurementEventStorage.h"

namespace BridgeMeasurement {
namespace {
using detail::MeasurementEventStorage;
constexpr std::size_t MaxSamples = 16384;
constexpr std::size_t StackBytes = 4096;
constexpr std::size_t MaxTelemetryEvents = 65536, MaxMemorySnapshots = 128;
constexpr UINT MemoryChunkCapacity = 256, MemoryMaskCapacity = 4096;
constexpr std::uint64_t SnapshotCadenceNs = 500'000'000ull;
struct Range { std::uintptr_t begin, end; };
struct RawSample {
  std::uint64_t timeNs = 0, suspendedNs = 0;
  std::uintptr_t ip = 0;
  CapturedChain chain;
  std::array<std::uintptr_t, 3> candidates{};
};
struct CpuFunction {
  SymbolResult symbol;
  std::string name;
  std::uint64_t samples = 0;
};
struct CpuResult {
  std::uint64_t id = 0, elapsedNs = 0, missed = 0, dropped = 0, suspendedNs = 0;
  std::uint64_t maxSuspendedNs = 0, cycles = 0, presents = 0, startedNs = 0;
  std::uint64_t scheduleMissed = 0, maxScheduleLatenessNs = 0;
  std::uint64_t validChains = 0, partialChains = 0, invalidChains = 0;
  std::vector<RawSample> samples;
  std::vector<RuntimeCpuModule> modules;
  std::vector<CpuFunction> functions;
  std::unordered_map<std::uintptr_t, SymbolResult> symbols;
  std::string error;
  const char* csError = "not-initialized";
  DWORD thread = 0;
  bool available = false;
  std::unique_ptr<CpuResult> cs;
};
struct CpuJob {
  RuntimeConfig config;
  std::string directory;
  DWORD thread = 0, csThread = 0;
  std::uint64_t id = 0;
  std::atomic<bool> cancel{false};
};
struct ContextPoint {
  std::uint64_t timeNs = 0, presents = 0;
  std::uint64_t frameUnchangedNs = 0;
  std::uint32_t frame = 0;
  bool windowKnown = false, foregroundKnown = false, foreground = false, minimized = false;
  bool frameKnown = false, unchanged = false, presentGap = false;
  bool presentsAdvanced = false;
};
struct ContextJob {
  RuntimeConfig config;
  std::uint64_t id = 0;
  std::atomic<HWND> window{nullptr};
  std::atomic<bool> cancel{false}, done{false}, failed{false};
  std::mutex mutex;
  std::vector<ContextPoint> points;
  std::uint64_t dropped = 0;
};
struct MemoryObservation {
  D3D9BridgeMemorySnapshot snapshot{};
  std::vector<D3D9BridgeMemoryChunk> chunks;
  std::vector<UINT> masks;
  const char* boundary = "periodic";
};
struct TelemetryCapture {
  bool available = false, active = false;
  const char* error = "";
  std::uint64_t id = 0, lastSnapshotNs = 0, runtimeDropped = 0, invalidEvents = 0, snapshotSkipped = 0, snapshotFailures = 0;
  D3D9BridgeTelemetryRead read{};
  std::shared_ptr<MeasurementEventStorage> storage;
  std::vector<MemoryObservation> memory;
};
struct ReportJob {
  RuntimeConfig config;
  std::string path;
  RuntimeSnapshot snapshot;
  FrameTrace trace;
  JournalAttributionSummary journal;
  D3D9BridgeAuditSnapshot audit{};
  std::array<D3D9BridgeThreadSnapshot, 3> threadBegin{}, threadEnd{};
  std::shared_ptr<CpuResult> cpu;
  std::shared_ptr<ContextJob> context;
  std::shared_ptr<TelemetryCapture> telemetry;
  Phase finalPhase = Phase::Complete;
};

std::mutex g_mutex;
std::atomic<bool> g_enabled{false};
std::atomic<unsigned> g_workers{0};
struct WorkerExit { ~WorkerExit() { g_workers.fetch_sub(1, std::memory_order_release); } };
std::atomic<std::uint64_t> g_successfulPresents{0};
RuntimeConfig g_config;
std::string g_directory;
Controller g_controller;
IDirect3DDevice9* g_device = nullptr; // borrowed; cleared before final release
ID3D9BridgeDiagnostics* g_diagnostics = nullptr; // likewise never retains device
ID3D9BridgeThreadStats* g_threadStats = nullptr; // borrowed, same device lifetime
ID3D9BridgeTelemetry* g_telemetry = nullptr; // borrowed only under device callback/lifecycle
HWND g_deviceWindow = nullptr;
std::array<D3D9BridgeThreadSnapshot, 3> g_threadBegin{}, g_threadEnd{};
int g_threadPhase = -1;
DWORD g_owner = 0;
std::shared_ptr<CpuJob> g_cpuJob;
std::shared_ptr<CpuResult> g_cpuResult;
std::shared_ptr<ContextJob> g_context;
std::shared_ptr<TelemetryCapture> g_telemetryCapture;
bool g_cpuReady = false, g_journalStarted = false, g_auditStarted = false;
bool g_reportFailed = false, g_reportStarted = false, g_reportInterrupted = false;
bool g_journalDrainPending = false;
std::uint64_t g_nextCapture = 0, g_capture = 0, g_lastHudNs = 0;
std::uint64_t g_auditCapture = 0;
std::uint32_t g_unavailable = 0;
FrameStats g_live;
bool g_liveStarted = false;
double g_liveFps = 0, g_liveMeanMs = 0, g_liveMaxMs = 0;
JournalAttributionSummary g_journal;
D3D9BridgeAuditSnapshot g_audit{};
char g_reportPath[MAX_PATH]{}, g_error[128]{};

std::uint64_t NowNs() {
  static const auto frequency = [] { LARGE_INTEGER value{}; QueryPerformanceFrequency(&value); return value.QuadPart; }();
  LARGE_INTEGER ticks{};
  QueryPerformanceCounter(&ticks);
  if (frequency <= 0 || ticks.QuadPart < 0) return 0;
  const auto t = std::uint64_t(ticks.QuadPart), f = std::uint64_t(frequency);
  return (t / f) * 1'000'000'000ull + ((t % f) * 1'000'000'000ull) / f;
}
DWORD WINAPI ContextWorker(void* opaque) {
  WorkerExit activity;
  std::unique_ptr<std::shared_ptr<ContextJob>> holder(static_cast<std::shared_ptr<ContextJob>*>(opaque));
  const auto job = *holder;
  struct Done { std::shared_ptr<ContextJob> job; ~Done() { job->done.store(true, std::memory_order_release); } } done{job};
  try {
    std::uint32_t lastFrame = 0;
    bool haveFrame = false;
    auto lastPresents = g_successfulPresents.load(std::memory_order_relaxed);
    auto lastPresentNs = NowNs();
    auto frameChangedNs = lastPresentNs, presentsAtFrameChange = lastPresents;
    while (!job->cancel.load(std::memory_order_relaxed) &&
        !(job->config.stopEvent && WaitForSingleObject(job->config.stopEvent, 0) == WAIT_OBJECT_0)) {
      ContextPoint point;
      point.timeNs = NowNs(); point.presents = g_successfulPresents.load(std::memory_order_relaxed);
      if (point.presents != lastPresents) { lastPresents = point.presents; lastPresentNs = point.timeNs; }
      point.presentGap = point.timeNs >= lastPresentNs && point.timeNs - lastPresentNs >= 250'000'000ull;
      const auto window = job->window.load(std::memory_order_relaxed);
      DWORD pid = 0;
      if (window && IsWindow(window) && GetWindowThreadProcessId(window, &pid) && pid == GetCurrentProcessId()) {
        point.windowKnown = true;
        point.minimized = IsIconic(window) != FALSE;
        const HWND foreground = GetForegroundWindow();
        point.foregroundKnown = foreground != nullptr;
        point.foreground = foreground && GetAncestor(foreground, GA_ROOTOWNER) == GetAncestor(window, GA_ROOTOWNER);
      }
      if (job->config.readFrame) point.frameKnown = job->config.readFrame(point.frame);
      point.unchanged = point.frameKnown && haveFrame && point.frame == lastFrame;
      if (point.frameKnown) {
        if (!point.unchanged) { frameChangedNs = point.timeNs; presentsAtFrameChange = point.presents; }
        point.frameUnchangedNs = point.timeNs >= frameChangedNs ? point.timeNs - frameChangedNs : 0;
        point.presentsAdvanced = point.presents > presentsAtFrameChange;
        lastFrame = point.frame; haveFrame = true;
      } else haveFrame = false;
      {
        std::lock_guard<std::mutex> lock(job->mutex);
        if (job->points.size() < 4096) job->points.push_back(point);
        else ++job->dropped;
      }
      if (job->config.stopEvent) {
        if (WaitForSingleObject(job->config.stopEvent, 25) != WAIT_TIMEOUT) break;
      } else Sleep(25);
    }
  } catch (...) { job->failed.store(true, std::memory_order_release); }
  return 0;
}
void CopyText(char* out, std::size_t size, const std::string& text) {
  if (!size) return;
  const auto count = (std::min)(size - 1, text.size());
  std::memcpy(out, text.data(), count); out[count] = 0;
}
void Log(const RuntimeConfig& config, const std::string& text) {
  if (config.log) config.log(text.c_str());
}
std::string Quote(const std::string& value) {
  std::string result = "\"";
  for (char c : value) {
    if (c == '\\' || c == '"') result += '\\';
    if (c == '\r') result += "\\r";
    else if (c == '\n') result += "\\n";
    else result += c;
  }
  return result + '"';
}
std::string Basename(const std::string& path) {
  const auto slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}
std::string Lower(std::string text) {
  for (auto& c : text) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return text;
}
const RuntimeCpuModule* FindModule(const std::vector<RuntimeCpuModule>& modules, std::uintptr_t ip) {
  for (const auto& module : modules)
    if (ip >= module.base && ip - module.base < module.size) return &module;
  return nullptr;
}
struct PeCopy {
  DWORD timestamp = 0;
  unsigned count = 0;
  IMAGE_SECTION_HEADER sections[96]{};
};
bool CopyPe(std::uintptr_t base, PeCopy* copy) {
  __try {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew > 0x100000) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.NumberOfSections > 96) return false;
    copy->timestamp = nt->FileHeader.TimeDateStamp;
    copy->count = nt->FileHeader.NumberOfSections;
    std::memcpy(copy->sections, IMAGE_FIRST_SECTION(nt), copy->count * sizeof(IMAGE_SECTION_HEADER));
    return true;
  } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void CollectModules(std::vector<RuntimeCpuModule>& modules, std::vector<Range>& ranges) {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
  if (snapshot == INVALID_HANDLE_VALUE) return;
  MODULEENTRY32W entry{}; entry.dwSize = sizeof(entry);
  if (Module32FirstW(snapshot, &entry)) do {
    RuntimeCpuModule module;
    module.base = reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
    module.size = entry.modBaseSize;
    // Reports and MeasurementSymbolResolver use UTF-8 regardless of Windows ACP.
    const int pathLength = lstrlenW(entry.szExePath);
    const int utf8Length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
      entry.szExePath, pathLength, nullptr, 0, nullptr, nullptr);
    module.path.resize(utf8Length);
    if (utf8Length) WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
      entry.szExePath, pathLength, module.path.data(), utf8Length, nullptr, nullptr);
    PeCopy pe{};
    if (CopyPe(module.base, &pe)) {
      module.timestamp = pe.timestamp;
      for (unsigned i = 0; i < pe.count; ++i) {
        const auto& section = pe.sections[i];
        const auto size = (std::max)(section.Misc.VirtualSize, section.SizeOfRawData);
        if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) && section.VirtualAddress < module.size)
          ranges.push_back({module.base + section.VirtualAddress,
            module.base + section.VirtualAddress + (std::min)(size, module.size - section.VirtualAddress)});
      }
    }
    modules.push_back(std::move(module));
  } while (Module32NextW(snapshot, &entry));
  CloseHandle(snapshot);
  std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) { return a.begin < b.begin; });
}
bool Executable(const std::vector<Range>& ranges, std::uintptr_t address) {
  auto it = std::upper_bound(ranges.begin(), ranges.end(), address,
    [](std::uintptr_t value, const Range& range) { return value < range.begin; });
  return it != ranges.begin() && address < (--it)->end;
}
std::size_t CopyStack(std::uintptr_t pointer, void* bytes) {
  MEMORY_BASIC_INFORMATION info{};
  if (!pointer || !VirtualQuery(reinterpret_cast<void*>(pointer), &info, sizeof(info)) ||
      info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
  const auto base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
  if (pointer < base || pointer - base >= info.RegionSize) return 0;
  const auto size = (std::min<std::size_t>)(StackBytes, info.RegionSize - (pointer - base));
  __try { std::memcpy(bytes, reinterpret_cast<const void*>(pointer), size); return size; }
  __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
bool Stopping(const CpuJob& job) {
  return job.cancel.load(std::memory_order_relaxed) ||
    (job.config.stopEvent && WaitForSingleObject(job.config.stopEvent, 0) == WAIT_OBJECT_0);
}
void DrainTelemetryLocked(bool finalDrain) {
  if (!g_telemetryCapture || !g_telemetryCapture->available || !g_telemetry) return;
  auto& capture = *g_telemetryCapture;
  try {
    std::array<D3D9BridgeTelemetryEvent, 256> buffer{};
    for (unsigned batch = 0; batch < (finalDrain ? 257u : 4u); ++batch) {
      D3D9BridgeTelemetryRead read{};
      read.StructSize = sizeof(read); read.Version = D3D9_BRIDGE_TELEMETRY_VERSION;
      if (FAILED(g_telemetry->ReadEvents(capture.id, &read, buffer.data(), UINT(buffer.size()))) ||
          read.CaptureId != capture.id || read.Written > buffer.size()) {
        capture.error = "read-events-failed"; g_unavailable |= RuntimeTelemetryUnavailable; break;
      }
      capture.read = read;
      for (UINT i = 0; i < read.Written; ++i) {
        if (buffer[i].CaptureId != capture.id) { ++capture.invalidEvents; g_unavailable |= RuntimeTelemetryUnavailable; }
        else if (capture.storage) capture.storage->Push(buffer[i]);
        else ++capture.runtimeDropped;
      }
      if (!read.Remaining || !read.Written) break;
    }
  } catch (...) { capture.error = "read-events-exception"; g_unavailable |= RuntimeTelemetryUnavailable; }
}
void SnapshotMemoryLocked(const char* boundary, std::uint64_t now) {
  if (!g_telemetryCapture || !g_telemetryCapture->active || !g_telemetry) return;
  auto& capture = *g_telemetryCapture;
  capture.lastSnapshotNs = now;
  if (capture.memory.size() >= MaxMemorySnapshots) { ++capture.snapshotSkipped; return; }
  try {
    MemoryObservation observation;
    observation.boundary = boundary;
    observation.chunks.resize(MemoryChunkCapacity);
    observation.masks.resize(MemoryMaskCapacity);
    observation.snapshot.StructSize = sizeof(observation.snapshot);
    observation.snapshot.Version = D3D9_BRIDGE_TELEMETRY_VERSION;
    if (FAILED(g_telemetry->GetMemorySnapshot(capture.id, &observation.snapshot,
        observation.chunks.data(), UINT(observation.chunks.size()), observation.masks.data(), UINT(observation.masks.size()))) ||
        observation.snapshot.CaptureId != capture.id || observation.snapshot.CopiedChunks > observation.chunks.size() ||
        observation.snapshot.CopiedMasks > observation.masks.size()) {
      ++capture.snapshotFailures; g_unavailable |= RuntimeTelemetryUnavailable; return;
    }
    observation.chunks.resize(observation.snapshot.CopiedChunks);
    observation.masks.resize(observation.snapshot.CopiedMasks);
    capture.memory.push_back(std::move(observation));
  } catch (...) { ++capture.snapshotFailures; g_unavailable |= RuntimeTelemetryUnavailable; }
}
template<typename T>
bool Launch(const RuntimeConfig&, LPTHREAD_START_ROUTINE, const std::shared_ptr<T>&);
DWORD WINAPI EventStorageWorker(void* opaque) {
  WorkerExit activity;
  std::unique_ptr<std::shared_ptr<MeasurementEventStorage>> holder(static_cast<std::shared_ptr<MeasurementEventStorage>*>(opaque));
  (*holder)->Run();
  return 0;
}
void BeginTelemetryLocked(std::uint64_t now) {
  // Reserve bounded storage before StartCapture: allocation failure can never
  // leave an active backend recorder with no owning runtime capture.
  g_telemetryCapture.reset();
  try {
    g_telemetryCapture = std::make_shared<TelemetryCapture>();
    auto& capture = *g_telemetryCapture;
    capture.id = g_capture;
    if (!g_config.telemetry) { capture.error = "disabled"; return; }
    if (!g_telemetry) { capture.error = "interface-unavailable"; g_unavailable |= RuntimeTelemetryUnavailable; return; }
    capture.storage = std::make_shared<MeasurementEventStorage>();
    capture.storage->stop = g_config.stopEvent;
    capture.memory.reserve(MaxMemorySnapshots);
  } catch (...) {
    g_unavailable |= RuntimeTelemetryUnavailable;
    if (g_telemetryCapture) g_telemetryCapture->error = "storage-allocation-failed";
    CopyText(g_error, sizeof(g_error), "cannot allocate event storage");
    return;
  }
  auto& capture = *g_telemetryCapture;
  D3D9BridgeTelemetryConfig config{};
  config.StructSize = sizeof(config); config.Version = D3D9_BRIDGE_TELEMETRY_VERSION;
  config.CaptureId = g_capture; config.MaxEvents = UINT(MaxTelemetryEvents);
  config.MinDurationNs = 50'000; config.MinBytes = 65'536;
  try {
    if (FAILED(g_telemetry->StartCapture(&config))) {
      capture.error = "start-failed"; g_unavailable |= RuntimeTelemetryUnavailable; return;
    }
    capture.available = capture.active = true;
    capture.storage->ready.store(false, std::memory_order_release);
    if (!Launch(g_config, EventStorageWorker, capture.storage)) {
      capture.storage->errors.fetch_or(MeasurementEventStorage::Worker);
      capture.storage->ready = true;
      capture.storage->abort = true;
      capture.error = "storage-worker-failed"; g_unavailable |= RuntimeTelemetryUnavailable;
    }
    SnapshotMemoryLocked("begin", now);
  } catch (...) {
    capture.error = "start-exception"; g_unavailable |= RuntimeTelemetryUnavailable;
    try { g_telemetry->StopCapture(capture.id); } catch (...) { }
    capture.active = false;
  }
}
void PollTelemetryLocked(std::uint64_t now) {
  if (!g_controller.active() || !g_telemetryCapture || !g_telemetryCapture->active) return;
  DrainTelemetryLocked(false);
  const auto last = g_telemetryCapture->lastSnapshotNs;
  if (now >= last && now - last >= SnapshotCadenceNs) {
    SnapshotMemoryLocked("periodic", now);
    DrainTelemetryLocked(false);
  }
}
void EndTelemetryLocked(std::uint64_t now, const char* boundary) {
  if (!g_telemetryCapture || !g_telemetryCapture->active || !g_telemetry) return;
  // Snapshot errors are contained so StopCapture is attempted on every exit.
  SnapshotMemoryLocked(boundary, now);
  bool stopped = false;
  try { stopped = SUCCEEDED(g_telemetry->StopCapture(g_telemetryCapture->id)); } catch (...) { }
  if (!stopped) {
    g_telemetryCapture->error = "stop-failed"; g_unavailable |= RuntimeTelemetryUnavailable;
  }
  g_telemetryCapture->active = !stopped;
  DrainTelemetryLocked(true);
  const auto& read = g_telemetryCapture->read;
  // An in-flight span during capture is normal. After stop, however, its
  // missing completion and any backend loss make telemetry evidence partial.
  if (read.Dropped || read.LateDropped || read.InFlight || read.InFlightAtStop || read.Remaining ||
      g_telemetryCapture->runtimeDropped)
    g_unavailable |= RuntimeTelemetryUnavailable;
  if (g_telemetryCapture->storage) g_telemetryCapture->storage->sealed.store(true, std::memory_order_release);
}
void ResolveCpu(CpuResult& result) {
  MeasurementSymbolResolver resolver;
  for (const auto& module : result.modules) {
    const auto name = Lower(Basename(module.path));
    if (name == "d3d9.dll") resolver.LoadModule(module.base, module.path, module.path + ".rsym");
    else if (name == "ntdll.dll" || name == "kernelbase.dll" || name == "kernel32.dll" || name == "win32u.dll")
      resolver.LoadExports(module.base, module.path);
  }
  std::unordered_map<std::uintptr_t, std::uint64_t> ips;
  for (const auto& sample : result.samples) {
    ++ips[sample.ip];
    for (auto caller : sample.chain.addresses)
      if (caller && !result.symbols.count(caller)) result.symbols.emplace(caller, resolver.Resolve(caller));
    for (auto caller : sample.candidates)
      if (caller && !result.symbols.count(caller)) result.symbols.emplace(caller, resolver.Resolve(caller));
  }
  std::map<std::pair<std::uintptr_t, std::uintptr_t>, std::size_t> groups;
  for (const auto& [ip, count] : ips) {
    auto symbol = resolver.Resolve(ip);
    if (symbol.quality == SymbolQuality::ModuleNotFound) if (const auto* module = FindModule(result.modules, ip)) {
      symbol.modulePath = module->path; symbol.moduleBase = module->base;
      symbol.rva = std::uint32_t(ip - module->base); symbol.quality = SymbolQuality::Missing;
    }
    if (symbol.quality == SymbolQuality::ModuleNotFound) { symbol.modulePath = "private"; symbol.rva = std::uint32_t(ip); }
    result.symbols[ip] = symbol;
    const bool named = !symbol.name.empty() || !symbol.aliases.empty();
    const auto key = std::make_pair(symbol.moduleBase,
      named ? ip - symbol.offset : ip);
    auto [it, inserted] = groups.emplace(key, result.functions.size());
    if (inserted) {
      std::string name = symbol.name;
      for (const auto& alias : symbol.aliases) { if (!name.empty()) name += '|'; name += alias; }
      if (name.empty()) {
        char rva[32]{}; std::snprintf(rva, sizeof(rva), "+0x%08X", symbol.rva);
        name = Basename(symbol.modulePath) + rva;
      }
      result.functions.push_back({symbol, std::move(name), count});
    } else result.functions[it->second].samples += count;
  }
  std::sort(result.functions.begin(), result.functions.end(),
    [](const CpuFunction& a, const CpuFunction& b) { return a.samples > b.samples; });
}
void WriteCpuLog(const CpuJob& job, const CpuResult& result) {
  const auto path = (std::filesystem::path(job.directory) / "scripts/BridgeD3D9.cpuhotspots.log").string();
  std::filesystem::create_directories(std::filesystem::path(path).parent_path());
  FILE* file = nullptr;
  if (fopen_s(&file, path.c_str(), "a") || !file) throw std::runtime_error("cannot write CPU hotspot report");
  SYSTEMTIME utc{}; GetSystemTime(&utc);
  const double fps = result.elapsedNs ? double(result.presents) * 1e9 / double(result.elapsedNs) : 0;
  std::fprintf(file, "# capture=%llu begin=%04u-%02u-%02uT%02u:%02u:%02u.%03uZ targetThread=%lu durationMs=%u intervalMs=%u elapsedMs=%llu frames=%llu fps=%.2f presents=%llu presentRate=%.2f frameCounterValid=0 presentCounterValid=1 samples=%zu missed=%llu threadCycles=%llu source=unified suspendedNs=%llu dropped=%llu\n",
    result.id, utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds,
    job.thread, job.config.timing.cpuMs, job.config.intervalMs, result.elapsedNs / 1'000'000,
    result.presents, fps, result.presents, fps, result.samples.size(), result.missed,
    result.cycles, result.suspendedNs, result.dropped);
  RuntimeCpuReport view;
  view.captureId = result.id; view.elapsedMs = result.elapsedNs / 1'000'000;
  view.presents = result.presents; view.samples = result.samples.size(); view.missed = result.missed;
  view.fps = fps; view.modules = result.modules;
  std::map<std::uintptr_t, std::uint64_t> moduleCounts, ipCounts;
  for (const auto& sample : result.samples) {
    ++ipCounts[sample.ip];
    const auto* module = FindModule(result.modules, sample.ip);
    ++moduleCounts[module ? module->base : 0];
    if (sample.chain.quality == ChainQuality::FrameChain) {
      RuntimeCpuStack stack; stack.ip = sample.ip; stack.quality = sample.chain.quality; stack.samples = 1;
      for (unsigned i = 0; i < 3 && i < sample.chain.count; ++i) stack.callers[i] = sample.chain.addresses[i];
      view.stacks.push_back(stack);
    }
  }
  std::vector<std::pair<std::uintptr_t, std::uint64_t>> ordered(moduleCounts.begin(), moduleCounts.end());
  std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {return a.second > b.second;});
  unsigned rank = 0;
  for (const auto& [base, count] : ordered) {
    const auto* module = FindModule(result.modules, base);
    std::fprintf(file, "moduleRank=%u samples=%llu percent=%.2f module=%s base=%08llX path=%s\n",
      ++rank, count, result.samples.empty() ? 0.0 : 100.0 * double(count) / double(result.samples.size()),
      module ? Basename(module->path).c_str() : "private", std::uint64_t(base), Quote(module ? module->path : "private").c_str());
  }
  ordered.assign(ipCounts.begin(), ipCounts.end());
  std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {return a.second > b.second;});
  rank = 0;
  for (const auto& [ip, count] : ordered) {
    const auto* module = FindModule(result.modules, ip);
    const auto it = result.symbols.find(ip);
    std::fprintf(file, "rank=%u samples=%llu percent=%.2f address=%08llX module=%s base=%08llX rva=0x%08llX quality=%s\n",
      ++rank, count, result.samples.empty() ? 0.0 : 100.0 * double(count) / double(result.samples.size()), std::uint64_t(ip),
      module ? Basename(module->path).c_str() : "private", module ? std::uint64_t(module->base) : 0,
      module ? std::uint64_t(ip - module->base) : 0, it != result.symbols.end() ? SymbolQualityName(it->second.quality) : "missing");
    view.addresses.push_back({ip, count});
  }
  std::fclose(file);
  if (job.config.appendCpuReport) job.config.appendCpuReport(path.c_str(), view);
  file = nullptr;
  if (!fopen_s(&file, path.c_str(), "a") && file) {
    std::fprintf(file, "# capture=%llu end\n", result.id); std::fclose(file);
  }
}

DWORD WINAPI CpuWorker(void* opaque) {
  WorkerExit activity;
  std::unique_ptr<std::shared_ptr<CpuJob>> holder(static_cast<std::shared_ptr<CpuJob>*>(opaque));
  const auto job = *holder;
  std::shared_ptr<CpuResult> result;
  HANDLE mainThread = nullptr, csThread = nullptr;
  try {
    result = std::make_shared<CpuResult>(); result->id = job->id; result->thread = job->thread;
    result->samples.reserve(MaxSamples);
    if (!job->config.csCpu) result->csError = "disabled";
    else if (!job->csThread || job->csThread == job->thread || job->csThread == GetCurrentThreadId())
      result->csError = "missing-or-invalid-cs-thread";
    else try {
      result->cs = std::make_unique<CpuResult>();
      result->cs->id = job->id; result->cs->thread = job->csThread;
      result->cs->samples.reserve(MaxSamples);
      result->csError = "";
    } catch (...) { result->cs.reset(); result->csError = "cs-allocation-failed"; }
    std::vector<Range> ranges;
    CollectModules(result->modules, ranges);
    if (result->cs) try { result->cs->modules = result->modules; }
    catch (...) { result->cs.reset(); result->csError = "cs-allocation-failed"; }
    auto* cs = result->cs.get();
    mainThread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, job->thread);
    if (!mainThread || GetProcessIdOfThread(mainThread) != GetCurrentProcessId() || job->thread == GetCurrentThreadId())
      throw std::runtime_error("cannot open capture owner thread");
    result->available = true;
    if (cs) {
      csThread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, job->csThread);
      if (!csThread || GetProcessIdOfThread(csThread) != GetCurrentProcessId()) {
        if (csThread) CloseHandle(csThread);
        csThread = nullptr; result->csError = "cannot-open-cs-thread";
      } else cs->available = true;
    }
    ULONG64 mainBegin = 0, mainEnd = 0, csBegin = 0, csEnd = 0;
    QueryThreadCycleTime(mainThread, &mainBegin);
    if (csThread) QueryThreadCycleTime(csThread, &csBegin);
    const auto startPresents = g_successfulPresents.load(std::memory_order_relaxed);
    // Initialize the QPC frequency and all storage before either target is suspended.
    result->startedNs = NowNs();
    if (cs) cs->startedNs = result->startedNs;
    Jitter mainJitter(DWORD(job->id) ^ job->thread);
    Jitter csJitter(DWORD(job->id) ^ job->csThread ^ 0x85ebca6bu);
    std::array<std::uint32_t, StackBytes / 4> stack{};
    auto nextMain = result->startedNs + std::uint64_t(mainJitter.Next(job->config.intervalMs)) * 1'000'000ull;
    auto nextCs = result->startedNs + std::uint64_t(csJitter.Next(job->config.csIntervalMs)) * 1'000'000ull;
    auto sampleThread = [&](HANDLE thread, CpuResult& target) {
      const auto pauseStart = NowNs();
      if (SuspendThread(thread) == DWORD(-1)) { ++target.missed; return; }
      CONTEXT context{}; context.ContextFlags = CONTEXT_CONTROL;
      const bool got = GetThreadContext(thread, &context) != FALSE;
      RawSample sample; sample.timeNs = NowNs();
      std::uintptr_t sp = 0, bp = 0;
#if defined(_M_IX86)
      sample.ip = context.Eip; sp = context.Esp; bp = context.Ebp;
#elif defined(_M_X64)
      sample.ip = context.Rip; sp = context.Rsp; bp = context.Rbp;
#endif
      const auto copied = got ? CopyStack(sp, stack.data()) : 0;
      const auto resumed = ResumeThread(thread);
      const auto pauseEnd = NowNs();
      sample.suspendedNs = pauseEnd >= pauseStart ? pauseEnd - pauseStart : 0;
      target.suspendedNs += sample.suspendedNs;
      target.maxSuspendedNs = (std::max)(target.maxSuspendedNs, sample.suspendedNs);
      if (!got || !sample.ip || resumed == DWORD(-1)) { ++target.missed; return; }
      // The only suspension region is above. No parsing, allocation, logging,
      // device call or runtime lock occurs until the target has been resumed.
      sample.chain = WalkCapturedFrames(sp, stack.data(), copied, bp,
        [&](std::uintptr_t address) { return Executable(ranges, address); });
      unsigned candidates = 0;
      for (unsigned i = 0; i < copied / 4 && i < 64 && candidates < 3; ++i) {
        const auto address = std::uintptr_t(stack[i]);
        if (address != sample.ip && Executable(ranges, address)) sample.candidates[candidates++] = address;
      }
      if (sample.chain.quality == ChainQuality::FrameChain) ++target.validChains;
      else if (sample.chain.quality == ChainQuality::Partial || sample.chain.quality == ChainQuality::Truncated) ++target.partialChains;
      else ++target.invalidChains;
      if (target.samples.size() < MaxSamples) target.samples.push_back(sample);
      else ++target.dropped;
    };
    const auto deadline = result->startedNs + std::uint64_t(job->config.timing.cpuMs) * 1'000'000ull;
    while (!Stopping(*job)) {
      const auto now = NowNs();
      if (now >= deadline) break;
      if (now >= nextMain) {
        const auto late = now - nextMain;
        result->maxScheduleLatenessNs = (std::max)(result->maxScheduleLatenessNs, late);
        result->scheduleMissed += late / (std::uint64_t((std::max)(1u, job->config.intervalMs)) * 1'000'000ull);
        sampleThread(mainThread, *result);
        nextMain = NowNs() + std::uint64_t(mainJitter.Next(job->config.intervalMs)) * 1'000'000ull;
      }
      if (csThread && !Stopping(*job) && NowNs() >= nextCs) {
        const auto late = NowNs() - nextCs;
        cs->maxScheduleLatenessNs = (std::max)(cs->maxScheduleLatenessNs, late);
        cs->scheduleMissed += late / (std::uint64_t((std::max)(1u, job->config.csIntervalMs)) * 1'000'000ull);
        sampleThread(csThread, *cs);
        nextCs = NowNs() + std::uint64_t(csJitter.Next(job->config.csIntervalMs)) * 1'000'000ull;
      }
      const auto after = NowNs();
      const auto due = csThread ? (std::min)(nextMain, nextCs) : nextMain;
      const DWORD delay = due > after ? DWORD((std::min<std::uint64_t>)(10, (due - after + 999'999ull) / 1'000'000ull)) : 0;
      if (job->config.stopEvent) {
        if (WaitForSingleObject(job->config.stopEvent, delay) != WAIT_TIMEOUT) break;
      } else Sleep(delay);
    }
    result->elapsedNs = NowNs() - result->startedNs;
    if (cs) cs->elapsedNs = result->elapsedNs;
    result->presents = g_successfulPresents.load(std::memory_order_relaxed) - startPresents;
    if (cs) cs->presents = result->presents;
    QueryThreadCycleTime(mainThread, &mainEnd);
    result->cycles = mainEnd >= mainBegin ? mainEnd - mainBegin : 0;
    if (csThread) {
      QueryThreadCycleTime(csThread, &csEnd);
      cs->cycles = csEnd >= csBegin ? csEnd - csBegin : 0;
    }
    CloseHandle(mainThread); mainThread = nullptr;
    if (csThread) { CloseHandle(csThread); csThread = nullptr; }
    if (Stopping(*job)) throw std::runtime_error("CPU capture interrupted");
    if (result->samples.empty()) throw std::runtime_error("no valid CPU samples");
    if (cs && cs->available && cs->samples.empty()) { cs->available = false; result->csError = "no-valid-cs-samples"; }
    ResolveCpu(*result);
    if (Stopping(*job)) throw std::runtime_error("CPU capture interrupted");
    WriteCpuLog(*job, *result);
    if (Stopping(*job)) throw std::runtime_error("CPU capture interrupted");
    if (cs && cs->available) try { ResolveCpu(*cs); }
    catch (...) { cs->available = false; result->csError = "cs-symbol-resolution-failed"; }
  } catch (const std::exception& e) { if (result) result->error = e.what(); }
    catch (...) { if (result) result->error = "CPU capture failed"; }
  if (mainThread) CloseHandle(mainThread);
  if (csThread) CloseHandle(csThread);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_cpuJob == job && g_capture == job->id) {
      g_cpuResult = result;
      g_cpuReady = true;
      if (job->config.csCpu && (!result || !result->cs || !result->cs->available)) g_unavailable |= RuntimeCsCpuUnavailable;
      if (!result) CopyText(g_error, sizeof(g_error), "CPU result allocation failed");
      else if (!result->error.empty() && g_controller.active()) CopyText(g_error, sizeof(g_error), result->error);
    }
  }
  return 0;
}

const char* AuditName(unsigned i) {
  static constexpr const char* names[] = {"renderState", "samplerState", "textureStageState", "texture",
    "vertexShader", "pixelShader", "vertexFloatConstants", "vertexIntConstants", "vertexBoolConstants",
    "pixelFloatConstants", "pixelIntConstants", "pixelBoolConstants", "stateBlockCreate",
    "stateBlockBeginRecord", "stateBlockEndRecord", "stateBlockCapture", "stateBlockApply"};
  return i < std::size(names) ? names[i] : "unknown";
}
const char* TelemetryKindName(UINT kind) {
  switch (kind) {
    case D3D9_BRIDGE_TELEMETRY_ALLOC: return "alloc";
    case D3D9_BRIDGE_TELEMETRY_FREE: return "free";
    case D3D9_BRIDGE_TELEMETRY_MAP: return "map";
    case D3D9_BRIDGE_TELEMETRY_UNMAP: return "unmap";
    case D3D9_BRIDGE_TELEMETRY_CS_WAIT: return "cs-wait";
    case D3D9_BRIDGE_TELEMETRY_RESOURCE_WAIT: return "resource-wait";
    case D3D9_BRIDGE_TELEMETRY_GPU_WAIT: return "gpu-wait";
    case D3D9_BRIDGE_TELEMETRY_SUBMISSION_WAIT: return "submission-wait";
    case D3D9_BRIDGE_TELEMETRY_FRAME_LATENCY_WAIT: return "frame-latency-wait";
    case D3D9_BRIDGE_TELEMETRY_PRESENT_ACQUIRE: return "present-acquire";
    case D3D9_BRIDGE_TELEMETRY_SAMPLER_POOL_WAIT: return "sampler-pool-wait";
    case D3D9_BRIDGE_TELEMETRY_MAPPING_RETIRE: return "mapping-retire";
    case D3D9_BRIDGE_TELEMETRY_CS_CHUNK: return "cs-chunk";
    case D3D9_BRIDGE_TELEMETRY_COMMAND_ALLOCATE: return "command-allocate";
    case D3D9_BRIDGE_TELEMETRY_COMMAND_BEGIN: return "command-begin";
    case D3D9_BRIDGE_TELEMETRY_COMMAND_RESET: return "command-reset";
    default: return "unknown";
  }
}
void FillSnapshotLocked(RuntimeSnapshot& snapshot) {
  snapshot = {};
  snapshot.phase = g_reportFailed ? Phase::Error : g_reportInterrupted ? Phase::Interrupted : g_controller.phase();
  if (g_journalDrainPending && snapshot.phase == Phase::Complete) snapshot.phase = Phase::AttributionFinishing;
  snapshot.captureId = g_capture;
  snapshot.unavailableMask = g_unavailable | g_controller.unavailableMask();
  for (unsigned i = 0; i < 3; ++i) {
    snapshot.phaseStats[i] = g_controller.stats(i);
    const auto& a = g_threadBegin[i]; const auto& b = g_threadEnd[i];
    if (a.AvailableMask == 7 && b.AvailableMask == 7 && a.MainThreadId == b.MainThreadId &&
        a.CsThreadId == b.CsThreadId && b.MainCpu100ns >= a.MainCpu100ns && b.CsCpu100ns >= a.CsCpu100ns) {
      snapshot.threadActivityMask |= 1u << i;
      snapshot.phaseMainCpuNs[i] = (b.MainCpu100ns - a.MainCpu100ns) * 100;
      snapshot.phaseCsCpuNs[i] = (b.CsCpu100ns - a.CsCpu100ns) * 100;
    }
  }
  snapshot.frameTraceDropped = g_controller.trace().dropped();
  snapshot.workerBusy = g_workers.load(std::memory_order_acquire) != 0 || g_journalDrainPending;
  snapshot.hudRequested = g_config.hud;
  snapshot.hudIntegrationAvailable = g_diagnostics != nullptr;
  if (g_cpuResult) {
    snapshot.cpuSamples = g_cpuResult->samples.size(); snapshot.cpuMissed = g_cpuResult->missed;
    snapshot.cpuDropped = g_cpuResult->dropped; snapshot.suspendedNs = g_cpuResult->suspendedNs;
    snapshot.mainThreadCycles = g_cpuResult->cycles;
    if (g_cpuResult->cs) {
      snapshot.csSamples = g_cpuResult->cs->samples.size(); snapshot.csMissed = g_cpuResult->cs->missed;
      snapshot.csDropped = g_cpuResult->cs->dropped; snapshot.csSuspendedNs = g_cpuResult->cs->suspendedNs;
      snapshot.csThreadCycles = g_cpuResult->cs->cycles;
    }
  }
  if (g_telemetryCapture) {
    snapshot.telemetryFiltered = g_telemetryCapture->read.Filtered;
    snapshot.telemetryDropped = g_telemetryCapture->read.Dropped + g_telemetryCapture->runtimeDropped;
    if (g_telemetryCapture->storage) {
      const auto& storage = *g_telemetryCapture->storage;
      snapshot.telemetryReportFinished = storage.reportFinished.load(std::memory_order_acquire);
      snapshot.telemetryStorageErrors = storage.errors.load();
      const auto written = storage.written.load();
      snapshot.telemetryEvents = snapshot.telemetryReportFinished ? storage.finalizedReported.load() : written;
      snapshot.telemetryUnread = snapshot.telemetryReportFinished ? written - snapshot.telemetryEvents : 0;
      snapshot.telemetryDropped += storage.omitted.load() + snapshot.telemetryUnread;
    }
    if (g_telemetryCapture->storage && g_telemetryCapture->storage->errors.load()) snapshot.unavailableMask |= RuntimeTelemetryUnavailable;
    snapshot.memorySnapshots = g_telemetryCapture->memory.size();
  }
  CopyText(snapshot.reportPath, sizeof(snapshot.reportPath), g_reportPath);
  CopyText(snapshot.lastError, sizeof(snapshot.lastError), g_error);
}
void ReadAuditLocked() {
  if (!g_diagnostics) return;
  D3D9BridgeAuditSnapshot result{};
  result.StructSize = sizeof(result); result.Version = D3D9_BRIDGE_DIAGNOSTICS_VERSION;
  if (SUCCEEDED(g_diagnostics->GetAudit(&result))) g_audit = result;
}
void SyncThreadPhaseLocked() {
  const auto phase = g_controller.phase();
  const int next = phase == Phase::Baseline ? 0 : phase == Phase::Cpu ? 1 : phase == Phase::Attribution ? 2 : -1;
  if (next == g_threadPhase) return;
  D3D9BridgeThreadSnapshot sample{};
  sample.StructSize = sizeof(sample); sample.Version = 1;
  if (g_threadStats && FAILED(g_threadStats->GetThreadStats(&sample))) sample = {};
  if (g_threadPhase >= 0) g_threadEnd[unsigned(g_threadPhase)] = sample;
  if (next >= 0) g_threadBegin[unsigned(next)] = sample;
  g_threadPhase = next;
}
void StopAuditSafelyLocked() {
  if (!g_diagnostics || !g_auditStarted) return;
  bool stopped = false;
  // A thrown COM call may have stopped the audit before throwing. One retry
  // permits an idempotent S_FALSE result and never blocks recorder cleanup.
  for (unsigned attempt = 0; attempt < 2 && !stopped; ++attempt) {
    try { stopped = SUCCEEDED(g_diagnostics->StopAudit(g_auditCapture)); }
    catch (...) { g_unavailable |= RuntimeBackendUnavailable; }
  }
  if (!stopped) g_unavailable |= RuntimeBackendUnavailable;
  else { g_auditStarted = false; g_auditCapture = 0; }
  try { ReadAuditLocked(); } catch (...) { g_unavailable |= RuntimeBackendUnavailable; }
}
void StopAttributionLocked() {
  if (g_journalStarted) {
    // Present/Reset can already hold the device lock or be inside an admitted
    // transaction. Never drain it synchronously from that calling thread.
    try {
      JournalAttributionCloseAdmission();
      if (!JournalAttributionGetSummary(&g_journal)) {
        g_unavailable |= RuntimeJournalUnavailable; g_journalDrainPending = true;
      } else g_journalDrainPending = g_journal.inFlight != 0;
    } catch (...) {
      g_unavailable |= RuntimeJournalUnavailable; g_journalDrainPending = true;
    }
    g_journalStarted = false;
    if (!g_journalDrainPending && !g_journal.available) g_unavailable |= RuntimeJournalUnavailable;
  }
  StopAuditSafelyLocked();
}
void WriteCombinedReport(ReportJob& job) {
  std::filesystem::create_directories(std::filesystem::path(job.path).parent_path());
  FILE* file = nullptr;
  if (fopen_s(&file, job.path.c_str(), "w") || !file) throw std::runtime_error("cannot write combined report");
  struct FileClose { FILE* file; ~FileClose() { std::fclose(file); } } close{file};
  const auto& s = job.snapshot;
  const auto writeHeader = [&](const char* status) {
    // Fixed-width fields permit finalizing storage read errors after bounded
    // report iteration, without buffering the report or claiming success early.
    std::fprintf(file, "# measurement captureId=%llu status=%-16s pid=%lu clock=qpc-nanoseconds unavailableMask=%010u\n",
      s.captureId, status, GetCurrentProcessId(), s.unavailableMask);
  };
  writeHeader("pending");
  std::fprintf(file, "hud requested=%u integrationAvailable=%u baselineKeepsSameHudConfiguration=1\n", unsigned(s.hudRequested), unsigned(s.hudIntegrationAvailable));
  std::fprintf(file, "limitations=sampling-is-statistical;frame-chain-is-structural;candidate-callers-are-heuristic;inclusive-timings-are-not-additive\n");
  const char* phases[] = {"baseline", "cpu", "attribution"};
  for (unsigned i = 0; i < 3; ++i) {
    const auto& stats = s.phaseStats[i];
    std::fprintf(file, "phase=%s observer=%s coverageActive=%u contextPolling=%u cpuSuspendSampling=%u elapsedMs=%.6f successfulPresents=%llu failedPresents=%llu fps=%.6f averageIntervalMs=%.6f maxIntervalMs=%.6f invalidTimes=%llu\n",
      phases[i], i == 0 ? "baseline" : "instrumented",
      unsigned(job.telemetry && job.telemetry->available), unsigned(job.context != nullptr), unsigned(i == 1 && job.config.cpu),
      double(stats.elapsedNs()) / 1e6, stats.presents, stats.failedPresents, stats.fps(), stats.averageIntervalNs() / 1e6,
      double(stats.maxIntervalNs) / 1e6, stats.invalidTimes);
    const auto& a = job.threadBegin[i]; const auto& b = job.threadEnd[i];
    const bool available = a.AvailableMask == 7 && b.AvailableMask == 7
      && a.MainThreadId == b.MainThreadId && a.CsThreadId == b.CsThreadId
      && b.WallUs > a.WallUs && b.MainCpu100ns >= a.MainCpu100ns
      && b.CsCpu100ns >= a.CsCpu100ns && b.CsQueueIdleUs >= a.CsQueueIdleUs;
    if (!available) {
      std::fprintf(file, "threadActivity phase=%s available=0 reason=missing-or-incomplete-window\n", phases[i]);
      continue;
    }
    const auto wall = b.WallUs-a.WallUs;
    std::fprintf(file, "threadActivity phase=%s available=1 mainTid=%u csTid=%u wallUs=%llu mainCpuUs=%llu csCpuUs=%llu csQueueIdleUs=%llu mainCpuPercent=%.6f csCpuPercent=%.6f orderedDispatched=%llu orderedCompleted=%llu pendingStart=%llu pendingEnd=%llu completedCsWaits=%llu completedCsWaitUs=%llu completedDeviceGpuWaits=%llu completedDeviceGpuWaitUs=%llu estimatedGpuIdleUs=%llu submittedChunks=%llu queueSubmissions=%llu\n",
      phases[i], b.MainThreadId, b.CsThreadId, wall, (b.MainCpu100ns-a.MainCpu100ns)/10,
      (b.CsCpu100ns-a.CsCpu100ns)/10, b.CsQueueIdleUs-a.CsQueueIdleUs,
      10.0*double(b.MainCpu100ns-a.MainCpu100ns)/double(wall),
      10.0*double(b.CsCpu100ns-a.CsCpu100ns)/double(wall),
      b.OrderedDispatched-a.OrderedDispatched,b.OrderedCompleted-a.OrderedCompleted,
      a.OrderedDispatched-a.OrderedCompleted,b.OrderedDispatched-b.OrderedCompleted,
      b.CsSyncCount-a.CsSyncCount,b.CsSyncUs-a.CsSyncUs,b.GpuSyncCount-a.GpuSyncCount,
      b.GpuSyncUs-a.GpuSyncUs,b.GpuIdleUs-a.GpuIdleUs,b.Chunks-a.Chunks,b.Submits-a.Submits);
  }
  std::fprintf(file, "threadActivitySemantics=CPU-is-scheduled-execution;non-CPU-wall-is-not-proven-idle;CS-queue-idle-includes-open-wait;ordered-pending-includes-inflight-excludes-injected;waits-count-completed-cross-boundary-spans;GPU-waits-device-wide;GPU-idle-estimated;chunks-merged-on-submission;no-forced-flush-or-sync\n");
  const auto& audit = job.audit;
  std::fprintf(file, "backend captureId=%llu available=%u status=%u active=%u observedPresentBoundaries=%llu elapsedMs=%llu qpcFrequency=%llu timing=inclusive-api-elapsed\n",
    audit.CaptureId, audit.Available, audit.Status, audit.Active,
    audit.ObservedPresentBoundaries, audit.ElapsedMs, audit.QpcFrequency);
  for (unsigned i = 0; i < D3D9_BRIDGE_AUDIT_COUNTER_COUNT; ++i) {
    const auto& c = audit.Counters[i];
    const double ms = audit.QpcFrequency ? 1000.0 * double(c.QpcTicks) / double(audit.QpcFrequency) : 0;
    std::fprintf(file, "backendCounter kind=%s calls=%llu callsPerBoundary=%.6f redundantCalls=%llu units=%llu redundantUnits=%llu timingAvailable=%u timedCalls=%llu timingScope=device-locked-api-body-inclusive ",
      AuditName(i), c.Calls, audit.ObservedPresentBoundaries ? double(c.Calls) / double(audit.ObservedPresentBoundaries) : 0,
      c.RedundantCalls, c.Units, c.RedundantUnits, unsigned(c.TimedCalls != 0), c.TimedCalls);
    if (c.TimedCalls) std::fprintf(file, "inclusiveMs=%.9f inclusiveMsPerBoundary=%.9f maxUs=%.9f\n", ms,
      audit.ObservedPresentBoundaries ? ms / double(audit.ObservedPresentBoundaries) : 0,
      audit.QpcFrequency ? 1e6 * double(c.MaxQpcTicks) / double(audit.QpcFrequency) : 0);
    else std::fprintf(file, "inclusiveMs=unavailable inclusiveMsPerBoundary=unavailable maxUs=unavailable\n");
  }
  std::fprintf(file, "backendBindings samplerCacheHits=%llu textureCacheHits=%llu invalidations=%llu stateBlockFastSkips=%llu prepareDraws=%llu prepareCleanDraws=%llu categoryEvents=%llu\n",
    audit.SamplerBindingCacheHits, audit.TextureBindingCacheHits, audit.ResourceBindingCacheInvalidations,
    audit.StateBlockFastSkips, audit.PrepareDraws, audit.PrepareCleanDraws, audit.PrepareCategoryEvents);
  for (unsigned i = 0; i < D3D9_BRIDGE_PREPARE_COUNTER_COUNT; ++i)
    if (audit.Prepare[i].Draws || audit.Prepare[i].Units)
      std::fprintf(file, "prepareCategory index=%u draws=%llu units=%llu\n", i, audit.Prepare[i].Draws, audit.Prepare[i].Units);
  const auto& j = job.journal;
  std::fprintf(file, "journal captureId=%llu available=%u active=%u inFlight=%u frames=%llu presents=%llu transactions=%llu records=%llu droppedFrames=%llu droppedTechniques=%llu droppedPasses=%llu orphanRestores=%llu timing=inclusive-transaction-span;restore-separate\n",
    j.captureId, unsigned(j.available), unsigned(j.active), j.inFlight, j.frames, j.presents, j.transactions,
    j.records, j.droppedFrames, j.droppedTechniques, j.droppedPasses, j.orphanRestores);
  const double tickToMs = j.qpcFrequency ? 1000.0 / double(j.qpcFrequency) : 0;
  for (unsigned i = 0; i < j.topTechniqueCount; ++i) {
    const auto& t = j.topTechniques[i];
    std::fprintf(file, "journalTechnique name=%s transactions=%llu records=%llu passes=%u inclusiveSpanMs=%.9f inclusiveSpanMsPerPresent=%.9f restoreMs=%.9f restoreMsPerPresent=%.9f empty=%llu truncated=%llu\n",
      t.name, t.transactions, t.records, t.passCount, double(t.transactionSpanQpc) * tickToMs,
      j.presents ? double(t.transactionSpanQpc) * tickToMs / double(j.presents) : 0,
      double(t.restoreQpc) * tickToMs, j.presents ? double(t.restoreQpc) * tickToMs / double(j.presents) : 0,
      t.emptyTransactions, t.truncatedTransactions);
  }
  for (unsigned i = 0; i < j.frameStatCount; ++i) {
    const auto& row = j.frameStats[i];
    std::fprintf(file, "journalFrame admissionFrame=%llu transactions=%llu records=%llu inclusiveSpanMs=%.9f restoreMs=%.9f restoreCount=%llu truncated=%llu\n",
      row.frame, row.transactions, row.records, double(row.transactionSpanQpc) * tickToMs,
      double(row.restoreQpc) * tickToMs, row.restoreCount, row.truncatedTransactions);
  }
  if (j.captureStarted && !j.inFlight && !JournalAttributionWriteReport(file))
    throw std::runtime_error("cannot write full technique/pass attribution");
  std::fprintf(file, "frameTrace count=%zu dropped=%llu slowThresholdMs=%u\n", job.trace.size(), job.trace.dropped(), job.config.timing.slowFrameMs);
  for (std::size_t i = 0; i < job.trace.size(); ++i) {
    const auto& row = job.trace.data()[i];
    std::fprintf(file, "frame id=%llu phase=%u beginNs=%llu endNs=%llu intervalMs=%.6f slow=%u beginSuccessfulPresent=%u boundaryTruncated=%u\n",
      row.frameId, unsigned(row.phase), row.beginNs, row.endNs, double(row.endNs - row.beginNs) / 1e6,
      unsigned(row.endNs - row.beginNs > std::uint64_t(job.config.timing.slowFrameMs) * 1'000'000ull),
      unsigned(row.beginIsSuccessfulPresent), unsigned(!row.beginIsSuccessfulPresent));
  }
  if (job.telemetry) {
    const auto& telemetry = *job.telemetry;
    const auto& read = telemetry.read;
    std::fprintf(file, "telemetry available=%u active=%u captureId=%llu source=completed-spans minDurationNs=50000 minBytes=65536 backendCapacity=%u hostCapacity=%zu recorded=%zu backendDropped=%llu hostDropped=%llu invalidCaptureEvents=%llu filtered=%llu started=%llu inFlight=%llu inFlightAtStop=%llu lateDroppedAtRead=%llu remaining=%u error=%s\n",
      unsigned(telemetry.available), unsigned(telemetry.active), telemetry.id, read.Capacity, MeasurementEventStorage::Capacity,
      size_t(telemetry.storage ? telemetry.storage->written.load() : 0), read.Dropped, (telemetry.runtimeDropped + (telemetry.storage ? telemetry.storage->omitted.load() : 0)), telemetry.invalidEvents, read.Filtered, read.Started, read.InFlight,
      read.InFlightAtStop, read.LateDropped, read.Remaining, Quote(telemetry.error).c_str());
    std::fprintf(file, "telemetrySemantics=completed-only;inclusive-spans-may-nest;not-additive;open-at-stop-is-not-zero;overlaps-half-open;trace-gaps-and-dropped-frames-unmatched\n");
    std::fprintf(file, "presentWaitSemantics=frame-latency-signal-was-incomplete-before-wait-may-race;present-acquire-is-inclusive-CPU-driver-and-possible-waits;sampler-pool-wait-is-submit-fence-pressure;none-is-pure-GPU-execution\n");
    std::fprintf(file, "csWorkSemantics=cs-chunk-identity-is-queue-sequence;zero-is-unsynchronized-high-priority;chunk-includes-reference-release-after-sequence-publication;driver-spans-are-inclusive-wall-time;join-by-capture-thread-queue-sequence-and-overlap;filtered-or-open-spans-remain-unobserved\n");
    std::fprintf(file, "retirementSemantics=mapping-retire-is-enqueue-to-completion-latency-not-main-thread-blocked-time;deferred-logical-unmap-releases-owner;os-unmap-on-worker-records-actual-call;pending-bytes-still-count-against-mapped-budget;deferredFlag=0x%X highPriorityFlag=0x%X secondaryFlag=0x%X\n",
      D3D9_BRIDGE_TELEMETRY_DEFERRED,D3D9_BRIDGE_TELEMETRY_HIGH_PRIORITY,D3D9_BRIDGE_TELEMETRY_SECONDARY);
    std::fprintf(file, "telemetryFlags fileBacked=0x%X failed=0x%X allocatorOperation=0x%X deviceMemory=0x%X osMapping=0x%X deviceIdle=0x%X cacheHit=0x%X cacheReturn=0x%X cacheFill=0x%X cacheEvict=0x%X allocatorOperationDoesNotImplyPhysicalFree=1 submissionWaitIsGpuCompletion=0\n",
      D3D9_BRIDGE_TELEMETRY_FILE_BACKED, D3D9_BRIDGE_TELEMETRY_FAILED, D3D9_BRIDGE_TELEMETRY_RESOURCE,
      D3D9_BRIDGE_TELEMETRY_DEVICE_MEMORY, D3D9_BRIDGE_TELEMETRY_OS_MAPPING, D3D9_BRIDGE_TELEMETRY_DEVICE_IDLE,
      D3D9_BRIDGE_TELEMETRY_CACHE_HIT, D3D9_BRIDGE_TELEMETRY_CACHE_RETURN,
      D3D9_BRIDGE_TELEMETRY_CACHE_FILL, D3D9_BRIDGE_TELEMETRY_CACHE_EVICT);
    std::uint64_t unmatchedSpans = 0, partialSpans = 0;
    if (telemetry.storage) telemetry.storage->ForEach([&](std::size_t index, const D3D9BridgeTelemetryEvent& event) {
      std::fprintf(file, "telemetryEvent index=%zu captureId=%llu kind=%u kindName=%s beginNs=%llu endNs=%llu thread=%u identity=0x%llX bytes=%llu result=%d flags=0x%X\n",
        index, event.CaptureId, event.Kind, TelemetryKindName(event.Kind), event.BeginNs, event.EndNs, event.ThreadId, event.Identity,
        event.Bytes, event.Result, event.Flags);
      unsigned overlaps = 0;
      std::uint64_t boundaryTruncatedNs = 0;
      const auto covered = job.trace.ForEachOverlap(event.BeginNs, event.EndNs,
        [&](const FrameRecord& row, std::uint64_t begin, std::uint64_t end) {
          ++overlaps;
          if (!row.beginIsSuccessfulPresent) boundaryTruncatedNs += end - begin;
          const bool slow = row.beginIsSuccessfulPresent && row.endNs - row.beginNs > std::uint64_t(job.config.timing.slowFrameMs) * 1'000'000ull;
          std::fprintf(file, "frameOverlap event=%zu frame=%llu phase=%u beginNs=%llu endNs=%llu overlapNs=%llu class=%s\n",
            index, row.frameId, unsigned(row.phase), begin, end, end - begin,
            !row.beginIsSuccessfulPresent ? "boundary-truncated" : slow ? "slow" : "ordinary");
        });
      const auto duration = event.EndNs >= event.BeginNs ? event.EndNs - event.BeginNs : 0;
      const auto unobserved = duration >= covered ? duration - covered : 0;
      if (!overlaps) ++unmatchedSpans;
      if (unobserved) ++partialSpans;
      std::fprintf(file, "eventCoverage event=%zu overlapFrames=%u coveredNs=%llu boundaryTruncatedNs=%llu unmatchedNs=%llu invalidInterval=%u\n",
        index, overlaps, covered, boundaryTruncatedNs, unobserved, unsigned(event.EndNs < event.BeginNs));
    });
    if (telemetry.storage) {
      auto& storage = *telemetry.storage;
      storage.CheckTerminalAccounting();
      const auto received = storage.received.load(), accepted = storage.accepted.load();
      const auto written = storage.written.load(), reported = storage.reported.load();
      const auto omitted = storage.omitted.load();
      const bool conserved = received == accepted && accepted == written && written == reported && !omitted;
      const bool complete = conserved && !storage.errors.load() && !read.Dropped && !read.LateDropped && !read.InFlight && !read.InFlightAtStop && !read.Remaining && !telemetry.active && !telemetry.invalidEvents && !telemetry.error[0];
      if (!complete) job.snapshot.unavailableMask |= RuntimeTelemetryUnavailable;
      std::fprintf(file, "telemetryStorage complete=%u stagingBytes=%zu fileLimitBytes=%llu received=%llu accepted=%llu written=%llu reported=%llu omitted=%llu unread=%llu errors=0x%X\n",
        unsigned(complete), (MeasurementEventStorage::Capacity + 2 * MeasurementEventStorage::BlockEvents) * sizeof(D3D9BridgeTelemetryEvent), MeasurementEventStorage::FileLimit,
        received, accepted, written, reported, omitted, written >= reported ? written - reported : 0, storage.errors.load());
    }
    std::fprintf(file, "eventCoverageSummary unmatchedSpans=%llu partiallyUnmatchedSpans=%llu frameTraceDropped=%llu\n",
      unmatchedSpans, partialSpans, job.trace.dropped());
    std::fprintf(file, "memoryHistory source=present-and-boundaries cadenceMs=500 maxSnapshots=%zu chunkCapacity=%u maskCapacity=%u snapshots=%zu skipped=%llu failures=%llu totals=pooled-hud-chunks-only\n",
      MaxMemorySnapshots, MemoryChunkCapacity, MemoryMaskCapacity, telemetry.memory.size(), telemetry.snapshotSkipped, telemetry.snapshotFailures);
    std::uint64_t previousNs = 0;
    for (std::size_t index = 0; index < telemetry.memory.size(); ++index) {
      const auto& observation = telemetry.memory[index]; const auto& memory = observation.snapshot;
      const auto* frame = job.trace.Find(memory.TimestampNs);
      const auto interval = previousNs && memory.TimestampNs >= previousNs ? memory.TimestampNs - previousNs : 0;
      previousNs = memory.TimestampNs;
      std::fprintf(file, "memorySnapshot index=%zu boundary=%s timeNs=%llu intervalNs=%llu cadenceGap=%u scanNs=%llu frame=%llu capacity=%llu used=%llu totalChunks=%u copiedChunks=%u totalMasks=%u copiedMasks=%u flags=0x%X\n",
        index, observation.boundary, memory.TimestampNs, interval, unsigned(interval > SnapshotCadenceNs + 100'000'000ull),
        memory.ScanDurationNs, frame ? frame->frameId : 0, memory.TotalCapacity, memory.TotalUsed, memory.TotalChunks,
        memory.CopiedChunks, memory.TotalMasks, memory.CopiedMasks, memory.Flags);
      for (std::size_t chunkIndex = 0; chunkIndex < observation.chunks.size(); ++chunkIndex) {
        const auto& chunk = observation.chunks[chunkIndex];
        std::fprintf(file, "memoryChunk snapshot=%zu index=%zu cookie=0x%llX heap=%u type=%u capacity=%llu used=%llu mapped=%u active=%u pages=%u maskOffset=%u maskCount=%u\n",
          index, chunkIndex, chunk.Cookie, chunk.Heap, chunk.Type, chunk.Capacity, chunk.Used, chunk.Mapped,
          chunk.Active, chunk.PageCount, chunk.MaskOffset, chunk.MaskCount);
        if (chunk.MaskOffset <= observation.masks.size() && chunk.MaskCount <= observation.masks.size() - chunk.MaskOffset)
          for (UINT m = 0; m < chunk.MaskCount; ++m)
            std::fprintf(file, "memoryMask snapshot=%zu chunk=%zu index=%u bits=0x%08X\n",
              index, chunkIndex, m, observation.masks[chunk.MaskOffset + m]);
      }
    }
    for (std::size_t index = 1; index < telemetry.memory.size(); ++index) {
      const auto& before = telemetry.memory[index - 1]; const auto& after = telemetry.memory[index];
      std::map<UINT64, const D3D9BridgeMemoryChunk*> previous, current;
      for (const auto& chunk : before.chunks) previous[chunk.Cookie] = &chunk;
      for (const auto& chunk : after.chunks) current[chunk.Cookie] = &chunk;
      const bool beforeComplete = !(before.snapshot.Flags & D3D9_BRIDGE_MEMORY_CHUNKS_TRUNCATED);
      const bool afterComplete = !(after.snapshot.Flags & D3D9_BRIDGE_MEMORY_CHUNKS_TRUNCATED);
      const auto maskValid = [](const MemoryObservation& sample, const D3D9BridgeMemoryChunk& chunk) {
        return chunk.MaskOffset <= sample.masks.size() && chunk.MaskCount <= sample.masks.size() - chunk.MaskOffset &&
          (chunk.PageCount == 0 || chunk.MaskCount != 0);
      };
      for (const auto& [cookie, chunk] : current) {
        const auto prior = previous.find(cookie);
        if (prior == previous.end()) {
          std::fprintf(file, "memoryChange from=%zu to=%zu cookie=0x%llX change=%s used=%llu\n",
            index - 1, index, cookie, beforeComplete ? "new" : "previous-presence-unknown", chunk->Used);
          continue;
        }
        const auto& old = *prior->second;
        const char* masks = "unknown";
        if (maskValid(before, old) && maskValid(after, *chunk)) {
          const bool same = old.PageCount == chunk->PageCount && old.MaskCount == chunk->MaskCount &&
            std::equal(before.masks.begin() + old.MaskOffset, before.masks.begin() + old.MaskOffset + old.MaskCount,
              after.masks.begin() + chunk->MaskOffset);
          masks = same ? "unchanged" : "changed";
        }
        const auto delta = chunk->Used >= old.Used ? chunk->Used - old.Used : old.Used - chunk->Used;
        if (delta || old.Capacity != chunk->Capacity || old.Mapped != chunk->Mapped || old.Active != chunk->Active ||
            old.Heap != chunk->Heap || old.Type != chunk->Type || std::strcmp(masks, "unchanged") != 0)
          std::fprintf(file, "memoryChange from=%zu to=%zu cookie=0x%llX change=observed usedBefore=%llu usedAfter=%llu usedDirection=%s usedDelta=%llu capacityBefore=%llu capacityAfter=%llu mappedBefore=%u mappedAfter=%u activeBefore=%u activeAfter=%u mask=%s\n",
            index - 1, index, cookie, old.Used, chunk->Used, chunk->Used >= old.Used ? "increase-or-same" : "decrease",
            delta, old.Capacity, chunk->Capacity, old.Mapped, chunk->Mapped, old.Active, chunk->Active, masks);
      }
      for (const auto& [cookie, chunk] : previous) if (!current.count(cookie))
        std::fprintf(file, "memoryChange from=%zu to=%zu cookie=0x%llX change=%s previousUsed=%llu\n",
          index - 1, index, cookie, afterComplete ? "removed" : "current-presence-unknown", chunk->Used);
    }
    std::fprintf(file, "memoryChangeSemantics=adjacent-snapshots;cookie-identity-may-be-reused;truncated-presence-is-unknown;dedicated-allocations-excluded\n");
  } else std::fprintf(file, "telemetry available=0 error=not-started\n");
  if (job.cpu) {
    const auto& cpu = *job.cpu;
    std::fprintf(file, "cpuStream kind=main available=%u thread=%lu intervalMs=%u samples=%zu missed=%llu scheduleMissed=%llu maxScheduleLatenessNs=%llu dropped=%llu suspendedNs=%llu maxSuspendedNs=%llu validFrameChains=%llu partialFrameChains=%llu invalidFrameChains=%llu error=%s\n",
      unsigned(cpu.available), cpu.thread, job.config.intervalMs, cpu.samples.size(), cpu.missed,
      cpu.scheduleMissed, cpu.maxScheduleLatenessNs, cpu.dropped, cpu.suspendedNs, cpu.maxSuspendedNs,
      cpu.validChains, cpu.partialChains, cpu.invalidChains, Quote(cpu.error).c_str());
    std::fprintf(file, "cpu captureId=%llu samples=%zu missed=%llu dropped=%llu suspendedNs=%llu maxSuspendedNs=%llu samplingElapsedNs=%llu sampledPresents=%llu validFrameChains=%llu partialFrameChains=%llu invalidFrameChains=%llu\n",
      cpu.id, cpu.samples.size(), cpu.missed, cpu.dropped, cpu.suspendedNs, cpu.maxSuspendedNs,
      cpu.elapsedNs, cpu.presents, cpu.validChains, cpu.partialChains, cpu.invalidChains);
    for (const auto& module : cpu.modules)
      std::fprintf(file, "module base=0x%llX imageSize=%u timestamp=%u path=%s\n", std::uint64_t(module.base), module.size, module.timestamp, Quote(module.path).c_str());
    for (const auto& function : cpu.functions)
      std::fprintf(file, "function base=0x%llX rva=0x%X samples=%llu percent=%.6f quality=%s provenance=%s name=%s path=%s binarySha256=%s\n",
        std::uint64_t(function.symbol.moduleBase), function.symbol.rva - function.symbol.offset, function.samples,
        cpu.samples.empty() ? 0 : 100.0 * double(function.samples) / double(cpu.samples.size()),
        SymbolQualityName(function.symbol.quality), SymbolProvenanceName(function.symbol.provenance),
        Quote(function.name).c_str(), Quote(function.symbol.modulePath).c_str(), function.symbol.binarySha256.c_str());
    std::map<std::pair<std::uintptr_t, std::uintptr_t>, std::pair<std::uint64_t, std::uint64_t>> byClass;
    std::uint64_t slow = 0, ordinary = 0, unmatched = 0;
    for (const auto& sample : cpu.samples) {
      const auto* frame = job.trace.Find(sample.timeNs);
      const bool completeFrame = frame && frame->beginIsSuccessfulPresent;
      const bool isSlow = completeFrame && frame->endNs - frame->beginNs > std::uint64_t(job.config.timing.slowFrameMs) * 1'000'000ull;
      if (!completeFrame) ++unmatched; else if (isSlow) ++slow; else ++ordinary;
      auto found = cpu.symbols.find(sample.ip);
      if (completeFrame && found != cpu.symbols.end()) {
        const auto& symbol = found->second;
        auto& counts = byClass[{symbol.moduleBase, sample.ip - symbol.offset}];
        if (isSlow) ++counts.first; else ++counts.second;
      }
      std::fprintf(file, "sample stream=main timeNs=%llu frame=%llu phase=%u class=%s ip=0x%llX suspendedNs=%llu chainQuality=%u chainCount=%u\n",
        sample.timeNs, frame ? frame->frameId : 0, frame ? unsigned(frame->phase) : 0,
        !frame ? "unmatched" : !completeFrame ? "boundary-truncated" : isSlow ? "slow" : "ordinary", std::uint64_t(sample.ip), sample.suspendedNs,
        unsigned(sample.chain.quality), sample.chain.count);
      for (unsigned i = 0; i < sample.chain.count; ++i) {
        const auto address = sample.chain.addresses[i]; const auto* module = FindModule(cpu.modules, address);
        std::fprintf(file, "  caller kind=structural-frame-chain quality=%u index=%u address=0x%llX base=0x%llX rva=0x%llX path=%s\n",
          unsigned(sample.chain.quality), i, std::uint64_t(address), module ? std::uint64_t(module->base) : 0,
          module ? std::uint64_t(address - module->base) : 0, Quote(module ? module->path : "private").c_str());
      }
      for (unsigned i = 0; i < sample.candidates.size(); ++i) if (sample.candidates[i]) {
        const auto address = sample.candidates[i]; const auto* module = FindModule(cpu.modules, address);
        std::fprintf(file, "  caller kind=heuristic-candidate index=%u address=0x%llX base=0x%llX rva=0x%llX path=%s\n",
          i, std::uint64_t(address), module ? std::uint64_t(module->base) : 0,
          module ? std::uint64_t(address - module->base) : 0, Quote(module ? module->path : "private").c_str());
      }
    }
    std::fprintf(file, "sampleCoverage slow=%llu ordinary=%llu unmatched=%llu recorded=%zu dropped=%llu\n", slow, ordinary, unmatched, cpu.samples.size(), cpu.dropped);
    for (const auto& [identity, counts] : byClass)
      std::fprintf(file, "functionByFrameClass base=0x%llX functionStart=0x%llX slowSamples=%llu ordinarySamples=%llu\n",
        std::uint64_t(identity.first), std::uint64_t(identity.second), counts.first, counts.second);
    if (cpu.cs) {
      const auto& cs = *cpu.cs;
      std::fprintf(file, "cpuStream kind=cs available=%u thread=%lu intervalMs=%u samples=%zu missed=%llu scheduleMissed=%llu maxScheduleLatenessNs=%llu dropped=%llu suspendedNs=%llu maxSuspendedNs=%llu validFrameChains=%llu partialFrameChains=%llu invalidFrameChains=%llu error=%s\n",
        unsigned(cs.available), cs.thread, job.config.csIntervalMs, cs.samples.size(), cs.missed, cs.scheduleMissed,
        cs.maxScheduleLatenessNs, cs.dropped,
        cs.suspendedNs, cs.maxSuspendedNs, cs.validChains, cs.partialChains, cs.invalidChains, Quote(cpu.csError).c_str());
      std::uint64_t csSlow = 0, csOrdinary = 0, csUnmatched = 0, csBoundary = 0;
      std::map<std::pair<std::uintptr_t, std::uintptr_t>, std::pair<std::uint64_t, std::uint64_t>> csByClass;
      for (const auto& sample : cs.samples) {
        const auto* frame = job.trace.Find(sample.timeNs);
        const bool isSlow = frame && frame->beginIsSuccessfulPresent && frame->endNs - frame->beginNs > std::uint64_t(job.config.timing.slowFrameMs) * 1'000'000ull;
        if (!frame) ++csUnmatched;
        else if (!frame->beginIsSuccessfulPresent) ++csBoundary;
        else {
          if (isSlow) ++csSlow; else ++csOrdinary;
          const auto symbol = cs.symbols.find(sample.ip);
          if (symbol != cs.symbols.end()) {
            auto& counts = csByClass[{symbol->second.moduleBase, sample.ip - symbol->second.offset}];
            if (isSlow) ++counts.first; else ++counts.second;
          }
        }
        std::fprintf(file, "sample stream=cs timeNs=%llu frame=%llu phase=%u class=%s ip=0x%llX suspendedNs=%llu chainQuality=%u chainCount=%u\n",
          sample.timeNs, frame ? frame->frameId : 0, frame ? unsigned(frame->phase) : 0,
          !frame ? "unmatched" : !frame->beginIsSuccessfulPresent ? "boundary-truncated" : isSlow ? "slow" : "ordinary", std::uint64_t(sample.ip), sample.suspendedNs,
          unsigned(sample.chain.quality), sample.chain.count);
        for (unsigned i = 0; i < sample.chain.count; ++i) {
          const auto address = sample.chain.addresses[i]; const auto* module = FindModule(cs.modules, address);
          std::fprintf(file, "  caller stream=cs kind=structural-frame-chain quality=%u index=%u address=0x%llX base=0x%llX rva=0x%llX path=%s\n",
            unsigned(sample.chain.quality), i, std::uint64_t(address), module ? std::uint64_t(module->base) : 0,
            module ? std::uint64_t(address - module->base) : 0, Quote(module ? module->path : "private").c_str());
        }
        for (unsigned i = 0; i < sample.candidates.size(); ++i) if (sample.candidates[i]) {
          const auto address = sample.candidates[i]; const auto* module = FindModule(cs.modules, address);
          std::fprintf(file, "  caller stream=cs kind=heuristic-candidate index=%u address=0x%llX base=0x%llX rva=0x%llX path=%s\n",
            i, std::uint64_t(address), module ? std::uint64_t(module->base) : 0,
            module ? std::uint64_t(address - module->base) : 0, Quote(module ? module->path : "private").c_str());
        }
      }
      std::fprintf(file, "sampleCoverage stream=cs slow=%llu ordinary=%llu unmatched=%llu boundaryTruncated=%llu recorded=%zu dropped=%llu\n",
        csSlow, csOrdinary, csUnmatched, csBoundary, cs.samples.size(), cs.dropped);
      for (const auto& [identity, counts] : csByClass)
        std::fprintf(file, "functionByFrameClass stream=cs base=0x%llX functionStart=0x%llX slowSamples=%llu ordinarySamples=%llu\n",
          std::uint64_t(identity.first), std::uint64_t(identity.second), counts.first, counts.second);
      for (const auto& function : cs.functions)
        std::fprintf(file, "function stream=cs base=0x%llX rva=0x%X samples=%llu quality=%s name=%s path=%s\n",
          std::uint64_t(function.symbol.moduleBase), function.symbol.rva - function.symbol.offset, function.samples,
          SymbolQualityName(function.symbol.quality), Quote(function.name).c_str(), Quote(function.symbol.modulePath).c_str());
    } else std::fprintf(file, "cpuStream kind=cs available=0 reason=%s\n", cpu.csError);
  } else {
    std::fprintf(file, "cpuStream kind=main available=0 reason=no-completed-result\n");
    std::fprintf(file, "cpuStream kind=cs available=0 reason=no-completed-result\n");
  }
  if (job.context) {
    std::lock_guard<std::mutex> lock(job.context->mutex);
    std::fprintf(file, "contextSummary available=%u source=window-poll;safe-game-frame-callback cadenceMs=25 count=%zu dropped=%llu completed=%u failed=%u renderWork=unknown pixelEvidence=none\n",
      unsigned(!job.context->failed.load(std::memory_order_acquire)), job.context->points.size(), job.context->dropped,
      unsigned(job.context->done.load(std::memory_order_acquire)), unsigned(job.context->failed.load(std::memory_order_acquire)));
    std::uint64_t excluded = 0, candidates = 0, gaps = 0;
    for (const auto& point : job.context->points) {
      const auto* frame = job.trace.Find(point.timeNs);
      const bool candidate = point.frameKnown && point.frameUnchangedNs >= 250'000'000ull;
      const bool exclude = (point.windowKnown && point.minimized) ||
        (point.foregroundKnown && !point.foreground) || candidate;
      excluded += exclude; candidates += candidate; gaps += point.presentGap;
      std::fprintf(file, "context kind=window timeNs=%llu known=%u foregroundKnown=%u foreground=%u minimized=%u frame=%llu excludeSteadyState=%u\n",
        point.timeNs, unsigned(point.windowKnown), unsigned(point.foregroundKnown), unsigned(point.foreground), unsigned(point.minimized),
        frame ? frame->frameId : 0, unsigned(exclude));
      std::fprintf(file, "context kind=gameFrame timeNs=%llu known=%u value=%u unchanged=%u unchangedNs=%llu frame=%llu presents=%llu\n",
        point.timeNs, unsigned(point.frameKnown), point.frame, unsigned(point.unchanged), point.frameUnchangedNs,
        frame ? frame->frameId : 0, point.presents);
      if (candidate)
        std::fprintf(file, "reuseCandidate confidence=heuristic source=unchanged-game-frame timeNs=%llu unchangedNs=%llu presentsAdvancedSinceFrameChange=%u presentGap=%u frame=%llu renderWork=unknown pixelEvidence=none\n",
          point.timeNs, point.frameUnchangedNs, unsigned(point.presentsAdvanced), unsigned(point.presentGap), frame ? frame->frameId : 0);
      if (point.presentGap)
        std::fprintf(file, "context kind=presentGap timeNs=%llu thresholdMs=250 presents=%llu frame=%llu\n",
          point.timeNs, point.presents, frame ? frame->frameId : 0);
    }
    std::fprintf(file, "contextCounts excludedPolls=%llu candidatePolls=%llu presentGapPolls=%llu candidateThresholdMs=250 unit=poll scope=observed-only\n",
      excluded, candidates, gaps);
    const auto writeRanges = [&](const char* kind, auto predicate) {
      bool open = false; std::uint64_t begin = 0, last = 0, count = 0;
      for (const auto& point : job.context->points) {
        if (predicate(point)) {
          if (!open) { begin = point.timeNs; count = 0; open = true; }
          last = point.timeNs; ++count;
        } else if (open) {
          std::fprintf(file, "contextRange kind=%s beginObservedNs=%llu endObservedNs=%llu polls=%llu interpolation=none\n", kind, begin, last, count);
          open = false;
        }
      }
      if (open) std::fprintf(file, "contextRange kind=%s beginObservedNs=%llu endObservedNs=%llu polls=%llu interpolation=none\n", kind, begin, last, count);
    };
    writeRanges("excluded-steady-state", [](const ContextPoint& point) {
      return (point.windowKnown && point.minimized) || (point.foregroundKnown && !point.foreground) ||
        (point.frameKnown && point.frameUnchangedNs >= 250'000'000ull);
    });
    writeRanges("frame-stall-candidate", [](const ContextPoint& point) {
      return point.frameKnown && point.frameUnchangedNs >= 250'000'000ull;
    });
  } else std::fprintf(file, "contextSummary available=0 reason=%s renderWork=unknown pixelEvidence=none\n",
    job.config.context ? "worker-unavailable" : "disabled");
  if (job.telemetry && job.telemetry->storage && job.telemetry->storage->errors.load()) {
    job.snapshot.unavailableMask |= RuntimeTelemetryUnavailable;
    CopyText(job.snapshot.lastError, sizeof(job.snapshot.lastError), "event storage incomplete");
  }
  if (s.lastError[0]) std::fprintf(file, "error=%s\n", Quote(s.lastError).c_str());
  std::fprintf(file, "reportEnd captureId=%llu completed=1\n", s.captureId);
  if (std::fseek(file, 0, SEEK_SET)) throw std::runtime_error("cannot finalize combined report header");
  writeHeader(s.phase == Phase::Complete ? (s.unavailableMask ? "complete-partial" : "complete") :
    s.phase == Phase::Interrupted ? "interrupted" : "error");
  if (std::fseek(file, 0, SEEK_END) || std::fflush(file) || std::ferror(file)) throw std::runtime_error("combined report write failed");
}
DWORD WINAPI ReportWorker(void* opaque) {
  WorkerExit activity;
  std::unique_ptr<std::shared_ptr<ReportJob>> holder(static_cast<std::shared_ptr<ReportJob>*>(opaque));
  const auto job = *holder;
  std::string error;
  bool reportWritten = false;
  try {
    if (job->context) {
      const auto deadline = NowNs() + 200'000'000ull;
      while (!job->context->done.load(std::memory_order_acquire) && NowNs() < deadline) Sleep(1);
      if (!job->context->done.load(std::memory_order_acquire) || job->context->failed.load(std::memory_order_acquire)) {
        job->snapshot.unavailableMask |= RuntimeContextUnavailable;
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_capture == job->snapshot.captureId) g_unavailable |= RuntimeContextUnavailable;
      }
    }
    // This wait is outside Runtime/device/owner locks. A missing transaction
    // end yields an explicitly incomplete report after one second or shutdown,
    // rather than a stuck game or falsely complete attribution.
    if (job->journal.captureStarted) {
      const auto deadline = NowNs() + 1'000'000'000ull;
      do {
        JournalAttributionGetSummary(&job->journal);
        if (!job->journal.inFlight) break;
        if (job->config.stopEvent) {
          if (WaitForSingleObject(job->config.stopEvent, 2) != WAIT_TIMEOUT) break;
        } else Sleep(2);
      } while (NowNs() < deadline);
      job->snapshot.phase = job->finalPhase;
      if (job->journal.inFlight) {
        job->snapshot.phase = Phase::Interrupted;
        job->snapshot.unavailableMask |= RuntimeJournalUnavailable;
        CopyText(job->snapshot.lastError, sizeof(job->snapshot.lastError), "journal transactions still pending; partial report");
      } else if (!job->journal.available) job->snapshot.unavailableMask |= RuntimeJournalUnavailable;
      {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_capture == job->snapshot.captureId) {
          g_journal = job->journal; g_journalDrainPending = job->journal.inFlight != 0;
          g_unavailable = job->snapshot.unavailableMask;
          if (job->journal.inFlight) {
            g_reportInterrupted = true;
            g_controller.Interrupt();
            CopyText(g_error, sizeof(g_error), job->snapshot.lastError);
          }
        }
      }
    }
    if (job->telemetry && job->telemetry->storage) {
      auto& storage = *job->telemetry->storage;
      storage.ClaimReader();
      while (!storage.ready.load(std::memory_order_acquire)) Sleep(1);
      if (storage.errors.load()) job->snapshot.unavailableMask |= RuntimeTelemetryUnavailable;
    }
    WriteCombinedReport(*job);
    reportWritten = true;
    if (job->telemetry && job->telemetry->storage)
      job->telemetry->storage->finalizedReported.store(job->telemetry->storage->reported.load());
    if (job->journal.captureStarted && !job->journal.inFlight) JournalAttributionDump();
    Log(job->config, "capture=" + std::to_string(job->snapshot.captureId) + " report=" + job->path);
  } catch (const std::exception& e) { error = e.what(); }
    catch (...) { error = "combined report failed"; }
  if (job->telemetry && job->telemetry->storage) {
    auto& storage = *job->telemetry->storage;
    if (!reportWritten) storage.errors.fetch_or(MeasurementEventStorage::Report);
    storage.reportFinished.store(true, std::memory_order_release);
    storage.consumed.store(true, std::memory_order_release);
    if (storage.errors.load() || (job->snapshot.unavailableMask & RuntimeTelemetryUnavailable)) {
      std::lock_guard<std::mutex> lock(g_mutex);
      if (g_capture == job->snapshot.captureId) {
        g_unavailable |= RuntimeTelemetryUnavailable;
        if (storage.errors.load()) CopyText(g_error, sizeof(g_error), "event storage incomplete");
      }
    }
  }
  if (!error.empty()) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_capture == job->snapshot.captureId) { g_reportFailed = true; CopyText(g_error, sizeof(g_error), error); }
  }
  return 0;
}
template<typename T>
bool Launch(const RuntimeConfig& config, LPTHREAD_START_ROUTINE worker, const std::shared_ptr<T>& job) {
  auto* data = new (std::nothrow) std::shared_ptr<T>(job);
  if (!data) return false;
  g_workers.fetch_add(1, std::memory_order_acq_rel);
  HANDLE handle = nullptr;
  try { handle = config.startWorker ? config.startWorker(worker, data) : nullptr; }
  catch (...) { g_workers.fetch_sub(1, std::memory_order_release); delete data; return false; }
  if (handle) return true;
  g_workers.fetch_sub(1, std::memory_order_release); delete data; return false;
}
void StartReportLocked() {
  if (g_reportStarted || !g_capture) return;
  g_reportStarted = true;
  if (g_context) g_context->cancel.store(true, std::memory_order_relaxed);
  try {
    SyncThreadPhaseLocked();
    auto job = std::make_shared<ReportJob>();
    job->config = g_config; job->path = g_reportPath;
    FillSnapshotLocked(job->snapshot);
    job->finalPhase = g_controller.phase();
    job->trace = g_controller.trace(); job->journal = g_journal; job->audit = g_audit; job->cpu = g_cpuResult;
    job->context = g_context;
    job->telemetry = g_telemetryCapture;
    job->threadBegin = g_threadBegin; job->threadEnd = g_threadEnd;
    if (!Launch(g_config, ReportWorker, job)) {
      g_reportFailed = true; std::snprintf(g_error, sizeof(g_error), "cannot start combined-report worker");
    }
  } catch (...) {
    g_reportFailed = true; std::snprintf(g_error, sizeof(g_error), "cannot allocate combined-report job");
  }
  if (g_reportFailed && g_telemetryCapture && g_telemetryCapture->storage) {
    auto& storage = *g_telemetryCapture->storage;
    storage.errors.fetch_or(MeasurementEventStorage::Report);
    storage.reportFinished.store(true, std::memory_order_release);
    storage.abort = true;
  }
}
void PublishHudLocked(std::uint64_t now, bool force = false) {
  if (!g_diagnostics || (!force && g_lastHudNs && now - g_lastHudNs < 200'000'000ull)) return;
  g_lastHudNs = now;
  if (g_journalStarted) JournalAttributionGetSummary(&g_journal);
  if (g_auditStarted) ReadAuditLocked();
  D3D9BridgeHudSnapshot hud{};
  hud.StructSize = sizeof(hud); hud.Version = D3D9_BRIDGE_DIAGNOSTICS_VERSION;
  hud.CaptureId = g_capture;
  hud.Phase = UINT(g_reportFailed ? Phase::Error : g_reportInterrupted ? Phase::Interrupted : g_controller.phase());
  if (g_journalDrainPending && Phase(hud.Phase) == Phase::Complete) hud.Phase = UINT(Phase::AttributionFinishing);
  hud.Visible = g_config.hud; hud.LineCount = 8;
  const auto phase = Phase(hud.Phase);
  hud.Status = phase == Phase::Interrupted ? D3D9_BRIDGE_AUDIT_INTERRUPTED :
    phase == Phase::Complete ? D3D9_BRIDGE_AUDIT_COMPLETE : phase == Phase::Ready ? D3D9_BRIDGE_AUDIT_IDLE : D3D9_BRIDGE_AUDIT_ACTIVE;
  const auto& stats = g_controller.stats(g_controller.phaseIndex());
  hud.ElapsedMs = stats.elapsedNs() / 1'000'000ull; hud.DurationMs = g_controller.durationMs();
  hud.ProgressPermille = hud.DurationMs ? UINT((std::min)(1000ull, hud.ElapsedMs * 1000ull / hud.DurationMs)) : 0;
  std::snprintf(hud.Lines[0], sizeof(hud.Lines[0]), "RenderStack #%llu: %s", g_capture, PhaseName(phase));
  std::snprintf(hud.Lines[1], sizeof(hud.Lines[1]), "F8/F7: baseline -> CPU -> state | %llu / %llu ms", hud.ElapsedMs, hud.DurationMs);
  std::snprintf(hud.Lines[2], sizeof(hud.Lines[2]), "Live: %.1f FPS | mean %.2f ms | max %.2f ms", g_liveFps, g_liveMeanMs, g_liveMaxMs);
  std::snprintf(hud.Lines[3], sizeof(hud.Lines[3]), "Phase FPS: base %.1f* | CPU %.1f* | state %.1f* (*capture)",
    g_controller.stats(0).fps(), g_controller.stats(1).fps(), g_controller.stats(2).fps());
  if (g_cpuResult) {
    std::snprintf(hud.Lines[4], sizeof(hud.Lines[4]), "CPU: %zu samples, %llu missed, %llu dropped | suspended %.2f ms",
      g_cpuResult->samples.size(), g_cpuResult->missed, g_cpuResult->dropped, double(g_cpuResult->suspendedNs) / 1e6);
    if (!g_cpuResult->functions.empty()) {
      const auto& top = g_cpuResult->functions[0];
      std::snprintf(hud.Lines[5], sizeof(hud.Lines[5]), "Top: %.1f%% %s [%s]",
        g_cpuResult->samples.empty() ? 0 : 100.0 * double(top.samples) / double(g_cpuResult->samples.size()),
        top.name.c_str(), SymbolQualityName(top.symbol.quality));
    }
  } else if (phase == Phase::Cpu)
    std::snprintf(hud.Lines[4], sizeof(hud.Lines[4]), "CPU: sampling in progress | results after sampling");
  else if (phase == Phase::CpuFinishing)
    std::snprintf(hud.Lines[4], sizeof(hud.Lines[4]), "CPU: processing samples | results pending");
  else if (phase == Phase::Ready || phase == Phase::Baseline)
    std::snprintf(hud.Lines[4], sizeof(hud.Lines[4]), "CPU: waiting | no sampling during baseline");
  else std::snprintf(hud.Lines[4], sizeof(hud.Lines[4]), "CPU: no completed sampling result");
  std::snprintf(hud.Lines[6], sizeof(hud.Lines[6]), "State: %llu transactions | API %llu calls, %llu redundant | missing=0x%X",
    g_journal.transactions, g_audit.Total.Calls, g_audit.Total.RedundantCalls, g_unavailable | g_controller.unavailableMask());
  if (g_error[0]) std::snprintf(hud.Lines[7], sizeof(hud.Lines[7]), "Status: %s", g_error);
  else if (g_reportStarted) std::snprintf(hud.Lines[7], sizeof(hud.Lines[7]), "Report: %s%s", Basename(g_reportPath).c_str(), g_workers.load() ? " (writing)" : "");
  else std::snprintf(hud.Lines[7], sizeof(hud.Lines[7]), "Detailed functions, slow frames, technique/pass and API timing in report");
  g_diagnostics->PublishHud(&hud);
}
void InterruptLocked(const char* reason, bool releaseDevice) {
  if (g_cpuJob) g_cpuJob->cancel.store(true, std::memory_order_relaxed);
  if (g_context) g_context->cancel.store(true, std::memory_order_relaxed);
  const bool wasActive = g_controller.active();
  if (wasActive) {
    g_controller.Interrupt(); CopyText(g_error, sizeof(g_error), reason ? reason : "interrupted");
    StopAttributionLocked(); EndTelemetryLocked(NowNs(), "interrupted"); StartReportLocked();
  }
  PublishHudLocked(NowNs(), true);
  if (releaseDevice) {
    StopAuditSafelyLocked();
    if (g_telemetry && g_telemetryCapture && g_telemetryCapture->active) {
      // An earlier stop failure must not be allowed to retain a recorder on a
      // device being destroyed. Do not mutate a report already handed off.
      try { g_telemetry->StopCapture(g_telemetryCapture->id); } catch (...) { }
    }
    if (g_diagnostics) {
      D3D9BridgeHudSnapshot hidden{};
      hidden.StructSize = sizeof(hidden); hidden.Version = D3D9_BRIDGE_DIAGNOSTICS_VERSION;
      g_diagnostics->PublishHud(&hidden);
      g_diagnostics->SetCaptureOwner(FALSE);
    }
    g_diagnostics = nullptr; g_threadStats = nullptr; g_telemetry = nullptr; g_device = nullptr; g_deviceWindow = nullptr; g_owner = 0;
    g_auditStarted = false; g_auditCapture = 0;
  } else g_deviceWindow = nullptr; // Reset may replace the presentation window.
}
} // namespace

void ConfigureRuntime(const RuntimeConfig& config) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_workers.load(std::memory_order_acquire)) return; // host joins before reconfigure
  if (g_journalDrainPending) {
    JournalAttributionGetSummary(&g_journal);
    if (g_journal.inFlight) return; // never reset a late transaction's tables
    g_journalDrainPending = false;
  }
  if (g_diagnostics) {
    D3D9BridgeHudSnapshot hidden{};
    hidden.StructSize = sizeof(hidden); hidden.Version = D3D9_BRIDGE_DIAGNOSTICS_VERSION;
    g_diagnostics->PublishHud(&hidden);
    g_diagnostics->SetCaptureOwner(FALSE);
  }
  g_config = config; g_directory = config.gameDirectory ? config.gameDirectory : ".";
  g_controller = Controller(config.timing);
  g_device = nullptr; g_diagnostics = nullptr; g_threadStats = nullptr; g_telemetry = nullptr; g_deviceWindow = nullptr; g_owner = 0; g_capture = 0;
  g_threadBegin = {}; g_threadEnd = {}; g_threadPhase = -1;
  g_cpuJob.reset(); g_cpuResult.reset(); g_context.reset(); g_telemetryCapture.reset(); g_cpuReady = false;
  g_journalStarted = g_auditStarted = g_reportFailed = g_reportStarted = g_reportInterrupted = false;
  g_auditCapture = 0;
  g_journalDrainPending = false;
  g_journal = {}; g_audit = {}; g_unavailable = 0; g_lastHudNs = 0;
  g_reportPath[0] = g_error[0] = 0; g_liveStarted = false;
  g_liveFps = g_liveMeanMs = g_liveMaxMs = 0;
  g_enabled.store(config.enabled, std::memory_order_release);
}
bool RuntimeEnabled() noexcept { return g_enabled.load(std::memory_order_acquire); }

static HWND RuntimeDeviceWindow(IDirect3DDevice9* device) {
  HWND window = nullptr;
  IDirect3DSwapChain9* swapChain = nullptr;
  D3DPRESENT_PARAMETERS parameters{};
  bool presentationParametersKnown = false;
  if (SUCCEEDED(device->GetSwapChain(0, &swapChain)) && swapChain) {
    if (SUCCEEDED(swapChain->GetPresentParameters(&parameters))) {
      window = parameters.hDeviceWindow; presentationParametersKnown = true;
    }
    swapChain->Release();
  }
  if (presentationParametersKnown && !window) {
    D3DDEVICE_CREATION_PARAMETERS creation{};
    if (SUCCEEDED(device->GetCreationParameters(&creation))) window = creation.hFocusWindow;
  }
  return window;
}
void RuntimePrepareDevice(IDirect3DDevice9* device) {
  if (!RuntimeEnabled() || !device) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_device) return;
  ID3D9BridgeDiagnostics* diagnostics = nullptr;
  if (SUCCEEDED(device->QueryInterface(IID_D3D9BridgeDiagnostics, reinterpret_cast<void**>(&diagnostics)))) {
    const auto release = [](ID3D9BridgeDiagnostics* p) { p->Release(); };
    std::unique_ptr<ID3D9BridgeDiagnostics, decltype(release)> ref(diagnostics, release);
    if (SUCCEEDED(diagnostics->SetCaptureOwner(TRUE))) g_diagnostics = diagnostics;
    else g_unavailable |= RuntimeBackendUnavailable;
  } else g_unavailable |= RuntimeBackendUnavailable;
  ID3D9BridgeThreadStats* threadStats = nullptr;
  if (SUCCEEDED(device->QueryInterface(IID_D3D9BridgeThreadStats, reinterpret_cast<void**>(&threadStats)))) {
    g_threadStats = threadStats;
    threadStats->Release();
  }
  ID3D9BridgeTelemetry* telemetry = nullptr;
  if (SUCCEEDED(device->QueryInterface(IID_D3D9BridgeTelemetry, reinterpret_cast<void**>(&telemetry)))) {
    g_telemetry = telemetry;
    telemetry->Release();
  }
  g_deviceWindow = RuntimeDeviceWindow(device);
  g_device = device; // publish borrowed state only after successful setup
}

void RuntimePresent(IDirect3DDevice9* device, HRESULT result, HWND windowOverride) {
  if (!RuntimeEnabled() || !device) return;
  RuntimePrepareDevice(device);
  std::lock_guard<std::mutex> lock(g_mutex);
  // A secondary device must not claim the selected device's render thread.
  if (g_device != device) return;
  const auto now = NowNs();
  if (g_journalDrainPending && !g_workers.load(std::memory_order_acquire)) {
    JournalAttributionGetSummary(&g_journal);
    if (!g_journal.inFlight) g_journalDrainPending = false;
  }
  if (!g_owner) g_owner = GetCurrentThreadId();
  g_controller.SetAvailable(g_config.cpu, g_config.journal || g_diagnostics != nullptr);
  if (g_owner != GetCurrentThreadId()) return;
  if (g_context) g_context->window.store(windowOverride ? windowOverride : g_deviceWindow, std::memory_order_relaxed);
  const bool success = result == S_OK;
  if (success) g_successfulPresents.fetch_add(1, std::memory_order_relaxed);
  if (!g_liveStarted) { g_live.Begin(now); g_liveStarted = true; }
  else {
    g_live.Observe(now, success);
    if (g_live.elapsedNs() >= 500'000'000ull) {
      g_liveFps = g_live.fps(); g_liveMeanMs = g_live.averageIntervalNs() / 1e6;
      g_liveMaxMs = double(g_live.maxIntervalNs) / 1e6; g_live.Begin(now);
    }
  }
  const auto key = [&](int vk) { return g_config.keyDown ? g_config.keyDown(vk) : (GetAsyncKeyState(vk) & 0x8000) != 0; };
  const bool f7 = key(VK_F7), f8 = key(VK_F8);
  if ((!g_workers.load(std::memory_order_acquire) && !g_journalDrainPending) || g_controller.active()) {
    const bool newCapture = g_controller.Key(f7, f8, now, success);
    if (newCapture) {
      if (!g_deviceWindow) g_deviceWindow = RuntimeDeviceWindow(device);
      StopAuditSafelyLocked();
      if (g_telemetry && g_telemetryCapture && g_telemetryCapture->active) {
        try { g_telemetry->StopCapture(g_telemetryCapture->id); } catch (...) { }
      }
      g_capture = ++g_nextCapture;
      g_cpuJob.reset(); g_cpuResult.reset(); g_cpuReady = false;
      g_reportStarted = g_reportFailed = g_reportInterrupted = false; g_error[0] = 0;
      g_reportPath[0] = 0;
      const auto written = std::snprintf(g_reportPath, sizeof(g_reportPath),
        "%s\\scripts\\BridgeD3D9.measurement-%lu-%llu.log", g_directory.empty() ? "." : g_directory.c_str(),
        GetCurrentProcessId(), g_capture);
      if (written < 0 || std::size_t(written) >= sizeof(g_reportPath)) {
        g_reportPath[0] = 0; g_controller.Interrupt(); g_reportFailed = true;
        std::snprintf(g_error, sizeof(g_error), "measurement report path too long");
        PublishHudLocked(now, true); return;
      }
      g_journal = {}; g_audit = {};
      g_threadBegin = {}; g_threadEnd = {}; g_threadPhase = -1;
      SyncThreadPhaseLocked();
      g_unavailable = g_diagnostics ? 0 : RuntimeBackendUnavailable;
      if (g_auditStarted) g_unavailable |= RuntimeBackendUnavailable;
      if (!g_config.journal) g_unavailable |= RuntimeJournalUnavailable;
      BeginTelemetryLocked(now);
      g_context.reset();
      if (g_config.context) try {
        g_context = std::make_shared<ContextJob>();
        g_context->config = g_config; g_context->id = g_capture;
        g_context->points.reserve(4096);
        g_context->window.store(windowOverride ? windowOverride : g_deviceWindow, std::memory_order_relaxed);
        if (!Launch(g_config, ContextWorker, g_context)) {
          g_context.reset(); g_unavailable |= RuntimeContextUnavailable;
        }
      } catch (...) { g_context.reset(); g_unavailable |= RuntimeContextUnavailable; }
      Log(g_config, "capture=" + std::to_string(g_capture) + " started baseline -> CPU -> attribution");
      PublishHudLocked(now, true);
      return;
    }
  } else g_controller.SuppressKeys(f7, f8);
  PollTelemetryLocked(now);
  if (g_cpuReady && (g_controller.phase() == Phase::Cpu || g_controller.phase() == Phase::CpuFinishing)) {
    g_controller.CpuFinished(g_cpuResult && g_cpuResult->error.empty());
    g_cpuReady = false;
  }
  if (g_journalStarted && success) JournalAttributionOnPresent();
  const auto action = g_controller.Present(now, success);
  SyncThreadPhaseLocked();
  if (action == Action::StartCpu) {
    bool started = false;
    try {
      auto job = std::make_shared<CpuJob>(); job->config = g_config; job->directory = g_directory;
      job->thread = g_owner; job->id = g_capture; job->csThread = g_threadBegin[1].CsThreadId;
      g_cpuJob = job;
      started = Launch(g_config, CpuWorker, job);
    } catch (...) { }
    if (!started) {
      g_cpuJob.reset();
      g_unavailable |= RuntimeCpuUnavailable;
      if (g_config.csCpu) g_unavailable |= RuntimeCsCpuUnavailable;
      std::snprintf(g_error, sizeof(g_error), "cannot start CPU sampler worker");
      g_controller.CpuFinished(false);
      if (g_controller.Present(now, success) == Action::ReportError) {
        SyncThreadPhaseLocked();
        StopAttributionLocked(); EndTelemetryLocked(now, "error"); StartReportLocked();
      }
    }
  } else if (action == Action::StartAttribution) {
    if (g_config.journal) {
      JournalAttributionSetCaptureIdentity(g_capture); JournalAttributionStartCapture(); g_journalStarted = true;
    }
    if (g_diagnostics && !g_auditStarted) {
      // StartAudit may have changed backend state before an exception crosses
      // the private COM boundary; retain its identity before making the call.
      g_auditStarted = true; g_auditCapture = g_capture;
      try {
        const auto started = g_diagnostics->StartAudit(g_capture, g_config.timing.attributionMs);
        if (SUCCEEDED(started)) ReadAuditLocked();
        else { g_unavailable |= RuntimeBackendUnavailable; StopAuditSafelyLocked(); }
      } catch (...) { g_unavailable |= RuntimeBackendUnavailable; StopAuditSafelyLocked(); }
    } else if (g_auditStarted) {
      g_unavailable |= RuntimeBackendUnavailable;
    }
  } else if (action == Action::FinishAttribution || action == Action::ReportError) {
    StopAttributionLocked(); EndTelemetryLocked(now, action == Action::ReportError ? "error" : "end"); StartReportLocked();
  }
  PublishHudLocked(now, action != Action::None);
}
void RuntimeInterrupt(IDirect3DDevice9* device, const char* reason, bool releaseDevice) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (device && device != g_device) return;
  InterruptLocked(reason, releaseDevice);
}
void RuntimeShutdown() {
  std::lock_guard<std::mutex> lock(g_mutex);
  InterruptLocked("shutdown", true);
  g_enabled.store(false, std::memory_order_release);
}
bool RuntimeGetSnapshot(RuntimeSnapshot* snapshot) {
  if (!snapshot) return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  FillSnapshotLocked(*snapshot);
  return true;
}
} // namespace BridgeMeasurement
