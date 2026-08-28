#pragma once

#include <cstddef>
#include <cstdint>

namespace ProperShadersPatching
{
struct PatchBytes
{
    const std::uint8_t* actual{};
    const std::uint8_t* expected{};
    std::size_t size{};
};

inline bool BytesMatch(
    const std::uint8_t* actual,
    const std::uint8_t* expected,
    std::size_t size)
{
    if (size == 0) return true;
    if (!actual || !expected) return false;
    for (std::size_t i = 0; i < size; ++i) {
        if (actual[i] != expected[i]) return false;
    }
    return true;
}

inline std::size_t FindFirstMismatch(
    const PatchBytes* patches,
    std::size_t patchCount)
{
    if (!patches) return 0;
    for (std::size_t i = 0; i < patchCount; ++i) {
        const PatchBytes& patch = patches[i];
        if (!BytesMatch(patch.actual, patch.expected, patch.size)) return i;
    }
    return patchCount;
}
}
