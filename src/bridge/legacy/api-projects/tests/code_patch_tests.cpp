#include "test_check.hpp"
#include "BridgeCodePatch.h"

#include <array>
#include <cstdint>
#include <cstring>

namespace {
using BridgeCodePatch::CodePatch;
using BridgeCodePatch::NativeOps;

// Every successful operation below still calls the real Win32 API. Only the
// otherwise inaccessible failure is injected; code execution and page state
// are always observed from real VirtualAlloc memory.
struct Faults {
  inline static unsigned protectCalls = 0;
  inline static unsigned flushCalls = 0;
  inline static unsigned failProtect = 0;
  inline static unsigned failFlush = 0;
  static void Reset(unsigned protect = 0, unsigned flush = 0) {
    protectCalls = flushCalls = 0;
    failProtect = protect;
    failFlush = flush;
  }
  static BOOL WINAPI Protect(LPVOID site, SIZE_T length, DWORD protection, PDWORD old) {
    if (++protectCalls == failProtect) {
      SetLastError(ERROR_ACCESS_DENIED);
      return FALSE;
    }
    return VirtualProtect(site, length, protection, old);
  }
  static BOOL WINAPI Flush(HANDLE process, LPCVOID site, SIZE_T length) {
    if (++flushCalls == failFlush) {
      SetLastError(ERROR_WRITE_FAULT);
      return FALSE;
    }
    return FlushInstructionCache(process, site, length);
  }
  static NativeOps Ops() { return {&Protect, &Flush}; }
};

struct Executable {
  uint8_t* site = nullptr;
  std::array<uint8_t, 16> original{};
  std::array<uint8_t, 16> replacement{};
  Executable() {
    site = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096,
      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    CHECK(site != nullptr);
    if (!site) return;
    std::memset(site, 0x90, 4096);
    const uint8_t before[] = {0xB8, 7, 0, 0, 0, 0xC3};
    const uint8_t target[] = {0xB8, 42, 0, 0, 0, 0xC3};
    std::memcpy(site, before, sizeof(before));
    std::memcpy(site + 64, target, sizeof(target));
    std::memcpy(original.data(), site, original.size());
    replacement.fill(0x90); // Caller owns NOP padding and instruction shape.
    replacement[0] = 0xE9;
    const int32_t relative = 64 - 5;
    std::memcpy(replacement.data() + 1, &relative, sizeof(relative));
    DWORD old = 0;
    CHECK(VirtualProtect(site, 4096, PAGE_EXECUTE_READ, &old) != FALSE);
    CHECK(FlushInstructionCache(GetCurrentProcess(), site, 4096) != FALSE);
  }
  ~Executable() { if (site) VirtualFree(site, 0, MEM_RELEASE); }
  int Invoke() const { return reinterpret_cast<int (*)()>(site)(); }
  DWORD Protection() const {
    MEMORY_BASIC_INFORMATION memory{};
    CHECK(VirtualQuery(site, &memory, sizeof(memory)) == sizeof(memory));
    return memory.Protect;
  }
  void Write(const void* bytes, size_t length) {
    DWORD old = 0;
    CHECK(VirtualProtect(site, length, PAGE_EXECUTE_READWRITE, &old) != FALSE);
    std::memcpy(site, bytes, length);
    CHECK(FlushInstructionCache(GetCurrentProcess(), site, length) != FALSE);
    DWORD ignored = 0;
    CHECK(VirtualProtect(site, length, old, &ignored) != FALSE);
  }
};

void NormalInstructionLengths() {
  for (size_t length : {size_t(5), size_t(6), size_t(7), size_t(16)}) {
    Executable memory;
    if (!memory.site) continue;
    CodePatch patch;
    CHECK(memory.Invoke() == 7);
    CHECK(patch.Install(memory.site, memory.original.data(), memory.replacement.data(), length));
    CHECK(patch.OwnsBytes());
    CHECK(!patch.HasPendingProtection());
    CHECK(patch.Error() == ERROR_SUCCESS);
    CHECK(memory.Invoke() == 42);
    CHECK(memory.Protection() == PAGE_EXECUTE_READ);
    CHECK(std::memcmp(memory.site, memory.replacement.data(), length) == 0);
    CHECK(patch.Restore());
    CHECK(!patch.OwnsBytes());
    CHECK(!patch.HasPendingProtection());
    CHECK(memory.Invoke() == 7);
    CHECK(memory.Protection() == PAGE_EXECUTE_READ);
    CHECK(std::memcmp(memory.site, memory.original.data(), memory.original.size()) == 0);
    CHECK(patch.Restore());
  }
}

void InvalidInputAndSignature() {
  Executable memory;
  if (!memory.site) return;
  CodePatch patch;
  for (size_t length : {size_t(0), size_t(17)}) {
    CHECK(!patch.Install(memory.site, memory.original.data(), memory.replacement.data(), length));
    CHECK(patch.Error() == ERROR_INVALID_PARAMETER);
    CHECK(!patch.OwnsBytes());
    CHECK(!patch.HasPendingProtection());
    CHECK(memory.Invoke() == 7);
  }
  CHECK(!patch.Install(nullptr, memory.original.data(), memory.replacement.data(), 6));
  CHECK(!patch.Install(memory.site, nullptr, memory.replacement.data(), 6));
  CHECK(!patch.Install(memory.site, memory.original.data(), nullptr, 6));
  auto unexpected = memory.original;
  unexpected[0] = 0xCC;
  CHECK(!patch.Install(memory.site, unexpected.data(), memory.replacement.data(), 6));
  CHECK(patch.Error() == ERROR_REVISION_MISMATCH);
  CHECK(!patch.OwnsBytes());
  CHECK(memory.Invoke() == 7);
  CHECK(memory.Protection() == PAGE_EXECUTE_READ);
}

void RejectsMixedProtectionRange() {
  auto* pages = static_cast<uint8_t*>(VirtualAlloc(nullptr, 8192,
    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  CHECK(pages != nullptr);
  if (!pages) return;
  std::memset(pages, 0x90, 8192);
  DWORD old = 0;
  CHECK(VirtualProtect(pages, 4096, PAGE_EXECUTE_READ, &old) != FALSE);
  CHECK(VirtualProtect(pages + 4096, 4096, PAGE_READONLY, &old) != FALSE);
  const uint8_t expected[6] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
  const uint8_t replacement[6] = {0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
  CodePatch patch;
  CHECK(!patch.Install(pages + 4093, expected, replacement, sizeof(expected)));
  CHECK(patch.Error() == ERROR_INVALID_ADDRESS);
  CHECK(!patch.OwnsBytes());
  CHECK(std::memcmp(pages + 4093, expected, sizeof(expected)) == 0);
  MEMORY_BASIC_INFORMATION memory{};
  CHECK(VirtualQuery(pages, &memory, sizeof(memory)) == sizeof(memory));
  CHECK(memory.Protect == PAGE_EXECUTE_READ);
  CHECK(VirtualQuery(pages + 4096, &memory, sizeof(memory)) == sizeof(memory));
  CHECK(memory.Protect == PAGE_READONLY);
  CHECK(VirtualFree(pages, 0, MEM_RELEASE) != FALSE);
}

void RefusesOtherOwners() {
  Executable memory;
  if (!memory.site) return;
  CodePatch patch;
  CHECK(patch.Install(memory.site, memory.original.data(), memory.replacement.data(), 7));
  CHECK(!patch.Install(memory.site, memory.original.data(), memory.replacement.data(), 7));
  CHECK(patch.Error() == ERROR_BUSY);
  CodePatch other;
  CHECK(!other.Install(memory.site, memory.original.data(), memory.replacement.data(), 7));
  CHECK(!other.OwnsBytes());
  CHECK(other.Restore());
  CHECK(memory.Invoke() == 42);
  const uint8_t foreign[] = {0xB8, 99, 0, 0, 0, 0xC3, 0x90};
  memory.Write(foreign, sizeof(foreign));
  CHECK(!patch.Restore());
  CHECK(patch.Error() == ERROR_REVISION_MISMATCH);
  CHECK(patch.OwnsBytes());
  CHECK(memory.Invoke() == 99);
  CHECK(std::memcmp(memory.site, foreign, sizeof(foreign)) == 0);
  memory.Write(memory.replacement.data(), 7);
  CHECK(patch.Restore());
  CHECK(memory.Invoke() == 7);
}

void InitialProtectFailureDoesNotClaimBytes() {
  Executable memory;
  if (!memory.site) return;
  Faults::Reset(1);
  CodePatch patch(Faults::Ops());
  CHECK(!patch.Install(memory.site, memory.original.data(), memory.replacement.data(), 6));
  CHECK(patch.Error() == ERROR_ACCESS_DENIED);
  CHECK(!patch.OwnsBytes());
  CHECK(!patch.HasPendingProtection());
  CHECK(memory.Invoke() == 7);
  CHECK(memory.Protection() == PAGE_EXECUTE_READ);
  Faults::Reset();
  CHECK(patch.Install(memory.site, memory.original.data(), memory.replacement.data(), 6));
  CHECK(memory.Invoke() == 42);
  CHECK(patch.Restore());
}

void InstallProtectionFailureRetainsRollback() {
  Executable memory;
  if (!memory.site) return;
  Faults::Reset(2);
  CodePatch patch(Faults::Ops());
  CHECK(!patch.Install(memory.site, memory.original.data(), memory.replacement.data(), 7));
  CHECK(patch.Error() == ERROR_ACCESS_DENIED);
  CHECK(patch.OwnsBytes());
  CHECK(patch.HasPendingProtection());
  CHECK(memory.Invoke() == 42);
  CHECK(memory.Protection() == PAGE_EXECUTE_READWRITE);
  Faults::Reset();
  CHECK(patch.Restore());
  CHECK(!patch.OwnsBytes());
  CHECK(!patch.HasPendingProtection());
  CHECK(memory.Invoke() == 7);
  CHECK(memory.Protection() == PAGE_EXECUTE_READ);
}

void RestoreFailuresAreRetryable() {
  for (unsigned failedProtect : {1u, 2u}) {
    Executable memory;
    if (!memory.site) continue;
    Faults::Reset();
    CodePatch patch(Faults::Ops());
    CHECK(patch.Install(memory.site, memory.original.data(), memory.replacement.data(), 6));
    Faults::Reset(failedProtect);
    CHECK(!patch.Restore());
    CHECK(patch.Error() == ERROR_ACCESS_DENIED);
    CHECK(patch.OwnsBytes());
    CHECK(patch.HasPendingProtection() == (failedProtect == 2));
    CHECK(memory.Invoke() == (failedProtect == 1 ? 42 : 7));
    Faults::Reset();
    CHECK(patch.Restore());
    CHECK(!patch.OwnsBytes());
    CHECK(!patch.HasPendingProtection());
    CHECK(memory.Invoke() == 7);
    CHECK(memory.Protection() == PAGE_EXECUTE_READ);
  }
}

void CacheFailuresAreRetryable() {
  for (bool failInstall : {true, false}) {
    Executable memory;
    if (!memory.site) continue;
    Faults::Reset(0, failInstall ? 1u : 0u);
    CodePatch patch(Faults::Ops());
    const bool installed = patch.Install(memory.site, memory.original.data(), memory.replacement.data(), 5);
    CHECK(installed == !failInstall);
    CHECK(patch.OwnsBytes());
    if (failInstall) {
      CHECK(patch.Error() == ERROR_WRITE_FAULT);
    } else {
      Faults::Reset(0, 1);
      CHECK(!patch.Restore());
      CHECK(patch.Error() == ERROR_WRITE_FAULT);
      CHECK(patch.OwnsBytes());
      CHECK(memory.Invoke() == 7);
    }
    CHECK(!patch.HasPendingProtection());
    CHECK(memory.Protection() == PAGE_EXECUTE_READ);
    Faults::Reset();
    CHECK(patch.Restore());
    CHECK(!patch.OwnsBytes());
    CHECK(memory.Invoke() == 7);
    CHECK(memory.Protection() == PAGE_EXECUTE_READ);
  }
}

void PendingCleanupRefusesForeignBytes() {
  Executable memory;
  if (!memory.site) return;
  Faults::Reset();
  CodePatch patch(Faults::Ops());
  CHECK(patch.Install(memory.site, memory.original.data(), memory.replacement.data(), 6));
  Faults::Reset(2);
  CHECK(!patch.Restore());
  const uint8_t foreign[] = {0xB8, 99, 0, 0, 0, 0xC3};
  memory.Write(foreign, sizeof(foreign));
  Faults::Reset();
  CHECK(!patch.Restore());
  CHECK(patch.Error() == ERROR_REVISION_MISMATCH);
  CHECK(patch.OwnsBytes());
  CHECK(patch.HasPendingProtection());
  CHECK(memory.Invoke() == 99);
  memory.Write(memory.original.data(), 6);
  CHECK(patch.Restore());
  CHECK(!patch.OwnsBytes());
  CHECK(memory.Invoke() == 7);
  CHECK(memory.Protection() == PAGE_EXECUTE_READ);
}
} // namespace

int main() {
  NormalInstructionLengths();
  InvalidInputAndSignature();
  RejectsMixedProtectionRange();
  RefusesOtherOwners();
  InitialProtectFailureDoesNotClaimBytes();
  InstallProtectionFailureRetainsRollback();
  RestoreFailuresAreRetryable();
  CacheFailuresAreRetryable();
  PendingCleanupRefusesForeignBytes();
  return api_test::finish();
}
