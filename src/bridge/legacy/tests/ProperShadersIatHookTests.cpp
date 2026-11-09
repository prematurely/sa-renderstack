#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ProperShadersIatHook.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <thread>

namespace {
using ProperShadersPatching::IatHook;
using Target = int (WINAPI*)(int);
std::atomic<IatHook*> activeHook{nullptr};
int WINAPI Original(int value) { return value + 17; }
int WINAPI OtherOriginal(int value) { return value + 29; }
int WINAPI ForeignHook(int value) { return value + 41; }
HANDLE originalEntered = nullptr;
HANDLE originalResume = nullptr;
int WINAPI BlockingOriginal(int value) {
    SetEvent(originalEntered);
    if (WaitForSingleObject(originalResume, 10000) != WAIT_OBJECT_0) std::abort();
    return value + 17;
}
int WINAPI Hook(int value) {
    const auto original = reinterpret_cast<Target>(activeHook.load()->Original());
    return original ? original(value) : -1000;
}
PVOID Address(Target value) { return reinterpret_cast<PVOID>(value); }

bool Check(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

struct Slot {
    PVOID volatile* value = static_cast<PVOID volatile*>(VirtualAlloc(
        nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    explicit Slot(Target target = &Original) {
        if (!value) std::abort();
        *value = Address(target);
        DWORD previous = 0;
        if (!VirtualProtect(const_cast<PVOID*>(value), 4096, PAGE_READONLY, &previous)) std::abort();
    }
    ~Slot() { VirtualFree(const_cast<PVOID*>(value), 0, MEM_RELEASE); }
    int Call(int valueToPass) const { return reinterpret_cast<Target>(*value)(valueToPass); }
    DWORD Protection() const {
        MEMORY_BASIC_INFORMATION memory{};
        VirtualQuery(const_cast<PVOID*>(value), &memory, sizeof(memory));
        return memory.Protect;
    }
    void Replace(Target target) {
        DWORD previous = 0;
        VirtualProtect(const_cast<PVOID*>(value), 4096, PAGE_READWRITE, &previous);
        InterlockedExchangePointer(value, Address(target));
        DWORD ignored = 0;
        VirtualProtect(const_cast<PVOID*>(value), 4096, previous, &ignored);
    }
};

struct ReadOnlyMappedSlot {
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE mapping = nullptr;
    PVOID volatile* value = nullptr;
    ReadOnlyMappedSlot() {
        wchar_t directory[MAX_PATH]{};
        wchar_t path[MAX_PATH]{};
        if (!GetTempPathW(MAX_PATH, directory) || !GetTempFileNameW(directory, L"iat", 0, path)) std::abort();
        file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (file == INVALID_HANDLE_VALUE) std::abort();
        PVOID target = Address(&Original);
        DWORD written = 0;
        LARGE_INTEGER fileSize{};
        fileSize.QuadPart = 4096;
        if (!WriteFile(file, &target, sizeof(target), &written, nullptr) || written != sizeof(target) ||
            !SetFilePointerEx(file, fileSize, nullptr, FILE_BEGIN) || !SetEndOfFile(file)) std::abort();
        mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping) std::abort();
        value = static_cast<PVOID volatile*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
        if (!value) std::abort();
    }
    ~ReadOnlyMappedSlot() {
        UnmapViewOfFile(const_cast<PVOID*>(value));
        CloseHandle(mapping);
        CloseHandle(file);
    }
};

// Protection rollback failure cannot be induced deterministically on a valid,
// committed private page without racing its lifetime. Only that Win32 operation
// is substituted; the slot, slot CAS, protection changes and target calls are real.
struct ProtectControl {
    static inline ProtectControl* current = nullptr;
    std::atomic<unsigned> calls{0};
    unsigned failAt = 0;
    unsigned pauseAt = 0;
    Target changeAtFirstProtect = nullptr;
    HANDLE paused = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE resume = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ProtectControl() { current = this; }
    ~ProtectControl() { CloseHandle(paused); CloseHandle(resume); current = nullptr; }
    static BOOL WINAPI Protect(LPVOID address, SIZE_T size, DWORD protection, PDWORD oldProtection) {
        auto& self = *current;
        const unsigned call = ++self.calls;
        if (call == self.pauseAt) {
            SetEvent(self.paused);
            if (WaitForSingleObject(self.resume, 10000) != WAIT_OBJECT_0) std::abort();
        }
        if (call == self.failAt) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
        const BOOL result = VirtualProtect(address, size, protection, oldProtection);
        if (result && call == 1 && self.changeAtFirstProtect) {
            InterlockedExchangePointer(static_cast<PVOID volatile*>(address), Address(self.changeAtFirstProtect));
        }
        return result;
    }
};

bool PublicationWindow() {
    Slot slot;
    ProtectControl control;
    control.pauseAt = 2;
    IatHook hook(&ProtectControl::Protect);
    activeHook.store(&hook);
    bool installed = false;
    std::thread installer([&] { installed = hook.Install(slot.value, Address(&Original), Address(&Hook)); });
    const bool paused = WaitForSingleObject(control.paused, 10000) == WAIT_OBJECT_0;
    bool ok = Check(paused, "installer reaches deterministic post-CAS barrier");
    if (paused) {
        ok &= Check(*slot.value == Address(&Hook), "hook is visible at publication barrier");
        // This fails if original publication moves behind the IAT CAS.
        ok &= Check(slot.Call(5) == 22, "concurrent IAT call immediately after CAS reaches original");
    }
    SetEvent(control.resume);
    installer.join();
    ok &= Check(installed, "installation completes");
    ok &= Check(slot.Protection() == PAGE_READONLY, "successful install preserves IAT page protection");
    ok &= Check(hook.Restore(), "restore succeeds");
    ok &= Check(slot.Call(7) == 24, "restored slot invokes original");
    // A caller may cache the entry before restore and enter after it completes.
    ok &= Check(Hook(11) == 28, "cached hook entry remains callable after restore");
    return ok;
}

bool SignatureConflict() {
    Slot slot(&ForeignHook);
    IatHook hook;
    activeHook.store(&hook);
    bool ok = Check(!hook.Install(slot.value, Address(&Original), Address(&Hook)), "unexpected import signature is rejected");
    ok &= Check(slot.Call(1) == 42, "signature rejection preserves foreign owner");
    ok &= Check(!hook.OwnsSlot(), "signature rejection takes no ownership");
    hook.Restore();
    return ok;
}

bool InstallOwnershipRace() {
    Slot slot;
    ProtectControl control;
    control.changeAtFirstProtect = &ForeignHook;
    IatHook hook(&ProtectControl::Protect);
    activeHook.store(&hook);
    const bool installed = hook.Install(slot.value, Address(&Original), Address(&Hook));
    bool ok = Check(!installed, "CAS rejects owner change after signature validation");
    ok &= Check(slot.Call(2) == 43, "CAS conflict preserves intervening owner");
    ok &= Check(slot.Protection() == PAGE_READONLY, "CAS conflict restores page protection");
    ok &= Check(!hook.OwnsSlot(), "CAS conflict does not claim publication");
    hook.Restore();
    return ok;
}

bool RealProtectionFailure() {
    ReadOnlyMappedSlot slot;
    IatHook hook;
    activeHook.store(&hook);
    bool ok = Check(!hook.Install(slot.value, Address(&Original), Address(&Hook)), "real read-only section refuses writable IAT installation");
    ok &= Check(reinterpret_cast<Target>(*slot.value)(4) == 21, "real protection failure preserves callable import");
    ok &= Check(!hook.OwnsSlot() && !hook.HasPendingProtection() && !hook.Original(), "real protection failure retains no publication or protection debt");
    ok &= Check(hook.Restore(), "restore after real pre-publication failure is harmless");
    return ok;
}

bool RestoreDuringOriginalCall() {
    Slot slot(&BlockingOriginal);
    IatHook hook;
    activeHook.store(&hook);
    originalEntered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    originalResume = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool ok = Check(hook.Install(slot.value, Address(&BlockingOriginal), Address(&Hook)), "active-call setup installs hook");
    int result = 0;
    std::thread caller([&] { result = slot.Call(10); });
    ok &= Check(WaitForSingleObject(originalEntered, 10000) == WAIT_OBJECT_0, "real IAT callback is inside original target");
    ok &= Check(hook.Restore() && *slot.value == Address(&BlockingOriginal), "restore detaches while an original call is active");
    SetEvent(originalResume);
    caller.join();
    ok &= Check(result == 27, "in-flight original call completes after slot restoration");
    ok &= Check(Hook(12) == 29, "stale callback remains callable after active call drains");
    CloseHandle(originalEntered);
    CloseHandle(originalResume);
    originalEntered = originalResume = nullptr;
    return ok;
}

bool InstallProtectionRollbackFailure() {
    Slot slot;
    ProtectControl control;
    control.failAt = 2;
    IatHook hook(&ProtectControl::Protect);
    activeHook.store(&hook);
    bool ok = Check(!hook.Install(slot.value, Address(&Original), Address(&Hook)), "install reports failed protection rollback");
    ok &= Check(hook.OwnsSlot() && hook.HasPendingProtection(), "failed install accurately retains published hook and protection debt");
    ok &= Check(slot.Call(4) == 21, "published hook calls original while protection rollback is pending");
    control.failAt = 0;
    ok &= Check(hook.Restore(), "failed-install restoration can retry");
    ok &= Check(slot.Call(4) == 21 && slot.Protection() == PAGE_READONLY, "failed-install retry restores target and original page protection");
    ok &= Check(!hook.OwnsSlot() && !hook.HasPendingProtection(), "failed-install retry clears only repaired tracking");
    ok &= Check(Hook(6) == 23, "failed-install retry retains original for stale hook calls");
    return ok;
}

bool RestoreProtectionFailure() {
    Slot slot;
    ProtectControl control;
    IatHook hook(&ProtectControl::Protect);
    activeHook.store(&hook);
    bool ok = Check(hook.Install(slot.value, Address(&Original), Address(&Hook)), "restore-failure setup installs hook");
    control.failAt = control.calls + 1;
    ok &= Check(!hook.Restore(), "restore reports write-protection failure");
    ok &= Check(hook.OwnsSlot() && slot.Call(5) == 22, "failed restore retains callable original and published-slot tracking");
    control.failAt = 0;
    ok &= Check(hook.Restore(), "restore retries after write-protection failure");
    ok &= Check(slot.Call(5) == 22 && *slot.value == Address(&Original), "restore retry repairs original IAT target");
    return ok;
}

bool RestoreProtectionRollbackFailure() {
    Slot slot;
    ProtectControl control;
    IatHook hook(&ProtectControl::Protect);
    activeHook.store(&hook);
    bool ok = Check(hook.Install(slot.value, Address(&Original), Address(&Hook)), "restore rollback-failure setup installs hook");
    control.failAt = control.calls + 2;
    ok &= Check(!hook.Restore(), "restore reports protection rollback failure after target CAS");
    ok &= Check(!hook.OwnsSlot() && hook.HasPendingProtection(), "restore rollback failure tracks restored target and remaining protection debt");
    ok &= Check(*slot.value == Address(&Original) && Hook(8) == 25, "restore rollback failure retains stale-entry target");
    control.failAt = 0;
    ok &= Check(hook.Restore(), "restore protection debt retries");
    ok &= Check(slot.Protection() == PAGE_READONLY && !hook.HasPendingProtection(), "restore retry repairs original page protection");
    return ok;
}

bool RestoreOwnershipConflict() {
    Slot slot;
    IatHook hook;
    activeHook.store(&hook);
    bool ok = Check(hook.Install(slot.value, Address(&Original), Address(&Hook)), "ownership-conflict setup installs hook");
    slot.Replace(&ForeignHook);
    ok &= Check(!hook.Restore(), "restore reports foreign slot ownership");
    ok &= Check(slot.Call(9) == 50 && slot.Protection() == PAGE_READONLY, "restore conflict preserves foreign owner and page protection");
    ok &= Check(Hook(9) == 26, "restore ownership conflict retains stale-entry original");
    slot.Replace(&Hook);
    ok &= Check(hook.Restore() && *slot.value == Address(&Original), "restoration can retry when original ownership returns");
    return ok;
}

bool TargetLifetime() {
    Slot slot;
    Slot otherSlot(&OtherOriginal);
    IatHook hook;
    activeHook.store(&hook);
    bool ok = Check(hook.Install(slot.value, Address(&Original), Address(&Hook)) && hook.Restore(), "target-lifetime setup installs and restores");
    ok &= Check(!hook.Install(otherSlot.value, Address(&OtherOriginal), Address(&Hook)), "published hook refuses to overwrite original target for a later module");
    ok &= Check(otherSlot.Call(1) == 30 && Hook(1) == 18, "rejected retarget preserves both new import and stale hook behavior");
    ok &= Check(hook.Install(slot.value, Address(&Original), Address(&Hook)), "same original target can reinstall");
    ok &= Check(slot.Call(1) == 18 && hook.Restore(), "reinstallation remains callable and restorable");
    return ok;
}
}

#ifdef IAT_OLD_ALGORITHM
void IatBaselineAfterPublish() {
    if (!ProtectControl::current) return;
    auto& control = *ProtectControl::current;
    if (control.pauseAt == 2) {
        control.pauseAt = 0;
        SetEvent(control.paused);
        if (WaitForSingleObject(control.resume, 10000) != WAIT_OBJECT_0) std::abort();
    }
}
#endif

int main() {
    const struct { const char* name; bool (*run)(); } tests[] = {
        {"publication_window_and_stale_entry", PublicationWindow},
        {"signature_conflict", SignatureConflict},
        {"install_ownership_race", InstallOwnershipRace},
        {"real_section_protection_failure", RealProtectionFailure},
        {"restore_during_original_call", RestoreDuringOriginalCall},
        {"install_protection_rollback_failure", InstallProtectionRollbackFailure},
        {"restore_protection_failure", RestoreProtectionFailure},
        {"restore_protection_rollback_failure", RestoreProtectionRollbackFailure},
        {"restore_ownership_conflict", RestoreOwnershipConflict},
        {"target_lifetime", TargetLifetime},
    };
    unsigned failed = 0;
    for (const auto& test : tests) {
        const bool passed = test.run();
        std::printf("%s: %s\n", passed ? "PASS" : "FAIL", test.name);
        failed += !passed;
    }
    std::printf("IAT tests: %zu total, %u failed\n", std::size(tests), failed);
    return failed ? 1 : 0;
}
