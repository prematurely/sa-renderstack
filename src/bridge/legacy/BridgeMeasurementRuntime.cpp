#include "BridgeMeasurementRuntime.h"
#include "MeasurementSymbols.h"
#include "ProperShadersStateJournal.h"
#include "../../../backend/dxvk/src/d3d9/d3d9_bridge_diagnostics.h"
#include "../../../backend/dxvk/src/d3d9/d3d9_bridge_thread_stats.h"

#include <tlhelp32.h>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace BridgeMeasurement {
namespace {
constexpr std::size_t MaxSamples = 16384;
constexpr std::size_t StackBytes = 4096;
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
  std::uint64_t validChains = 0, partialChains = 0, invalidChains = 0;
  std::vector<RawSample> samples;
  std::vector<RuntimeCpuModule> modules;
  std::vector<CpuFunction> functions;
  std::unordered_map<std::uintptr_t, SymbolResult> symbols;
  std::string error;
};
struct CpuJob {
  RuntimeConfig config;
  std::string directory;
  DWORD thread = 0;
  std::uint64_t id = 0;
  std::atomic<bool> cancel{false};
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
std::array<D3D9BridgeThreadSnapshot, 3> g_threadBegin{}, g_threadEnd{};
int g_threadPhase = -1;
DWORD g_owner = 0;
std::shared_ptr<CpuJob> g_cpuJob;
std::shared_ptr<CpuResult> g_cpuResult;
bool g_cpuReady = false, g_journalStarted = false, g_auditStarted = false;
bool g_reportFailed = false, g_reportStarted = false, g_reportInterrupted = false;
bool g_journalDrainPending = false;
std::uint64_t g_nextCapture = 0, g_capture = 0, g_lastHudNs = 0;
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
  MODULEENTRY32 entry{}; entry.dwSize = sizeof(entry);
  if (Module32First(snapshot, &entry)) do {
    RuntimeCpuModule module;
    module.base = reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
    module.size = entry.modBaseSize;
    module.path = entry.szExePath;
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
  } while (Module32Next(snapshot, &entry));
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
  HANDLE thread = nullptr;
  try {
    result = std::make_shared<CpuResult>(); result->id = job->id;
    result->samples.reserve(MaxSamples);
    std::vector<Range> ranges;
    CollectModules(result->modules, ranges);
    thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, job->thread);
    if (!thread || GetProcessIdOfThread(thread) != GetCurrentProcessId()) throw std::runtime_error("cannot open capture owner thread");
    ULONG64 startCycles = 0, endCycles = 0;
    QueryThreadCycleTime(thread, &startCycles);
    const auto startPresents = g_successfulPresents.load(std::memory_order_relaxed);
    result->startedNs = NowNs();
    Jitter jitter(DWORD(job->id) ^ job->thread);
    std::array<std::uint32_t, StackBytes / 4> stack{};
    while (!Stopping(*job) && NowNs() - result->startedNs < std::uint64_t(job->config.timing.cpuMs) * 1'000'000ull) {
      const DWORD delay = jitter.Next(job->config.intervalMs);
      if (job->config.stopEvent) {
        if (WaitForSingleObject(job->config.stopEvent, delay) != WAIT_TIMEOUT) break;
      } else Sleep(delay);
      if (Stopping(*job)) break;
      const auto pauseStart = NowNs();
      if (SuspendThread(thread) == DWORD(-1)) { ++result->missed; continue; }
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
      result->suspendedNs += sample.suspendedNs;
      result->maxSuspendedNs = (std::max)(result->maxSuspendedNs, sample.suspendedNs);
      if (!got || !sample.ip || resumed == DWORD(-1)) { ++result->missed; continue; }
      // All parsing and aggregation happens after ResumeThread. The structural
      // chain has its own quality; candidate executable words stay heuristic.
      sample.chain = WalkCapturedFrames(sp, stack.data(), copied, bp,
        [&](std::uintptr_t address) { return Executable(ranges, address); });
      unsigned candidates = 0;
      for (unsigned i = 0; i < copied / 4 && i < 64 && candidates < 3; ++i) {
        const auto address = std::uintptr_t(stack[i]);
        if (address != sample.ip && Executable(ranges, address)) sample.candidates[candidates++] = address;
      }
      if (sample.chain.quality == ChainQuality::FrameChain) ++result->validChains;
      else if (sample.chain.quality == ChainQuality::Partial || sample.chain.quality == ChainQuality::Truncated) ++result->partialChains;
      else ++result->invalidChains;
      if (result->samples.size() < MaxSamples) result->samples.push_back(sample);
      else ++result->dropped;
    }
    result->elapsedNs = NowNs() - result->startedNs;
    result->presents = g_successfulPresents.load(std::memory_order_relaxed) - startPresents;
    QueryThreadCycleTime(thread, &endCycles);
    result->cycles = endCycles >= startCycles ? endCycles - startCycles : 0;
    CloseHandle(thread); thread = nullptr;
    if (Stopping(*job)) throw std::runtime_error("CPU capture interrupted");
    if (result->samples.empty()) throw std::runtime_error("no valid CPU samples");
    ResolveCpu(*result);
    if (Stopping(*job)) throw std::runtime_error("CPU capture interrupted");
    WriteCpuLog(*job, *result);
  } catch (const std::exception& e) { if (result) result->error = e.what(); }
    catch (...) { if (result) result->error = "CPU capture failed"; }
  if (thread) CloseHandle(thread);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_cpuJob == job && g_capture == job->id) {
      g_cpuResult = result;
      g_cpuReady = true;
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
void FillSnapshotLocked(RuntimeSnapshot& snapshot) {
  snapshot = {};
  snapshot.phase = g_reportFailed ? Phase::Error : g_reportInterrupted ? Phase::Interrupted : g_controller.phase();
  if (g_journalDrainPending && snapshot.phase == Phase::Complete) snapshot.phase = Phase::AttributionFinishing;
  snapshot.captureId = g_capture;
  snapshot.unavailableMask = g_unavailable | g_controller.unavailableMask();
  for (unsigned i = 0; i < 3; ++i) snapshot.phaseStats[i] = g_controller.stats(i);
  snapshot.frameTraceDropped = g_controller.trace().dropped();
  snapshot.workerBusy = g_workers.load(std::memory_order_acquire) != 0 || g_journalDrainPending;
  snapshot.hudRequested = g_config.hud;
  snapshot.hudIntegrationAvailable = g_diagnostics != nullptr;
  if (g_cpuResult) {
    snapshot.cpuSamples = g_cpuResult->samples.size(); snapshot.cpuMissed = g_cpuResult->missed;
    snapshot.cpuDropped = g_cpuResult->dropped; snapshot.suspendedNs = g_cpuResult->suspendedNs;
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
void StopAttributionLocked() {
  if (g_journalStarted) {
    // Present/Reset can already hold the device lock or be inside an admitted
    // transaction. Never drain it synchronously from that calling thread.
    JournalAttributionCloseAdmission();
    JournalAttributionGetSummary(&g_journal);
    g_journalDrainPending = g_journal.inFlight != 0;
    g_journalStarted = false;
    if (!g_journalDrainPending && !g_journal.available) g_unavailable |= RuntimeJournalUnavailable;
  }
  if (g_diagnostics && g_auditStarted) {
    g_diagnostics->StopAudit(g_capture);
    ReadAuditLocked();
    g_auditStarted = false;
  }
}
void WriteCombinedReport(const ReportJob& job) {
  std::filesystem::create_directories(std::filesystem::path(job.path).parent_path());
  FILE* file = nullptr;
  if (fopen_s(&file, job.path.c_str(), "w") || !file) throw std::runtime_error("cannot write combined report");
  struct FileClose { FILE* file; ~FileClose() { std::fclose(file); } } close{file};
  const auto& s = job.snapshot;
  const char* status = s.phase == Phase::Complete ? (s.unavailableMask ? "complete-partial" : "complete") :
    s.phase == Phase::Interrupted ? "interrupted" : "error";
  std::fprintf(file, "# measurement captureId=%llu status=%s pid=%lu clock=qpc-nanoseconds unavailableMask=%u\n",
    s.captureId, status, GetCurrentProcessId(), s.unavailableMask);
  std::fprintf(file, "hud requested=%u integrationAvailable=%u baselineKeepsSameHudConfiguration=1\n", unsigned(s.hudRequested), unsigned(s.hudIntegrationAvailable));
  std::fprintf(file, "limitations=sampling-is-statistical;frame-chain-is-structural;candidate-callers-are-heuristic;inclusive-timings-are-not-additive\n");
  const char* phases[] = {"baseline", "cpu", "attribution"};
  for (unsigned i = 0; i < 3; ++i) {
    const auto& stats = s.phaseStats[i];
    std::fprintf(file, "phase=%s observer=%s elapsedMs=%.6f successfulPresents=%llu failedPresents=%llu fps=%.6f averageIntervalMs=%.6f maxIntervalMs=%.6f invalidTimes=%llu\n",
      phases[i], i == 0 ? "baseline" : "instrumented", double(stats.elapsedNs()) / 1e6,
      stats.presents, stats.failedPresents, stats.fps(), stats.averageIntervalNs() / 1e6,
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
    std::fprintf(file, "frame id=%llu phase=%u beginNs=%llu endNs=%llu intervalMs=%.6f slow=%u\n",
      row.frameId, unsigned(row.phase), row.beginNs, row.endNs, double(row.endNs - row.beginNs) / 1e6,
      unsigned(row.endNs - row.beginNs > std::uint64_t(job.config.timing.slowFrameMs) * 1'000'000ull));
  }
  if (job.cpu) {
    const auto& cpu = *job.cpu;
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
      const bool isSlow = frame && frame->endNs - frame->beginNs > std::uint64_t(job.config.timing.slowFrameMs) * 1'000'000ull;
      if (!frame) ++unmatched; else if (isSlow) ++slow; else ++ordinary;
      auto found = cpu.symbols.find(sample.ip);
      if (frame && found != cpu.symbols.end()) {
        const auto& symbol = found->second;
        auto& counts = byClass[{symbol.moduleBase, sample.ip - symbol.offset}];
        if (isSlow) ++counts.first; else ++counts.second;
      }
      std::fprintf(file, "sample timeNs=%llu frame=%llu phase=%u class=%s ip=0x%llX suspendedNs=%llu chainQuality=%u chainCount=%u\n",
        sample.timeNs, frame ? frame->frameId : 0, frame ? unsigned(frame->phase) : 0,
        !frame ? "unmatched" : isSlow ? "slow" : "ordinary", std::uint64_t(sample.ip), sample.suspendedNs,
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
  }
  if (s.lastError[0]) std::fprintf(file, "error=%s\n", Quote(s.lastError).c_str());
  if (std::fflush(file) || std::ferror(file)) throw std::runtime_error("combined report write failed");
}
DWORD WINAPI ReportWorker(void* opaque) {
  WorkerExit activity;
  std::unique_ptr<std::shared_ptr<ReportJob>> holder(static_cast<std::shared_ptr<ReportJob>*>(opaque));
  const auto job = *holder;
  std::string error;
  try {
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
    WriteCombinedReport(*job);
    if (job->journal.captureStarted && !job->journal.inFlight) JournalAttributionDump();
    Log(job->config, "capture=" + std::to_string(job->snapshot.captureId) + " report=" + job->path);
  } catch (const std::exception& e) { error = e.what(); }
    catch (...) { error = "combined report failed"; }
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
  const HANDLE handle = config.startWorker ? config.startWorker(worker, data) : nullptr;
  if (handle) return true;
  g_workers.fetch_sub(1, std::memory_order_release); delete data; return false;
}
void StartReportLocked() {
  if (g_reportStarted || !g_capture) return;
  g_reportStarted = true;
  SyncThreadPhaseLocked();
  auto job = std::make_shared<ReportJob>();
  job->config = g_config; job->path = g_reportPath;
  FillSnapshotLocked(job->snapshot);
  job->finalPhase = g_controller.phase();
  job->trace = g_controller.trace(); job->journal = g_journal; job->audit = g_audit; job->cpu = g_cpuResult;
  job->threadBegin = g_threadBegin; job->threadEnd = g_threadEnd;
  if (!Launch(g_config, ReportWorker, job)) {
    g_reportFailed = true; CopyText(g_error, sizeof(g_error), "cannot start combined-report worker");
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
  std::snprintf(hud.Lines[3], sizeof(hud.Lines[3]), "Phase FPS: base %.1f | CPU %.1f* | state %.1f* (*instrumented)",
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
  } else std::snprintf(hud.Lines[4], sizeof(hud.Lines[4]), "CPU: waiting | no sampling during baseline");
  std::snprintf(hud.Lines[6], sizeof(hud.Lines[6]), "State: %llu transactions | API %llu calls, %llu redundant | missing=0x%X",
    g_journal.transactions, g_audit.Total.Calls, g_audit.Total.RedundantCalls, g_unavailable | g_controller.unavailableMask());
  if (g_error[0]) std::snprintf(hud.Lines[7], sizeof(hud.Lines[7]), "Status: %s", g_error);
  else if (g_reportStarted) std::snprintf(hud.Lines[7], sizeof(hud.Lines[7]), "Report: %s%s", Basename(g_reportPath).c_str(), g_workers.load() ? " (writing)" : "");
  else std::snprintf(hud.Lines[7], sizeof(hud.Lines[7]), "Detailed functions, slow frames, technique/pass and API timing in report");
  g_diagnostics->PublishHud(&hud);
}
void InterruptLocked(const char* reason, bool releaseDevice) {
  if (g_cpuJob) g_cpuJob->cancel.store(true, std::memory_order_relaxed);
  const bool wasActive = g_controller.active();
  if (wasActive) {
    g_controller.Interrupt(); CopyText(g_error, sizeof(g_error), reason ? reason : "interrupted");
    StopAttributionLocked(); StartReportLocked();
  }
  PublishHudLocked(NowNs(), true);
  if (releaseDevice) {
    if (g_diagnostics) {
      D3D9BridgeHudSnapshot hidden{};
      hidden.StructSize = sizeof(hidden); hidden.Version = D3D9_BRIDGE_DIAGNOSTICS_VERSION;
      g_diagnostics->PublishHud(&hidden);
      g_diagnostics->SetCaptureOwner(FALSE);
    }
    g_diagnostics = nullptr; g_threadStats = nullptr; g_device = nullptr; g_owner = 0;
  }
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
  g_device = nullptr; g_diagnostics = nullptr; g_threadStats = nullptr; g_owner = 0; g_capture = 0;
  g_threadBegin = {}; g_threadEnd = {}; g_threadPhase = -1;
  g_cpuJob.reset(); g_cpuResult.reset(); g_cpuReady = false;
  g_journalStarted = g_auditStarted = g_reportFailed = g_reportStarted = g_reportInterrupted = false;
  g_journalDrainPending = false;
  g_journal = {}; g_audit = {}; g_unavailable = 0; g_lastHudNs = 0;
  g_reportPath[0] = g_error[0] = 0; g_liveStarted = false;
  g_liveFps = g_liveMeanMs = g_liveMaxMs = 0;
  g_enabled.store(config.enabled, std::memory_order_release);
}
bool RuntimeEnabled() noexcept { return g_enabled.load(std::memory_order_acquire); }

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
  g_device = device; // publish borrowed state only after successful setup
}

void RuntimePresent(IDirect3DDevice9* device, HRESULT result) {
  if (!RuntimeEnabled() || !device) return;
  RuntimePrepareDevice(device);
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto now = NowNs();
  if (g_journalDrainPending && !g_workers.load(std::memory_order_acquire)) {
    JournalAttributionGetSummary(&g_journal);
    if (!g_journal.inFlight) g_journalDrainPending = false;
  }
  if (!g_owner) g_owner = GetCurrentThreadId();
  g_controller.SetAvailable(g_config.cpu, g_config.journal || g_diagnostics != nullptr);
  if (g_device != device || g_owner != GetCurrentThreadId()) return;
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
    const bool newCapture = g_controller.Key(f7, f8, now);
    if (newCapture) {
      g_capture = ++g_nextCapture;
      g_cpuJob.reset(); g_cpuResult.reset(); g_cpuReady = false;
      g_reportStarted = g_reportFailed = g_reportInterrupted = false; g_error[0] = 0;
      g_journal = {}; g_audit = {};
      g_threadBegin = {}; g_threadEnd = {}; g_threadPhase = -1;
      SyncThreadPhaseLocked();
      g_unavailable = g_diagnostics ? 0 : RuntimeBackendUnavailable;
      if (!g_config.journal) g_unavailable |= RuntimeJournalUnavailable;
      const auto path = (std::filesystem::path(g_directory) / "scripts" /
        ("BridgeD3D9.measurement-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(g_capture) + ".log")).string();
      CopyText(g_reportPath, sizeof(g_reportPath), path);
      Log(g_config, "capture=" + std::to_string(g_capture) + " started baseline -> CPU -> attribution");
      PublishHudLocked(now, true);
      return;
    }
  } else g_controller.SuppressKeys(f7, f8);
  if (g_cpuReady && (g_controller.phase() == Phase::Cpu || g_controller.phase() == Phase::CpuFinishing)) {
    g_controller.CpuFinished(g_cpuResult && g_cpuResult->error.empty());
    g_cpuReady = false;
  }
  if (g_journalStarted && success) JournalAttributionOnPresent();
  const auto action = g_controller.Present(now, success);
  SyncThreadPhaseLocked();
  if (action == Action::StartCpu) {
    auto job = std::make_shared<CpuJob>(); job->config = g_config; job->directory = g_directory;
    job->thread = g_owner; job->id = g_capture; g_cpuJob = job;
    if (!g_config.cpu) {
      g_unavailable |= RuntimeCpuUnavailable; g_controller.CpuFinished(true);
    } else if (!Launch(g_config, CpuWorker, job)) {
      CopyText(g_error, sizeof(g_error), "cannot start CPU sampler worker"); g_controller.CpuFinished(false);
    }
  } else if (action == Action::StartAttribution) {
    if (g_config.journal) {
      JournalAttributionSetCaptureIdentity(g_capture); JournalAttributionStartCapture(); g_journalStarted = true;
    }
    if (g_diagnostics) {
      const auto started = g_diagnostics->StartAudit(g_capture, g_config.timing.attributionMs);
      if (SUCCEEDED(started)) { g_auditStarted = true; ReadAuditLocked(); }
      else g_unavailable |= RuntimeBackendUnavailable;
    }
  } else if (action == Action::FinishAttribution || action == Action::ReportError) {
    StopAttributionLocked(); StartReportLocked();
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
