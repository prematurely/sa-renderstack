#pragma once
#include "util_thread_scheduling_policy.h"

#ifdef _WIN32
#include <windows.h>
#include <array>
#include <cstring>
#endif

namespace renderstack::scheduling {

#ifdef _WIN32
namespace native {
inline bool topology(void*, detail::Topology& out, detail::Limits& limits) noexcept {
  out = {}; limits = {};
  DWORD bytes = 0;
  GetSystemCpuSetInformation(nullptr, 0, &bytes, GetCurrentProcess(), 0);
  if (!bytes || bytes > 8192) return false;
  std::array<std::uint8_t, 8192> buffer{};
  if (!GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()), bytes, &bytes, GetCurrentProcess(), 0)) return false;
  for (DWORD off = 0; off + sizeof(DWORD) <= bytes;) {
    auto* info = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + off);
    if (info->Size < 8 || off + info->Size > bytes) return false;
    if (info->Type == CpuSetInformation && out.count < detail::MaxCpuSets) {
      const auto& c = info->CpuSet;
      out.cpus[out.count++] = { c.Id, c.Group, c.LogicalProcessorIndex, c.CoreIndex, c.EfficiencyClass,
        (c.AllFlags & 1u) != 0, (c.AllFlags & 2u) != 0, (c.AllFlags & 4u) != 0 };
    }
    off += info->Size;
  }
  DWORD_PTR process = 0, system = 0;
  if (!GetProcessAffinityMask(GetCurrentProcess(), &process, &system)) return false;
  GROUP_AFFINITY group{};
  if (!GetThreadGroupAffinity(GetCurrentThread(), &group)) return false;
  SYSTEM_INFO si{}; GetSystemInfo(&si);
  limits.processMask = static_cast<std::uint64_t>(process);
  limits.threadMask = static_cast<std::uint64_t>(group.Mask);
  limits.threadGroup = group.Group;
  limits.groupCount = 1;
  limits.activeProcessors = si.dwNumberOfProcessors;
  return true;
}
inline bool readSelected(void*, detail::CpuSetList& out) noexcept {
  out = {}; ULONG required = 0;
  GetThreadSelectedCpuSets(GetCurrentThread(), nullptr, 0, &required);
  if (!required || required > detail::MaxCpuSets) return required == 0;
  ULONG ids[detail::MaxCpuSets]{};
  if (!GetThreadSelectedCpuSets(GetCurrentThread(), ids, required, &required)) return false;
  out.count = required; std::memcpy(out.ids, ids, required * sizeof(ULONG)); return true;
}
inline bool writeSelected(void*, const detail::CpuSetList& value) noexcept {
  ULONG ids[detail::MaxCpuSets]{};
  for (size_t i = 0; i < value.count && i < detail::MaxCpuSets; ++i)
    ids[i] = static_cast<ULONG>(value.ids[i]);
  return SetThreadSelectedCpuSets(GetCurrentThread(), value.count ? ids : nullptr, static_cast<ULONG>(value.count)) != FALSE;
}
inline std::uint32_t threadId(void*) noexcept { return GetCurrentThreadId(); }
struct MmcssState { HMODULE module = nullptr; HANDLE task = nullptr; };
inline void* registerMmcss(void*) noexcept {
  thread_local MmcssState state{};
  state = {};
  HMODULE avrt = LoadLibraryW(L"avrt.dll"); if (!avrt) return nullptr;
  using Fn = HANDLE (WINAPI*)(LPCWSTR, LPDWORD);
  auto fn = reinterpret_cast<Fn>(GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW"));
  if (!fn) { FreeLibrary(avrt); return nullptr; }
  DWORD index = 0; HANDLE handle = fn(L"Games", &index);
  if (!handle) { FreeLibrary(avrt); return nullptr; }
  state.module = avrt; state.task = handle; return &state;
}
inline void revertMmcss(void*, void* handle) noexcept {
  auto* state = static_cast<MmcssState*>(handle);
  HMODULE avrt = state ? state->module : nullptr;
  using Fn = BOOL (WINAPI*)(HANDLE);
  auto fn = avrt ? reinterpret_cast<Fn>(GetProcAddress(avrt, "AvRevertMmThreadCharacteristics")) : nullptr;
  if (fn && state) fn(state->task);
  if (state && state->module) FreeLibrary(state->module);
  if (state) *state = {};
}
inline detail::Operations operations() noexcept {
  return {nullptr, topology, readSelected, writeSelected, threadId, registerMmcss, revertMmcss};
}
}
#endif

class ThreadSchedulingScope final {
public:
  ThreadSchedulingScope(Options options, Role role, void(*log)(const char*)) noexcept
  : m_log(log), m_session(options, role,
#ifdef _WIN32
      native::operations(),
#else
      detail::Operations{},
#endif
      log) {}
  ThreadSchedulingScope(const ThreadSchedulingScope&) = delete;
  ThreadSchedulingScope& operator=(const ThreadSchedulingScope&) = delete;
  ThreadSchedulingScope(ThreadSchedulingScope&&) = delete;
  ThreadSchedulingScope& operator=(ThreadSchedulingScope&&) = delete;
  ~ThreadSchedulingScope() noexcept = default;
private:
  void(*m_log)(const char*) = nullptr;
  detail::SchedulingSession m_session;
};
}
