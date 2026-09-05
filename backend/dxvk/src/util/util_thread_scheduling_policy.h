#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#include <cwchar>
#endif
#include <cstring>
#include <algorithm>
#ifdef _WIN32
#include <windows.h>
#endif

namespace renderstack::scheduling {

enum class Role { DeviceOwner, CommandStream, Background };
struct Options { bool enabled = false; bool mmcss = false; };

namespace detail {

// M1 deliberately supports one group with at most 32 logical processors.
constexpr size_t MaxCpuSets = 32;

struct CpuInfo {
  uint32_t id = 0;
  uint16_t group = 0;
  uint8_t logical = 0;
  uint8_t core = 0;
  uint8_t efficiency = 0;
  bool parked = false;
  bool allocated = false;
  bool allocatedToProcess = false;
};

struct Topology { CpuInfo cpus[MaxCpuSets]{}; size_t count = 0; };
struct Limits {
  uint64_t processMask = 0;
  uint64_t threadMask = 0;
  uint16_t threadGroup = 0;
  uint16_t groupCount = 0;
  uint32_t activeProcessors = 0;
};
struct CpuSetList { uint32_t ids[MaxCpuSets]{}; size_t count = 0; };

inline uint32_t Read32(const uint8_t* bytes) noexcept {
  return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8u)
    | (uint32_t(bytes[2]) << 16u) | (uint32_t(bytes[3]) << 24u);
}

// Decode the documented SYSTEM_CPU_SET_INFORMATION wire layout bytewise, so
// unknown records and unaligned input do not require unsafe structure casts.
inline bool ParseCpuSetInformation(const void* data, size_t bytes, Topology& result) noexcept {
  result = {};
  Topology parsed{};
  if (!data && bytes) return false;
  const auto* cursor = static_cast<const uint8_t*>(data);
  while (bytes) {
    if (bytes < 8u) return false;
    const uint32_t size = Read32(cursor);
    const uint32_t type = Read32(cursor + 4u);
    if (size < 8u || size > bytes) return false;
    if (type == 0u) { // CpuSetInformation
      if (size < 32u || parsed.count == MaxCpuSets) return false;
      CpuInfo cpu{};
      cpu.id = Read32(cursor + 8u);
      cpu.group = uint16_t(cursor[12]) | uint16_t(uint16_t(cursor[13]) << 8u);
      cpu.logical = cursor[14];
      cpu.core = cursor[15];
      cpu.efficiency = cursor[18];
      cpu.parked = (cursor[19] & 1u) != 0;
      cpu.allocated = (cursor[19] & 2u) != 0;
      cpu.allocatedToProcess = (cursor[19] & 4u) != 0;
      for (size_t i = 0; i < parsed.count; ++i) {
        const auto& previous = parsed.cpus[i];
        if (previous.id == cpu.id || (previous.group == cpu.group && previous.logical == cpu.logical))
          return false;
      }
      parsed.cpus[parsed.count++] = cpu;
    }
    cursor += size;
    bytes -= size;
  }
  result = parsed;
  return true;
}

inline CpuSetList SelectCpuSets(const Topology& topology, const Limits& limits, Role role) noexcept {
  CpuSetList result{};
  if (!topology.count || topology.count > MaxCpuSets || limits.groupCount != 1u
   || limits.threadGroup != 0u || !limits.activeProcessors || limits.activeProcessors > 32u)
    return result;
  if (role != Role::DeviceOwner && role != Role::CommandStream && role != Role::Background)
    return result;

  uint8_t maxEfficiency = 0;
  for (size_t i = 0; i < topology.count; ++i) {
    const auto& cpu = topology.cpus[i];
    if (cpu.group != 0u || cpu.logical >= 32u) return {};
    if (cpu.efficiency > maxEfficiency) maxEfficiency = cpu.efficiency;
  }

  const uint64_t allowed = limits.processMask & limits.threadMask;
  CpuInfo cores[MaxCpuSets]{};
  size_t count = 0;
  for (size_t i = 0; i < topology.count; ++i) {
    const auto& cpu = topology.cpus[i];
    const bool highPerformance = cpu.efficiency == maxEfficiency;
    if (cpu.parked || (cpu.allocated && !cpu.allocatedToProcess)
     || !(allowed & (uint64_t(1) << cpu.logical))
     || (role == Role::Background ? highPerformance : !highPerformance))
      continue;
    size_t core = 0;
    while (core < count && cores[core].core != cpu.core) ++core;
    if (core == count) cores[count++] = cpu;
    else if (cpu.logical < cores[core].logical) cores[core] = cpu;
  }
  // Stable physical-core order, independent of the OS CPU-set record ordering.
  for (size_t i = 1; i < count; ++i) {
    const auto value = cores[i];
    size_t at = i;
    while (at && cores[at - 1].core > value.core) { cores[at] = cores[at - 1]; --at; }
    cores[at] = value;
  }
  if (role == Role::Background) {
    for (size_t i = 0; i < count; ++i) result.ids[result.count++] = cores[i].id;
  } else {
    const size_t ordinal = role == Role::DeviceOwner ? 0u : 1u;
    if (count > ordinal) result.ids[result.count++] = cores[ordinal].id;
  }
  return result;
}

inline bool SameCpuSets(const CpuSetList& a, const CpuSetList& b) noexcept {
  if (a.count != b.count || a.count > MaxCpuSets) return false;
  // Compare membership, accepting an OS readback with a different ordering.
  bool matched[MaxCpuSets]{};
  for (size_t i = 0; i < a.count; ++i) {
    size_t j = 0;
    while (j < b.count && (matched[j] || a.ids[i] != b.ids[j])) ++j;
    if (j == b.count) return false;
    matched[j] = true;
  }
  return true;
}

inline void ParseAffinityFlags(const char* text, Options* out) noexcept {
  if (!out || !text) return;
  if (std::strstr(text, "PerThread=1")) out->enabled = true;
  if (std::strstr(text, "Mmcss=1")) out->mmcss = true;
}

inline bool ResolveConfigPath(wchar_t* out, size_t capacity) noexcept {
  if (!out || !capacity) return false;
  out[0] = L'\0';
#ifdef _WIN32
  wchar_t module[MAX_PATH]{};
  const DWORD length = GetModuleFileNameW(nullptr, module, MAX_PATH);
  if (!length || length >= MAX_PATH) return false;
  wchar_t* slash = wcsrchr(module, L'\\');
  if (!slash) slash = wcsrchr(module, L'/');
  if (!slash) return false;
  *slash = L'\0';
  const wchar_t* names[] = { L"SA.RenderStack.ini", L"scripts\\BridgeD3D9.ini" };
  for (const auto* name : names) {
    wchar_t path[MAX_PATH * 2]{};
    if (swprintf(path, sizeof(path) / sizeof(path[0]), L"%ls\\%ls", module, name) <= 0) continue;
    DWORD attrs = GetFileAttributesW(path);
    if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
      if (wcslen(path) + 1 > capacity) return false;
      wcscpy(out, path); return true;
    }
  }
#endif
  return false;
}

