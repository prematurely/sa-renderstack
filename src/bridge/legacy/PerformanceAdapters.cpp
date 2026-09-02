#include "PerformanceAdapters.h"
#include "BridgePerformanceProviderV1.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <psapi.h>

#include <algorithm>
#include <cstdio>
#include <format>
#include <limits>
#include <print>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

#pragma comment(lib, "psapi.lib")

namespace BridgePerformance
{
namespace
{
std::uint64_t SaturatedSum(
    const std::uint64_t left,
    const std::uint64_t right) noexcept
{
    const std::uint64_t maximum =
        std::numeric_limits<std::uint64_t>::max();
    return right > maximum - left ? maximum : left + right;
}

void AddSampleOverflowWarning(
    AttributionResult& result,
    const char* counterName)
{
    const std::string detail = std::format(
        "counter '{}' saturated at UINT64_MAX",
        counterName);
    for (const AdapterWarning& warning : result.warnings)
    {
        if (warning.code == "sample-count-overflow" &&
            warning.adapter == "capture" && warning.detail == detail)
        {
            return;
        }
    }

    result.warnings.push_back({
        "sample-count-overflow",
        "capture",
        detail});
}

void AddSaturatedSamples(
    AttributionResult& result,
    std::uint64_t& total,
    const std::uint64_t samples,
    const char* counterName)
{
    const std::uint64_t maximum =
        std::numeric_limits<std::uint64_t>::max();
    if (samples > maximum - total)
    {
        AddSampleOverflowWarning(result, counterName);
    }
    total = SaturatedSum(total, samples);
}

std::string NormalizeSeparatorsAndCase(const std::string_view path)
{
    return path
        | std::views::transform([](const char character) -> char {
              if (character == '\\')
              {
                  return '/';
              }
              if (character >= 'A' && character <= 'Z')
              {
                  return static_cast<char>(character - 'A' + 'a');
              }
              return character;
          })
        | std::ranges::to<std::string>();
}

std::string NormalizeAdapterName(const std::string& name)
{
    return NormalizeSeparatorsAndCase(name);
}

std::string JoinSegments(
    const std::string& prefix,
    const std::vector<std::string>& segments)
{
    std::string result = prefix;
    for (const std::string& segment : segments)
    {
        if (!result.empty() && result.back() != '/' && result.back() != ':')
        {
            result.push_back('/');
        }
        result += segment;
    }
    return result;
}

std::string NormalizedLookupPath(const ModuleIdentity& module)
{
    return NormalizeModulePath(module.normalizedPath);
}

std::string NormalizedLookupBasename(
    const ModuleIdentity& module,
    const std::string& normalizedPath)
{
    if (!module.basename.empty())
    {
        return ModuleBasename(module.basename);
    }
    return ModuleBasename(normalizedPath);
}

std::string ModuleWarningName(const ModuleIdentity& module)
{
    if (!module.normalizedPath.empty())
    {
        return module.normalizedPath;
    }
    if (!module.basename.empty())
    {
        return module.basename;
    }
    return "<unknown-module>";
}

void AddExclusiveClaim(
    AdapterRegistry& registry,
    const std::string& token,
    std::size_t adapterIndex,
    std::unordered_map<std::string, std::size_t>& claims)
{
    if (token.empty())
    {
        return;
    }

    const auto owner = claims.find(token);
    if (owner != claims.end())
    {
        registry.warnings.push_back({
            "duplicate-owner",
            registry.adapters[adapterIndex].name,
            std::format(
                "token '{}' is already owned by '{}'",
                token,
                registry.adapters[owner->second].name)});
        return;
    }

    claims.emplace(token, adapterIndex);
}

void AddSharedClaim(
    AdapterRegistry& registry,
    const std::string& token,
    std::size_t adapterIndex,
    std::unordered_map<std::string, std::size_t>& claims)
{
    if (token.empty())
    {
        return;
    }

    const auto existing = claims.find(token);
    if (existing != claims.end())
    {
        if (existing->second != adapterIndex)
        {
            registry.warnings.push_back({
                "duplicate-shared-layer",
                registry.adapters[adapterIndex].name,
                std::format(
                    "token '{}' is already claimed by SharedLayer '{}'",
                    token,
                    registry.adapters[existing->second].name)});
        }
        return;
    }

    claims.emplace(token, adapterIndex);
}

void AddSharedMembership(
    const std::string& token,
    std::unordered_set<std::string>& membership)
{
    if (!token.empty())
    {
        membership.emplace(token);
    }
}

const AdapterDefinition* FindClaim(
    const AdapterRegistry& registry,
    const ModuleIdentity& module,
    const std::unordered_map<std::string, std::size_t>& fullPathClaims,
    const std::unordered_map<std::string, std::size_t>& basenameClaims)
{
    const std::string normalizedPath = NormalizedLookupPath(module);
    const auto fullPath = fullPathClaims.find(normalizedPath);
    if (fullPath != fullPathClaims.end())
    {
        return &registry.adapters[fullPath->second];
    }

    const std::string basename =
        NormalizedLookupBasename(module, normalizedPath);
    const auto byBasename = basenameClaims.find(basename);
    if (byBasename != basenameClaims.end())
    {
        return &registry.adapters[byBasename->second];
    }

    return nullptr;
}
}

std::string NormalizeModulePath(const std::string& path)
{
    const std::string normalized = NormalizeSeparatorsAndCase(path);
    std::string prefix;
    std::size_t position = 0;
    std::size_t protectedSegments = 0;
    bool absolute = false;

    if (normalized.size() >= 2 && normalized[1] == ':')
    {
        prefix = normalized.substr(0, 2);
        position = 2;
        if (position < normalized.size() && normalized[position] == '/')
        {
            prefix += '/';
            ++position;
            absolute = true;
        }
    }
    else if (normalized.size() >= 2 && normalized[0] == '/' &&
             normalized[1] == '/')
    {
        prefix = "//";
        position = 2;
        protectedSegments = 2;
        absolute = true;
    }
    else if (!normalized.empty() && normalized.front() == '/')
    {
        prefix = "/";
        position = 1;
        absolute = true;
    }

    std::vector<std::string> segments;
    while (position <= normalized.size())
    {
        const std::size_t separator = normalized.find('/', position);
        const std::size_t end = separator == std::string::npos
            ? normalized.size()
            : separator;
        const std::string segment = normalized.substr(position, end - position);

        if (segment.empty() || segment == ".")
        {
        }
        else if (segment == "..")
        {
            if (segments.size() > protectedSegments &&
                segments.back() != "..")
            {
                segments.pop_back();
            }
            else if (!absolute)
            {
                segments.push_back(segment);
            }
        }
        else
        {
            segments.push_back(segment);
        }

        if (separator == std::string::npos)
        {
            break;
        }
        position = separator + 1;
    }

    return JoinSegments(prefix, segments);
}

std::string ModuleBasename(const std::string& path)
{
    const std::string normalized = NormalizeModulePath(path);
    const std::size_t separator = normalized.find_last_of('/');
    return separator == std::string::npos
        ? normalized
        : normalized.substr(separator + 1);
}

const ModuleIdentity* FindSnapshotModuleByAddress(
    const std::vector<ModuleIdentity>& modules,
    const std::uintptr_t address) noexcept
{
    if (address == 0)
    {
        return nullptr;
    }

    const std::uintptr_t maximum =
        std::numeric_limits<std::uintptr_t>::max();
    for (const ModuleIdentity& module : modules)
    {
        if (module.moduleBase == 0 || module.imageSize == 0 ||
            module.imageSize > maximum - module.moduleBase)
        {
            continue;
        }

        const std::uintptr_t moduleEnd =
            module.moduleBase + module.imageSize;
        if (address >= module.moduleBase && address < moduleEnd)
        {
            return &module;
        }
    }
    return nullptr;
}

const ModuleIdentity* FindSnapshotProviderModule(
    const AdapterDefinition& adapter,
    const std::vector<ModuleIdentity>& modules)
{
    for (const std::string& configuredModule : adapter.modules)
    {
        const std::string token = NormalizeModulePath(configuredModule);
        if (token.empty())
        {
            continue;
        }

        const bool matchFullPath = token.find('/') != std::string::npos;
        for (const ModuleIdentity& module : modules)
        {
            if (module.moduleBase == 0 || module.imageSize == 0)
            {
                continue;
            }

            const std::string normalizedPath =
                NormalizedLookupPath(module);
            if (matchFullPath)
            {
                if (normalizedPath == token)
                {
                    return &module;
                }
                continue;
            }

            if (NormalizedLookupBasename(module, normalizedPath) == token)
            {
                return &module;
            }
        }
    }
    return nullptr;
}

AdapterRegistry BuildRegistry(
    const std::vector<AdapterDefinition>& definitions)
{
    AdapterRegistry registry;
    std::vector<AdapterDefinition> sortedAdapters = definitions;
    std::stable_sort(
        sortedAdapters.begin(),
        sortedAdapters.end(),
        [](const AdapterDefinition& left, const AdapterDefinition& right)
        {
            return left.registrationOrder < right.registrationOrder;
        });

    std::unordered_map<std::string, std::size_t> adapterNames;
    for (const AdapterDefinition& adapter : sortedAdapters)
    {
        const std::string normalizedName = NormalizeAdapterName(adapter.name);
        const auto existing = adapterNames.find(normalizedName);
        if (existing != adapterNames.end())
        {
            registry.warnings.push_back({
                "duplicate-adapter-name",
                adapter.name,
                std::format(
                    "adapter name '{}' duplicates '{}' case-insensitively",
                    adapter.name,
                    registry.adapters[existing->second].name)});
            continue;
        }

        adapterNames.emplace(normalizedName, registry.adapters.size());
        registry.adapters.push_back(adapter);
    }

    for (std::size_t adapterIndex = 0;
         adapterIndex < registry.adapters.size();
         ++adapterIndex)
    {
        const AdapterDefinition& adapter = registry.adapters[adapterIndex];
        if (!adapter.enabled || adapter.type == AdapterType::ConfigSnapshot)
        {
            continue;
        }

        if (adapter.type == AdapterType::ModuleGroup ||
            adapter.type == AdapterType::Hybrid)
        {
            for (const std::string& module : adapter.modules)
            {
                const std::string token = NormalizeModulePath(module);
                auto& claims = token.find('/') == std::string::npos
                    ? registry.exclusiveBasenameOwners
                    : registry.exclusiveFullPathOwners;
                AddExclusiveClaim(
                    registry,
                    token,
                    adapterIndex,
                    claims);
            }
        }

        for (const std::string& module : adapter.sharedModules)
        {
            const std::string token = NormalizeModulePath(module);
            auto& membership = token.find('/') == std::string::npos
                ? registry.sharedBasenameMembership
                : registry.sharedFullPathMembership;
            AddSharedMembership(token, membership);
            if (adapter.type == AdapterType::SharedLayer)
            {
                auto& claims = token.find('/') == std::string::npos
                    ? registry.sharedBasenameLayers
                    : registry.sharedFullPathLayers;
                AddSharedClaim(registry, token, adapterIndex, claims);
            }
        }

        if (adapter.type == AdapterType::SharedLayer)
        {
            for (const std::string& module : adapter.modules)
            {
                const std::string token = NormalizeModulePath(module);
                auto& membership = token.find('/') == std::string::npos
                    ? registry.sharedBasenameMembership
                    : registry.sharedFullPathMembership;
                AddSharedMembership(token, membership);
                auto& claims = token.find('/') == std::string::npos
                    ? registry.sharedBasenameLayers
                    : registry.sharedFullPathLayers;
                AddSharedClaim(
                    registry,
                    token,
                    adapterIndex,
                    claims);
            }
        }
    }

    return registry;
}

const AdapterDefinition* FindExclusiveOwner(
    const AdapterRegistry& registry,
    const ModuleIdentity& module)
{
    if (IsSharedModule(registry, module))
    {
        return nullptr;
    }

    return FindClaim(
        registry,
        module,
        registry.exclusiveFullPathOwners,
        registry.exclusiveBasenameOwners);
}

const AdapterDefinition* FindSharedLayer(
    const AdapterRegistry& registry,
    const ModuleIdentity& module)
{
    return FindClaim(
        registry,
        module,
        registry.sharedFullPathLayers,
        registry.sharedBasenameLayers);
}

bool IsSharedModule(
    const AdapterRegistry& registry,
    const ModuleIdentity& module)
{
    const std::string normalizedPath = NormalizedLookupPath(module);
    if (registry.sharedFullPathMembership.find(normalizedPath) !=
        registry.sharedFullPathMembership.end())
    {
        return true;
    }

    const std::string basename =
        NormalizedLookupBasename(module, normalizedPath);
    return registry.sharedBasenameMembership.find(basename) !=
        registry.sharedBasenameMembership.end();
}

AttributionResult AttributeCapture(
    const AdapterRegistry& registry,
    const CaptureSamples& capture)
{
    AttributionResult result;
    result.rawTotalSamples = capture.totalSamples;
    result.warnings = registry.warnings;

    std::unordered_map<std::size_t, std::size_t> summaryByAdapterIndex;
    for (std::size_t adapterIndex = 0;
         adapterIndex < registry.adapters.size();
         ++adapterIndex)
    {
        const AdapterDefinition& adapter = registry.adapters[adapterIndex];
        if (!adapter.enabled)
        {
            continue;
        }

        summaryByAdapterIndex.emplace(adapterIndex, result.summaries.size());
        result.summaries.push_back({
            adapter.name,
            adapter.registrationOrder,
            0,
            0,
            0});
    }

    std::unordered_map<std::uintptr_t, ModuleIdentity> modulesByBase;
    for (const ModuleSample& moduleSample : capture.modules)
    {
        const ModuleIdentity& module = moduleSample.module;
        if (module.moduleBase == 0)
        {
            result.warnings.push_back({
                "invalid-module-base",
                ModuleWarningName(module),
                "moduleBase must be non-zero; module sample skipped"});
            continue;
        }

        const auto existingModule = modulesByBase.find(module.moduleBase);
        if (existingModule != modulesByBase.end())
        {
            result.warnings.push_back({
                "duplicate-module-base",
                ModuleWarningName(module),
                std::format(
                    "module base {} already belongs to '{}'; module sample skipped",
                    module.moduleBase,
                    ModuleWarningName(existingModule->second))});
            continue;
        }

        modulesByBase.emplace(
            module.moduleBase,
            module);

        const AdapterDefinition* owner =
            FindExclusiveOwner(registry, module);
        if (owner == nullptr)
        {
            continue;
        }

        const auto adapterIndex = static_cast<std::size_t>(
            owner - registry.adapters.data());
        const auto summary = summaryByAdapterIndex.find(adapterIndex);
        if (summary == summaryByAdapterIndex.end())
        {
            continue;
        }

        AddSaturatedSamples(
            result,
            result.summaries[summary->second].exclusiveSamples,
            moduleSample.samples,
            "exclusiveSamples");
        AddSaturatedSamples(
            result,
            result.exclusiveAssignedSamples,
            moduleSample.samples,
            "exclusiveSamples");
    }

    for (const StackSample& stack : capture.stacks)
    {
        const auto instruction = modulesByBase.find(
            stack.instructionModuleBase);
        if (instruction == modulesByBase.end() ||
            !IsSharedModule(registry, instruction->second))
        {
            continue;
        }

        const AdapterDefinition* owner = nullptr;
        for (const std::uintptr_t callerBase : stack.callerModuleBases)
        {
            if (callerBase == 0)
            {
                continue;
            }

            const auto caller = modulesByBase.find(callerBase);
            if (caller == modulesByBase.end())
            {
                continue;
            }

            owner = FindExclusiveOwner(registry, caller->second);
            if (owner != nullptr)
            {
                break;
            }
        }

        if (owner != nullptr)
        {
            const auto adapterIndex = static_cast<std::size_t>(
                owner - registry.adapters.data());
            const auto summary = summaryByAdapterIndex.find(adapterIndex);
            if (summary != summaryByAdapterIndex.end())
            {
                AddSaturatedSamples(
                    result,
                    result.summaries[summary->second]
                        .attributedSharedSamples,
                    stack.samples,
                    "attributedSharedSamples");
                AddSaturatedSamples(
                    result,
                    result.attributedSharedSamples,
                    stack.samples,
                    "attributedSharedSamples");
            }
            continue;
        }

        const AdapterDefinition* fallback = FindSharedLayer(
            registry,
            instruction->second);
        if (fallback == nullptr)
        {
            AddSaturatedSamples(
                result,
                result.unassignedSharedSamples,
                stack.samples,
                "unassignedSharedSamples");
            continue;
        }

        const auto adapterIndex = static_cast<std::size_t>(
            fallback - registry.adapters.data());
        const auto summary = summaryByAdapterIndex.find(adapterIndex);
        if (summary != summaryByAdapterIndex.end())
        {
            AddSaturatedSamples(
                result,
                result.summaries[summary->second].unresolvedSharedSamples,
                stack.samples,
                "unresolvedSharedSamples");
            AddSaturatedSamples(
                result,
                result.unresolvedSharedSamples,
                stack.samples,
                "unresolvedSharedSamples");
        }
    }

    const std::uint64_t maximum =
        std::numeric_limits<std::uint64_t>::max();
    for (const AdapterSummary& summary : result.summaries)
    {
        if (summary.attributedSharedSamples >
            maximum - summary.exclusiveSamples)
        {
            AddSampleOverflowWarning(result, "summaryEvidence");
            break;
        }
    }

    std::stable_sort(
        result.summaries.begin(),
        result.summaries.end(),
        [](const AdapterSummary& left, const AdapterSummary& right)
        {
            const std::uint64_t leftEvidence = SaturatedSum(
                left.exclusiveSamples,
                left.attributedSharedSamples);
            const std::uint64_t rightEvidence = SaturatedSum(
                right.exclusiveSamples,
                right.attributedSharedSamples);
            if (leftEvidence != rightEvidence)
            {
                return leftEvidence > rightEvidence;
            }
            return left.registrationOrder < right.registrationOrder;
        });

    return result;
}

const AdapterSummary* FindSummary(
    const AttributionResult& result,
    const std::string& adapterName)
{
    const auto summary = std::ranges::find_if(
        result.summaries,
        [&adapterName](const AdapterSummary& candidate) {
            return candidate.name == adapterName;
        });
    return summary == result.summaries.end() ? nullptr : &*summary;
}
}

