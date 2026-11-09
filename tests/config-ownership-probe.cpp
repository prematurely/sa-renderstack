#include <windows.h>
#include <d3d9.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

struct ProcessPolicy {
  DWORD priority = 0;
  DWORD_PTR processMask = 0;
  DWORD_PTR systemMask = 0;
};

static bool ReadPolicy(ProcessPolicy& policy) {
  policy.priority = GetPriorityClass(GetCurrentProcess());
  return policy.priority != 0 && GetProcessAffinityMask(
    GetCurrentProcess(), &policy.processMask, &policy.systemMask);
}

int main(int argc, char** argv) {
  ProcessPolicy initial;
  if (!ReadPolicy(initial)) {
    std::fprintf(stderr, "FAIL read initial policy error=%lu\n", GetLastError());
    return 2;
  }
  if (argc == 2 && std::strcmp(argv[1], "--describe") == 0) {
    const DWORD_PTR firstCpu = initial.systemMask & (~initial.systemMask + 1);
    std::printf("first_cpu=0x%llX system_mask=0x%llX\n",
      static_cast<unsigned long long>(firstCpu),
      static_cast<unsigned long long>(initial.systemMask));
    return 0;
  }
  if (argc != 4 && argc != 5) {
    std::fprintf(stderr, "usage: probe DLL Direct3DCreate9|Direct3DCreate9Ex preserve|bridge-policy [mask]\n");
    return 2;
  }
  const bool extended = std::strcmp(argv[2], "Direct3DCreate9Ex") == 0;
  const bool bridgePolicy = std::strcmp(argv[3], "bridge-policy") == 0;
  if ((!extended && std::strcmp(argv[2], "Direct3DCreate9") != 0) ||
      (!bridgePolicy && std::strcmp(argv[3], "preserve") != 0) ||
      (bridgePolicy && argc != 5)) return 2;

  // A distinct initial priority and the full available mask expose an implicit
  // backend override, including the old 0xFFFF pin on machines with >16 CPUs.
  if (!SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS) ||
      !SetProcessAffinityMask(GetCurrentProcess(), initial.systemMask)) {
    std::fprintf(stderr, "FAIL seed child policy error=%lu\n", GetLastError());
    return 2;
  }
  ProcessPolicy before;
  if (!ReadPolicy(before)) return 2;
  const DWORD expectedPriority = bridgePolicy ? NORMAL_PRIORITY_CLASS : before.priority;
  const DWORD_PTR expectedMask = bridgePolicy
    ? static_cast<DWORD_PTR>(std::strtoull(argv[4], nullptr, 0))
    : before.processMask;

  HMODULE module = LoadLibraryA(argv[1]);
  if (!module) {
    std::fprintf(stderr, "FAIL LoadLibrary error=%lu path=%s\n", GetLastError(), argv[1]);
    return 2;
  }
  FARPROC factory = GetProcAddress(module, argv[2]);
  if (!factory) {
    std::fprintf(stderr, "FAIL factory missing: %s\n", argv[2]);
    return 2;
  }
  int failures = 0;
  for (unsigned call = 1; call <= 2; call++) {
    IDirect3D9* d3d9 = nullptr;
    if (extended) {
      IDirect3D9Ex* d3d9ex = nullptr;
      const HRESULT hr = reinterpret_cast<HRESULT (WINAPI*)(UINT, IDirect3D9Ex**)>(factory)(
        D3D_SDK_VERSION, &d3d9ex);
      if (FAILED(hr)) {
        std::fprintf(stderr, "FAIL factory HRESULT=0x%08lX\n", static_cast<unsigned long>(hr));
        return 2;
      }
      d3d9 = d3d9ex;
    } else {
      d3d9 = reinterpret_cast<IDirect3D9* (WINAPI*)(UINT)>(factory)(D3D_SDK_VERSION);
    }
    if (!d3d9) {
      std::fprintf(stderr, "FAIL factory returned null\n");
      return 2;
    }
    ProcessPolicy after;
    if (!ReadPolicy(after)) return 2;
    const bool passed = after.priority == expectedPriority && after.processMask == expectedMask;
    std::printf("%s %s call=%u before_priority=0x%lX after_priority=0x%lX expected_priority=0x%lX before_mask=0x%llX after_mask=0x%llX expected_mask=0x%llX\n",
      passed ? "PASS" : "FAIL", argv[2], call, before.priority, after.priority, expectedPriority,
      static_cast<unsigned long long>(before.processMask),
      static_cast<unsigned long long>(after.processMask),
      static_cast<unsigned long long>(expectedMask));
    failures += passed ? 0 : 1;
    d3d9->Release();
  }
  // The entire process is the isolation boundary; leaving the loaded module in
  // place avoids making DLL unload semantics part of this configuration test.
  return failures ? 1 : 0;
}