inline Options ReadOptions() noexcept {
  Options result{};
#ifdef _WIN32
  wchar_t path[MAX_PATH * 2]{};
  if (!ResolveConfigPath(path, sizeof(path) / sizeof(path[0]))) return result;
  const int perThread = GetPrivateProfileIntW(L"Affinity", L"PerThread", 0, path);
  const int mmcss = GetPrivateProfileIntW(L"Affinity", L"Mmcss", 0, path);
  result.enabled = perThread != 0; result.mmcss = mmcss != 0;
#endif
  return result;
}

// Explicit platform operations make policy and lifecycle testable without
// changing the scheduling of the test runner or requiring MMCSS/GPU services.
struct Operations {
  void* context = nullptr;
  bool (*topology)(void*, Topology&, Limits&) noexcept = nullptr;
  bool (*readSelected)(void*, CpuSetList&) noexcept = nullptr;
  bool (*writeSelected)(void*, const CpuSetList&) noexcept = nullptr;
  uint32_t (*threadId)(void*) noexcept = nullptr;
  void* (*registerMmcss)(void*) noexcept = nullptr;
  void (*revertMmcss)(void*, void*) noexcept = nullptr;
};

class SchedulingSession final {
public:
  SchedulingSession(Options options, Role role, Operations operations, void (*log)(const char*)) noexcept
  : m_operations(operations), m_log(log) {
    if (!options.enabled || !m_operations.threadId) return;
    m_owner = m_operations.threadId(m_operations.context);
    if (!m_owner) return;

    Topology topology{};
    Limits limits{};
    if (m_operations.topology && m_operations.readSelected && m_operations.writeSelected
     && m_operations.topology(m_operations.context, topology, limits)) {
      m_applied = SelectCpuSets(topology, limits, role);
      if (m_applied.count && m_operations.readSelected(m_operations.context, m_previous)
       && m_previous.count <= MaxCpuSets && !SameCpuSets(m_applied, m_previous)) {
        m_changed = m_operations.writeSelected(m_operations.context, m_applied);
        Log(m_changed ? "thread scheduling: CPU Sets applied" : "thread scheduling: CPU Sets unavailable; unchanged");
      } else {
        Log("thread scheduling: no eligible CPU Sets change");
      }
    } else {
      Log("thread scheduling: CPU topology unavailable; unchanged");
    }

    if (options.mmcss && role == Role::CommandStream && m_operations.registerMmcss && m_operations.revertMmcss) {
      m_mmcss = m_operations.registerMmcss(m_operations.context);
      Log(m_mmcss ? "thread scheduling: MMCSS Games/Normal registered" : "thread scheduling: MMCSS unavailable; SKIP");
    }
  }

