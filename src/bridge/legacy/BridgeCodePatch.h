#pragma once

#include <windows.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace BridgeCodePatch {

struct NativeOps {
    decltype(&::VirtualProtect) protect = &::VirtualProtect;
    decltype(&::FlushInstructionCache) flush = &::FlushInstructionCache;
};

// The caller serializes this object and code execution at its owner safe
// points, and keeps the target allocation/module alive until Restore succeeds.
// No destructor writes code: shutdown must explicitly retry failed rollback.
class CodePatch {
public:
    explicit CodePatch(NativeOps ops = {}) noexcept : ops_(ops) {}
    CodePatch(const CodePatch&) = delete;
    CodePatch& operator=(const CodePatch&) = delete;
    CodePatch(CodePatch&&) = delete;
    CodePatch& operator=(CodePatch&&) = delete;

    bool Install(void* site, const void* expected, const void* replacement,
        std::size_t size) noexcept
    {
        error_ = ERROR_SUCCESS;
        if (!site || !expected || !replacement || size == 0 ||
            size > original_.size() || !ops_.protect || !ops_.flush) {
            return Fail(ERROR_INVALID_PARAMETER);
        }
        if (owned_) return Fail(ERROR_BUSY);

        // One saved protection must describe the entire byte range. Reject a
        // boundary between differently protected regions rather than applying
        // the first page's protection to another owner's adjacent page.
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(site, &memory, sizeof(memory)) != sizeof(memory)) {
            return Fail(NativeError());
        }
        const auto address = reinterpret_cast<std::uintptr_t>(site);
        const auto base = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        if (memory.State != MEM_COMMIT || address < base ||
            address - base > memory.RegionSize ||
            size > memory.RegionSize - (address - base)) {
            return Fail(ERROR_INVALID_ADDRESS);
        }
        std::array<std::uint8_t, 16> current{};
        if (!Read(site, current.data(), size)) return false;
        if (std::memcmp(current.data(), expected, size) != 0) {
            return Fail(ERROR_REVISION_MISMATCH);
        }

        // Capture both byte arrays before making the site writable, including
        // when the caller's replacement buffer aliases the target allocation.
        original_ = current;
        std::memcpy(replacement_.data(), replacement, size);
        DWORD oldProtection = 0;
        if (!ops_.protect(site, size, PAGE_EXECUTE_READWRITE, &oldProtection)) {
            return Fail(NativeError());
        }
        site_ = site;
        size_ = size;
        originalProtection_ = oldProtection;
        protectionPending_ = true;
        owned_ = true;
        originalWritten_ = false;
        std::memcpy(site_, replacement_.data(), size_);
        cachePending_ = true;
        return CompleteMemoryOperations();
    }

    bool Restore() noexcept
    {
        error_ = ERROR_SUCCESS;
        if (!owned_) return true;

        std::array<std::uint8_t, 16> current{};
        if (!Read(site_, current.data(), size_)) return false;
        const auto& expected = originalWritten_ ? original_ : replacement_;
        if (std::memcmp(current.data(), expected.data(), size_) != 0) {
            return Fail(ERROR_REVISION_MISMATCH);
        }

        if (!originalWritten_) {
            DWORD ignored = 0;
            if (!ops_.protect(site_, size_, PAGE_EXECUTE_READWRITE, &ignored)) {
                return Fail(NativeError());
            }
            // Keep the protection from Install, even if a failed cleanup left
            // this page writable before a later Restore retry.
            protectionPending_ = true;
            std::memcpy(site_, original_.data(), size_);
            originalWritten_ = true;
            cachePending_ = true;
        }
        if (!CompleteMemoryOperations()) return false;

        // Rollback state is released only after bytes, cache and protection
        // have all reached their original state.
        site_ = nullptr;
        size_ = 0;
        owned_ = false;
        originalWritten_ = false;
        return true;
    }

    // True means rollback is still this transaction's responsibility. It also
    // covers restored original bytes with failed cache/protection cleanup.
    bool OwnsBytes() const noexcept { return owned_; }
    bool HasPendingProtection() const noexcept { return protectionPending_; }
    DWORD Error() const noexcept { return error_; }

private:
    static DWORD NativeError() noexcept
    {
        const DWORD error = GetLastError();
        return error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error;
    }

    bool Fail(DWORD error) noexcept
    {
        error_ = error;
        SetLastError(error);
        return false;
    }

    bool Read(const void* site, void* bytes, std::size_t size) noexcept
    {
        SIZE_T read = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), site, bytes, size, &read)) {
            return Fail(NativeError());
        }
        if (read != size) return Fail(ERROR_PARTIAL_COPY);
        return true;
    }

    bool CompleteMemoryOperations() noexcept
    {
        DWORD failure = ERROR_SUCCESS;
        if (cachePending_) {
            if (ops_.flush(GetCurrentProcess(), site_, size_)) {
                cachePending_ = false;
            } else {
                failure = NativeError();
            }
        }
        // Always attempt to restore page protection, even after a cache error.
        // Each failed operation stays pending until a later Restore retry.
        if (protectionPending_) {
            DWORD ignored = 0;
            if (ops_.protect(site_, size_, originalProtection_, &ignored)) {
                protectionPending_ = false;
            } else {
                failure = NativeError();
            }
        }
        return failure == ERROR_SUCCESS ? true : Fail(failure);
    }

    NativeOps ops_;
    std::array<std::uint8_t, 16> original_{};
    std::array<std::uint8_t, 16> replacement_{};
    void* site_ = nullptr;
    std::size_t size_ = 0;
    DWORD originalProtection_ = 0;
    DWORD error_ = ERROR_SUCCESS;
    bool owned_ = false;
    bool originalWritten_ = false;
    bool protectionPending_ = false;
    bool cachePending_ = false;
};
} // namespace BridgeCodePatch