namespace BridgePerformanceProviderV1
{
namespace
{
struct SehCallResult
{
    bool raised = false;
    BOOL returned = FALSE;
};

__declspec(noinline) SehCallResult CallProviderWithSeh(
    QueryFunction function,
    const Query* query,
    Snapshot* snapshot) noexcept
{
    SehCallResult result{};
    __try
    {
        result.returned = function(query, snapshot);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        result.raised = true;
    }
    return result;
}

}
}

namespace BridgePerformance
{
namespace
{
std::string EscapeReportValue(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size());
    static constexpr char Hex[] = "0123456789ABCDEF";
    for (const unsigned char character : value)
    {
        switch (character)
        {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            if (character < 0x20u || character == 0x7fu)
            {
                escaped += "\\x";
                escaped.push_back(Hex[(character >> 4) & 0xfu]);
                escaped.push_back(Hex[character & 0xfu]);
            }
            else
            {
                escaped.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    return escaped;
}

double Percentage(
    const std::uint64_t value,
    const std::uint64_t total) noexcept
{
    return total == 0
        ? 0.0
        : (static_cast<double>(value) * 100.0) /
            static_cast<double>(total);
}

std::uint64_t RegionSize(const MEMORY_BASIC_INFORMATION& information) noexcept
{
    return static_cast<std::uint64_t>(information.RegionSize);
}
}

ProcessMemoryEvidence AccumulateMemoryRegions(
    const std::uintptr_t addressStart,
    const std::uintptr_t addressEnd,
    const std::vector<MemoryRegionSample>& regions) noexcept
{
    ProcessMemoryEvidence result;
    result.userAddressSpaceBytes = addressEnd >= addressStart
        ? static_cast<std::uint64_t>(addressEnd - addressStart)
        : 0;
    if (addressEnd < addressStart)
    {
        result.queryFailed = true;
    }

    for (const MemoryRegionSample& region : regions)
    {
        const std::uint64_t size = static_cast<std::uint64_t>(region.size);
        if (region.state == MemoryRegionState::Free)
        {
            result.freeBytes = SaturatedSum(result.freeBytes, size);
            result.largestFreeRegion =
                std::max(result.largestFreeRegion, size);
        }
        else
        {
            result.virtualBytes =
                SaturatedSum(result.virtualBytes, size);
        }
    }
    return result;
}

ProcessMemoryEvidence CollectProcessMemory() noexcept
{
    ProcessMemoryEvidence result;

    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters)) != FALSE)
    {
        result.privateBytes = static_cast<std::uint64_t>(
            counters.PrivateUsage);
        result.workingSet = static_cast<std::uint64_t>(
            counters.WorkingSetSize);
    }
    else
    {
        result.queryFailed = true;
    }

    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    const std::uintptr_t addressStart = reinterpret_cast<std::uintptr_t>(
        systemInfo.lpMinimumApplicationAddress);
    const std::uintptr_t addressEnd = reinterpret_cast<std::uintptr_t>(
        systemInfo.lpMaximumApplicationAddress);
    result.userAddressSpaceBytes = addressEnd >= addressStart
        ? static_cast<std::uint64_t>(addressEnd - addressStart)
        : 0;
    if (addressEnd < addressStart)
    {
        result.queryFailed = true;
        return result;
    }

