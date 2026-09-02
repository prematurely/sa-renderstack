#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ProperShadersPatching
{
struct PatchBytes
{
    const std::uint8_t* actual{};
    const std::uint8_t* expected{};
    std::size_t size{};
};

inline bool BytesMatch(
    std::span<const std::uint8_t> actual,
    std::span<const std::uint8_t> expected)
{
    return actual.size() == expected.size() &&
        std::ranges::equal(actual, expected);
}

inline bool BytesMatch(
    const std::uint8_t* actual,
    const std::uint8_t* expected,
    std::size_t size)
{
    if (size == 0) return true;
    if (!actual || !expected) return false;
    return BytesMatch(
        std::span<const std::uint8_t>(actual, size),
        std::span<const std::uint8_t>(expected, size));
}

inline std::size_t FindFirstMismatch(
    std::span<const PatchBytes> patches)
{
    for (std::size_t i = 0; i < patches.size(); ++i) {
        const PatchBytes& patch = patches[i];
        if (!BytesMatch(patch.actual, patch.expected, patch.size)) return i;
    }
    return patches.size();
}
}
