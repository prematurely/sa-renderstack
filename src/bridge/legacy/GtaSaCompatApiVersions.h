#pragma once

#include <cstdint>

namespace GtaSaCompatApiVersions
{
inline constexpr std::uint32_t kVulkanPass = 2u;
inline constexpr std::uint32_t kStateBatch = 3u;
inline constexpr std::uint32_t kStateJournal = 4u;
inline constexpr std::uint32_t kEffectStateBatch = 5u;
inline constexpr std::uint32_t kStateDrawBatch = 6u;
inline constexpr std::uint32_t kSelectiveStateJournal = 7u;

constexpr bool Supports(std::uint32_t runtimeVersion, std::uint32_t requiredVersion)
{
    return runtimeVersion >= requiredVersion;
}
}