    std::uintptr_t cursor = addressStart;
    while (cursor < addressEnd)
    {
        MEMORY_BASIC_INFORMATION information{};
        const SIZE_T queried = VirtualQuery(
            reinterpret_cast<const void*>(cursor),
            &information,
            sizeof(information));
        if (queried == 0 || information.RegionSize == 0)
        {
            result.queryFailed = true;
            break;
        }

        const std::uint64_t size = RegionSize(information);
        if (information.State == MEM_FREE)
        {
            result.freeBytes = SaturatedSum(result.freeBytes, size);
            result.largestFreeRegion =
                std::max(result.largestFreeRegion, size);
        }
        else
        {
            result.virtualBytes =
                SaturatedSum(result.virtualBytes, size);
        }

        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(
            information.BaseAddress);
        if (base > std::numeric_limits<std::uintptr_t>::max() -
                static_cast<std::uintptr_t>(information.RegionSize))
        {
            result.queryFailed = true;
            break;
        }
        const std::uintptr_t next = base + static_cast<std::uintptr_t>(
            information.RegionSize);
        if (next <= cursor)
        {
            result.queryFailed = true;
            break;
        }
        cursor = next;
    }

    return result;
}

void AppendAdapterReport(
    std::FILE* output,
    const CaptureMetadata& metadata,
    const AttributionResult& attribution,
    const ProcessMemoryEvidence& memory,
    const std::vector<AdapterMetricRow>& metrics,
    const std::vector<AdapterConfigRow>& configs,
    const std::vector<AdapterWarning>& warnings)
{
    if (output == nullptr)
    {
        return;
    }

    std::print(
        output,
        "adapterCapture captureId={} elapsedMs={} frameCount={} fps={:.2f}\n",
        metadata.captureId,
        metadata.elapsedMs,
        metadata.frameCount,
        metadata.fps);

    for (std::size_t index = 0; index < attribution.summaries.size(); ++index)
    {
        const AdapterSummary& summary = attribution.summaries[index];
        const std::uint64_t totalEvidence = SaturatedSum(
            SaturatedSum(
                summary.exclusiveSamples,
                summary.attributedSharedSamples),
            summary.unresolvedSharedSamples);
        std::print(
            output,
            "adapterRank={} name={} registrationOrder={} "
            "exclusiveSamples={} exclusivePercent={:.2f} "
            "attributedSharedSamples={} attributedSharedPercent={:.2f} "
            "unresolvedSharedSamples={} unresolvedSharedPercent={:.2f} "
            "totalPercent={:.2f}\n",
            index + 1,
            EscapeReportValue(summary.name),
            summary.registrationOrder,
            summary.exclusiveSamples,
            Percentage(summary.exclusiveSamples, attribution.rawTotalSamples),
            summary.attributedSharedSamples,
            Percentage(
                summary.attributedSharedSamples,
                attribution.rawTotalSamples),
            summary.unresolvedSharedSamples,
            Percentage(
                summary.unresolvedSharedSamples,
                attribution.rawTotalSamples),
            Percentage(totalEvidence, attribution.rawTotalSamples));
    }

    for (const AdapterMetricRow& metric : metrics)
    {
        std::print(
            output,
            "adapterMetric name={} key={} value={}\n",
            EscapeReportValue(metric.adapter),
            EscapeReportValue(metric.key),
            metric.value);
    }

    for (const AdapterConfigRow& config : configs)
    {
        std::print(
            output,
            "adapterConfig name={} key={} value={} source={}\n",
            EscapeReportValue(config.adapter),
            EscapeReportValue(config.key),
            EscapeReportValue(config.value),
            EscapeReportValue(config.source));
    }

    std::print(
        output,
        "processMemory privateBytes={} workingSet={} virtualBytes={} "
        "freeBytes={} largestFreeRegion={} userAddressSpaceBytes={}\n",
        memory.privateBytes,
        memory.workingSet,
        memory.virtualBytes,
        memory.freeBytes,
        memory.largestFreeRegion,
        memory.userAddressSpaceBytes);

    if (memory.queryFailed)
    {
        std::print(
            output,
            "adapterWarning name=processMemory code=query-failed "
            "detail=one or more process memory queries failed\n");
    }

    for (const AdapterWarning& warning : warnings)
    {
        const std::string warningName = warning.adapter.empty()
            ? "global"
            : warning.adapter;
        std::print(
            output,
            "adapterWarning name={} code={} detail=\"{}\"\n",
            EscapeReportValue(warningName),
            EscapeReportValue(warning.code),
            EscapeReportValue(warning.detail));
    }
}
}