  ~SchedulingSession() noexcept {
    if (!m_owner) return;
    if (m_operations.threadId(m_operations.context) != m_owner) {
      Log("thread scheduling: cleanup requires the owner thread; skipped");
      return;
    }
    if (m_mmcss) m_operations.revertMmcss(m_operations.context, m_mmcss);
    if (m_changed) {
      CpuSetList current{};
      if (m_operations.readSelected(m_operations.context, current) && SameCpuSets(current, m_applied)) {
        const bool restored = m_operations.writeSelected(m_operations.context, m_previous);
        Log(restored ? "thread scheduling: previous CPU Sets restored" : "thread scheduling: CPU Sets restore unavailable");
      } else {
        Log("thread scheduling: CPU Sets changed externally; preserved");
      }
    }
  }

  SchedulingSession(const SchedulingSession&) = delete;
  SchedulingSession& operator=(const SchedulingSession&) = delete;
  SchedulingSession(SchedulingSession&&) = delete;
  SchedulingSession& operator=(SchedulingSession&&) = delete;

private:
  void Log(const char* message) noexcept {
    if (m_log) { try { m_log(message); } catch (...) { } }
  }
  Operations m_operations;
  void (*m_log)(const char*) = nullptr;
  CpuSetList m_previous{}, m_applied{};
  uint32_t m_owner = 0;
  void* m_mmcss = nullptr;
  bool m_changed = false;
};

} // namespace detail
inline void ParseAffinityFlags(const char* text, Options* out) noexcept { detail::ParseAffinityFlags(text, out); }
inline bool ResolveConfigPath(wchar_t* out, size_t capacity) noexcept { return detail::ResolveConfigPath(out, capacity); }
inline Options ReadOptions() noexcept { return detail::ReadOptions(); }
} // namespace renderstack::scheduling
