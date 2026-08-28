#pragma once

#include "PerformanceAdapters.h"

#include <cstdint>
#include <string>
#include <vector>

namespace BridgePerformance
{
struct AdapterRuntimeConfig
{
    bool enabled = false;
    bool includeProcessMemory = true;
    bool includeConfigSnapshots = true;
    bool enableProviders = true;
    std::uint32_t providerSlowWarningUs = 2000;
    AdapterRegistry registry;
    std::vector<AdapterWarning> warnings;
};

struct ConfigSnapshotLimits
{
    std::size_t maxBytes = 1024u * 1024u;
    std::size_t maxEntries = 512u;
};

struct ConfigSnapshotEntry
{
    std::string section;
    std::string key;
    std::string value;
    std::string source;
};

struct ConfigSnapshot
{
    std::vector<ConfigSnapshotEntry> entries;
    std::vector<AdapterWarning> warnings;
    std::size_t bytesRead = 0;
    bool truncated = false;
};

AdapterRuntimeConfig LoadAdapterConfig(
    const std::string& iniPath,
    const std::string& gameDirectory);

ConfigSnapshot ReadConfigSnapshot(
    const std::string& path,
    const ConfigSnapshotLimits& limits = {});
}