namespace BridgePerformanceProviderV1
{
namespace
{
std::uint64_t ReadQpc() noexcept
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart < 0
        ? 0u
        : static_cast<std::uint64_t>(value.QuadPart);
}

std::uint64_t ReadQpcFrequency() noexcept
{
    LARGE_INTEGER value{};
    if (!QueryPerformanceFrequency(&value) || value.QuadPart <= 0)
    {
        return 0;
    }
    return static_cast<std::uint64_t>(value.QuadPart);
}

std::uint64_t QpcToMicroseconds(
    const std::uint64_t ticks,
    const std::uint64_t frequency) noexcept
{
    if (frequency == 0 || ticks == 0)
    {
        return 0;
    }

    constexpr std::uint64_t MicrosecondsPerSecond = 1000000u;
    const std::uint64_t maximum =
        std::numeric_limits<std::uint64_t>::max();
    if (ticks > maximum / MicrosecondsPerSecond)
    {
        return maximum / frequency;
    }
    return (ticks * MicrosecondsPerSecond) / frequency;
}
}

ValidationResult ValidateSnapshot(const Snapshot& snapshot) noexcept
{
    if (snapshot.size != sizeof(Snapshot))
    {
        return {ValidationCode::SnapshotSize, 0};
    }
    if (snapshot.apiVersion != ApiVersion)
    {
        return {ValidationCode::SnapshotVersion, 0};
    }
    if (snapshot.metricCount > MaxMetrics)
    {
        return {ValidationCode::MetricCount, 0};
    }

    for (std::uint32_t index = 0; index < snapshot.metricCount; ++index)
    {
        const Metric& metric = snapshot.metrics[index];
        std::size_t nameLength = 0;
        while (nameLength < MetricNameCapacity &&
               metric.name[nameLength] != '\0')
        {
            ++nameLength;
        }
        if (nameLength == 0)
        {
            return {ValidationCode::EmptyMetricName, index};
        }
        if (nameLength == MetricNameCapacity)
        {
            return {ValidationCode::UnterminatedMetricName, index};
        }

        if (metric.kind != std::to_underlying(MetricKind::Counter) &&
            metric.kind != std::to_underlying(MetricKind::Microseconds))
        {
            return {ValidationCode::UnknownMetricKind, index};
        }
        if (metric.reserved != 0)
        {
            return {ValidationCode::NonzeroReserved, index};
        }
    }

    return {};
}

