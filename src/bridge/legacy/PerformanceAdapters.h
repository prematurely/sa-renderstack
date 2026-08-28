#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace BridgePerformance
{
enum class AdapterType
{
    ModuleGroup,
    Hybrid,
    ConfigSnapshot,
    SharedLayer,
};

struct AdapterDefinition
{
    std::string name;
    unsigned registrationOrder = 0;
    AdapterType type = AdapterType::ModuleGroup;
    bool enabled = false;
    std::vector<std::string> modules;
    std::vector<std::string> sharedModules;
    std::vector<std::string> configPaths;
    std::string providerExport;
};

struct ModuleIdentity
{
    std::string normalizedPath;
    std::string basename;
    std::uintptr_t moduleBase = 0;
    std::uintptr_t imageSize = 0;
};

struct ModuleSample
{
    ModuleIdentity module;
    std::uint64_t samples = 0;
};

struct StackSample
{
    std::uintptr_t instructionModuleBase = 0;
    std::array<std::uintptr_t, 3> callerModuleBases{};
    std::uint64_t samples = 0;
};

struct CaptureSamples
{
    std::uint64_t totalSamples = 0;
    std::vector<ModuleSample> modules;
    std::vector<StackSample> stacks;
};

struct AdapterWarning
{
    std::string code;
    std::string adapter;
    std::string detail;
};

struct AdapterSummary
{
    std::string name;
    unsigned registrationOrder = 0;
    std::uint64_t exclusiveSamples = 0;
    std::uint64_t attributedSharedSamples = 0;
    std::uint64_t unresolvedSharedSamples = 0;
};

struct AttributionResult
{
    std::vector<AdapterSummary> summaries;
    std::vector<AdapterWarning> warnings;
    std::uint64_t rawTotalSamples = 0;
    std::uint64_t exclusiveAssignedSamples = 0;
    std::uint64_t attributedSharedSamples = 0;
    std::uint64_t unresolvedSharedSamples = 0;
    std::uint64_t unassignedSharedSamples = 0;
};

enum class MemoryRegionState : std::uint32_t
{
    Free = 0,
    Reserved = 1,
    Committed = 2,
};

struct MemoryRegionSample
{
    std::uintptr_t address = 0;
    std::uintptr_t size = 0;
    MemoryRegionState state = MemoryRegionState::Free;
};

struct ProcessMemoryEvidence
{
    std::uint64_t privateBytes = 0;
    std::uint64_t workingSet = 0;
    std::uint64_t virtualBytes = 0;
    std::uint64_t freeBytes = 0;
    std::uint64_t largestFreeRegion = 0;
    std::uint64_t userAddressSpaceBytes = 0;
    bool queryFailed = false;
};

ProcessMemoryEvidence AccumulateMemoryRegions(
    std::uintptr_t addressStart,
    std::uintptr_t addressEnd,
    const std::vector<MemoryRegionSample>& regions) noexcept;

ProcessMemoryEvidence CollectProcessMemory() noexcept;

struct CaptureMetadata
{
    std::uint32_t captureId = 0;
    std::uint32_t elapsedMs = 0;
    std::uint64_t frameCount = 0;
    double fps = 0.0;
};

struct AdapterMetricRow
{
    std::string adapter;
    std::string key;
    std::uint64_t value = 0;
};

struct AdapterConfigRow
{
    std::string adapter;
    std::string key;
    std::string value;
    std::string source;
};

void AppendAdapterReport(
    std::FILE* output,
    const CaptureMetadata& metadata,
    const AttributionResult& attribution,
    const ProcessMemoryEvidence& memory,
    const std::vector<AdapterMetricRow>& metrics,
    const std::vector<AdapterConfigRow>& configs,
    const std::vector<AdapterWarning>& warnings);

struct AdapterRegistry
{
    std::vector<AdapterDefinition> adapters;
    std::vector<AdapterWarning> warnings;

private:
    std::unordered_map<std::string, std::size_t> exclusiveFullPathOwners;
    std::unordered_map<std::string, std::size_t> exclusiveBasenameOwners;
    std::unordered_set<std::string> sharedFullPathMembership;
    std::unordered_set<std::string> sharedBasenameMembership;
    std::unordered_map<std::string, std::size_t> sharedFullPathLayers;
    std::unordered_map<std::string, std::size_t> sharedBasenameLayers;

    friend AdapterRegistry BuildRegistry(
        const std::vector<AdapterDefinition>& definitions);
    friend const AdapterDefinition* FindExclusiveOwner(
        const AdapterRegistry& registry,
        const ModuleIdentity& module);
    friend bool IsSharedModule(
        const AdapterRegistry& registry,
        const ModuleIdentity& module);
    friend const AdapterDefinition* FindSharedLayer(
        const AdapterRegistry& registry,
        const ModuleIdentity& module);
};

std::string NormalizeModulePath(const std::string& path);
std::string ModuleBasename(const std::string& path);

const ModuleIdentity* FindSnapshotModuleByAddress(
    const std::vector<ModuleIdentity>& modules,
    std::uintptr_t address) noexcept;

const ModuleIdentity* FindSnapshotProviderModule(
    const AdapterDefinition& adapter,
    const std::vector<ModuleIdentity>& modules);

AdapterRegistry BuildRegistry(
    const std::vector<AdapterDefinition>& definitions);

const AdapterDefinition* FindExclusiveOwner(
    const AdapterRegistry& registry,
    const ModuleIdentity& module);

const AdapterDefinition* FindSharedLayer(
    const AdapterRegistry& registry,
    const ModuleIdentity& module);

bool IsSharedModule(
    const AdapterRegistry& registry,
    const ModuleIdentity& module);

AttributionResult AttributeCapture(
    const AdapterRegistry& registry,
    const CaptureSamples& capture);

const AdapterSummary* FindSummary(
    const AttributionResult& result,
    const std::string& adapterName);
}
