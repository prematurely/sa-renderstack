#pragma once

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace ProperShadersPatching
{
// One process-lifetime hook entry and one immutable original target. Lifecycle
// code must keep both modules mapped and serialize hook teardown with its own
// users. A caller can cache the hook before Restore and enter after it returns,
// so a successful restore never clears or retargets Original().
class IatHook
{
public:
    using ProtectMemory = decltype(&::VirtualProtect);

    explicit IatHook(ProtectMemory protect = &::VirtualProtect) noexcept
        : protect_(protect)
    {
    }

    bool Install(PVOID volatile* slot, PVOID expectedOriginal, PVOID hook)
    {
        std::lock_guard lock(mutex_);
        error_ = ERROR_SUCCESS;
        if (!slot || !expectedOriginal || !hook || expectedOriginal == hook ||
            reinterpret_cast<std::uintptr_t>(slot) % alignof(PVOID) != 0) {
            return Fail(ERROR_INVALID_PARAMETER);
        }

        const PVOID retainedOriginal = original_.load(std::memory_order_acquire);
        if (retainedOriginal &&
            (retainedOriginal != expectedOriginal || hook_ != hook)) {
            return Fail(ERROR_REVISION_MISMATCH);
        }
        if (slot_ && slot_ != slot) return Fail(ERROR_BUSY);

        PVOID current = nullptr;
        if (!ReadSlot(slot, current)) return Fail(GetLastError());
        if (slot_ && current == hook && published_) {
            // A previous call may have published the slot but failed to put its
            // original protection back. Report success only after that repair.
            return RestoreProtection();
        }
        if (current != expectedOriginal) return Fail(ERROR_REVISION_MISMATCH);
        if (protectionPending_ && !RestoreProtection()) return false;
        if (!MakeWritable(slot)) return false;

        // This release store precedes the interlocked publication of the hook.
        // The callback takes an acquire snapshot and can immediately call it.
        original_.store(expectedOriginal, std::memory_order_release);
        hook_ = hook;
        const PVOID previous = InterlockedCompareExchangePointer(
            slot, hook, expectedOriginal);
        const bool installed = previous == expectedOriginal;
        if (installed) published_ = true;

        const bool protectedAgain = RestoreProtection();
        if (!installed) {
            // The competing owner wins. Keep a failed protection rollback
            // tracked so Restore can repair it without overwriting that owner.
            if (protectedAgain) slot_ = nullptr;
            return Fail(protectedAgain ? ERROR_REVISION_MISMATCH : error_.load());
        }
        return protectedAgain;
    }

    bool Restore()
    {
        std::lock_guard lock(mutex_);
        error_ = ERROR_SUCCESS;
        if (!slot_) return true;
        if (!MakeWritable(slot_)) return false;

        const PVOID original = original_.load(std::memory_order_acquire);
        const PVOID previous = InterlockedCompareExchangePointer(
            slot_, original, hook_);
        // If a different owner has already restored the same original, there is
        // no pointer work left. An unrelated hook must never be overwritten.
        const bool restored = previous == hook_ || previous == original;
        const bool protectedAgain = RestoreProtection();
        if (restored && protectedAgain) slot_ = nullptr;
        if (!restored) {
            return Fail(protectedAgain ? ERROR_REVISION_MISMATCH : error_.load());
        }
        return protectedAgain;
    }

    PVOID Original() const noexcept
    {
        return original_.load(std::memory_order_acquire);
    }

    bool OwnsSlot() const
    {
        std::lock_guard lock(mutex_);
        PVOID current = nullptr;
        return slot_ && published_ && ReadSlot(slot_, current) && current == hook_;
    }

    bool HasPendingProtection() const
    {
        std::lock_guard lock(mutex_);
        return protectionPending_;
    }

    DWORD Error() const noexcept
    {
        return error_.load(std::memory_order_relaxed);
    }

private:
    static bool ReadSlot(PVOID volatile* slot, PVOID& current) noexcept
    {
        SIZE_T bytesRead = 0;
        return ReadProcessMemory(GetCurrentProcess(),
            const_cast<PVOID*>(slot), &current, sizeof(current), &bytesRead) &&
            bytesRead == sizeof(current);
    }

    bool MakeWritable(PVOID volatile* slot)
    {
        DWORD previousProtection = 0;
        if (!protect_(const_cast<PVOID*>(slot), sizeof(PVOID),
                PAGE_READWRITE, &previousProtection)) {
            return Fail(GetLastError());
        }
        // Preserve the first protection until it is actually restored. A retry
        // otherwise mistakes PAGE_READWRITE for the original protection.
        if (!protectionPending_) originalProtection_ = previousProtection;
        slot_ = slot;
        protectionPending_ = true;
        return true;
    }

    bool RestoreProtection()
    {
        if (!protectionPending_) return true;
        DWORD ignored = 0;
        if (!protect_(const_cast<PVOID*>(slot_), sizeof(PVOID),
                originalProtection_, &ignored)) {
            return Fail(GetLastError());
        }
        protectionPending_ = false;
        return true;
    }

    bool Fail(DWORD error) noexcept
    {
        error_.store(error, std::memory_order_relaxed);
        SetLastError(error);
        return false;
    }

    ProtectMemory protect_;
    mutable std::mutex mutex_;
    std::atomic<PVOID> original_{ nullptr };
    std::atomic<DWORD> error_{ ERROR_SUCCESS };
    PVOID volatile* slot_ = nullptr;
    PVOID hook_ = nullptr;
    DWORD originalProtection_ = 0;
    bool protectionPending_ = false;
    bool published_ = false;
};
}