const char* ValidationCodeName(const ValidationCode code) noexcept
{
    switch (code)
    {
    case ValidationCode::Valid:
        return "valid";
    case ValidationCode::SnapshotSize:
        return "snapshot-size";
    case ValidationCode::SnapshotVersion:
        return "snapshot-version";
    case ValidationCode::MetricCount:
        return "metric-count";
    case ValidationCode::EmptyMetricName:
        return "empty-metric-name";
    case ValidationCode::UnterminatedMetricName:
        return "unterminated-metric-name";
    case ValidationCode::UnknownMetricKind:
        return "unknown-metric-kind";
    case ValidationCode::NonzeroReserved:
        return "nonzero-reserved";
    }
    return "unknown";
}

CallResult Invoke(
    const QueryFunction function,
    const Query& query,
    const std::uint32_t slowWarningUs,
    ProviderHealth& health) noexcept
{
    CallResult result{};
    if (health.quarantined)
    {
        result.status = CallStatus::SkippedQuarantined;
        return result;
    }
    if (function == nullptr)
    {
        health.quarantined = true;
        result.status = CallStatus::InvalidFunction;
        return result;
    }
    if (query.size != sizeof(Query) || query.apiVersion != ApiVersion)
    {
        health.quarantined = true;
        result.status = CallStatus::InvalidQuery;
        return result;
    }

    Snapshot snapshot{};
    snapshot.size = 0;
    snapshot.apiVersion = 0;
    const std::uint64_t frequency = ReadQpcFrequency();
    const std::uint64_t start = ReadQpc();
    const SehCallResult call = CallProviderWithSeh(
        function,
        &query,
        &snapshot);
    const std::uint64_t end = ReadQpc();
    result.called = true;
    result.elapsedUs = end >= start
        ? QpcToMicroseconds(end - start, frequency)
        : 0;
    result.snapshot = snapshot;

    if (call.raised)
    {
        health.quarantined = true;
        result.status = CallStatus::ProviderException;
        return result;
    }
    if (!call.returned)
    {
        health.quarantined = true;
        result.status = CallStatus::ProviderReturnedFalse;
        return result;
    }

    result.validation = ValidateSnapshot(result.snapshot);
    if (result.validation.code != ValidationCode::Valid)
    {
        health.quarantined = true;
        result.status = CallStatus::InvalidSnapshot;
        return result;
    }
    if (slowWarningUs != 0 && result.elapsedUs > slowWarningUs)
    {
        health.quarantined = true;
        result.status = CallStatus::SlowProvider;
        return result;
    }

    result.status = CallStatus::Accepted;
    return result;
}
}
