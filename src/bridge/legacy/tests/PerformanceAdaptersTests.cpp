#include "../PerformanceAdapters.h"
#include "../BridgePerformanceProviderV1.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
namespace Provider = BridgePerformanceProviderV1;

bool Check(bool condition, const char* message)
{
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

BridgePerformance::AdapterDefinition Definition(
    const char* name,
    unsigned registrationOrder,
    BridgePerformance::AdapterType type,
    bool enabled,
    std::vector<std::string> modules,
    std::vector<std::string> sharedModules = {})
{
    BridgePerformance::AdapterDefinition definition{};
    definition.name = name;
    definition.registrationOrder = registrationOrder;
    definition.type = type;
    definition.enabled = enabled;
    definition.modules = std::move(modules);
    definition.sharedModules = std::move(sharedModules);
    definition.configPaths = {};
    definition.providerExport = {};
    return definition;
}

BridgePerformance::ModuleIdentity Identity(
    const char* normalizedPath,
    const char* basename)
{
    BridgePerformance::ModuleIdentity identity{};
    identity.normalizedPath = normalizedPath;
    identity.basename = basename;
    identity.moduleBase = 0;
    identity.imageSize = 0;
    return identity;
}

void FillMetric(
    Provider::Metric& metric,
    const char* name,
    Provider::MetricKind kind,
    std::uint64_t value)
{
    std::memset(&metric, 0, sizeof(metric));
    std::strncpy(metric.name, name, Provider::MetricNameCapacity - 1);
    metric.kind = static_cast<std::uint32_t>(kind);
    metric.value = value;
}

int g_providerCallCount = 0;

BOOL __stdcall HealthyProvider(
    const Provider::Query* query,
    Provider::Snapshot* snapshot)
{
    ++g_providerCallCount;
    if (query == nullptr || snapshot == nullptr)
    {
        return FALSE;
    }
    snapshot->size = sizeof(Provider::Snapshot);
    snapshot->apiVersion = Provider::ApiVersion;
    snapshot->metricCount = 2;
    FillMetric(
        snapshot->metrics[0],
        "draws",
        Provider::MetricKind::Counter,
        query->frameCount);
    FillMetric(
        snapshot->metrics[1],
        "gpuUs",
        Provider::MetricKind::Microseconds,
        42);
    return TRUE;
}

BOOL __stdcall SlowProvider(
    const Provider::Query*,
    Provider::Snapshot* snapshot)
{
    ++g_providerCallCount;
    Sleep(3);
    if (snapshot != nullptr)
    {
        snapshot->size = sizeof(Provider::Snapshot);
        snapshot->apiVersion = Provider::ApiVersion;
        snapshot->metricCount = 0;
    }
    return TRUE;
}

BOOL __stdcall CrashingProvider(
    const Provider::Query*,
    Provider::Snapshot*)
{
    ++g_providerCallCount;
    RaiseException(0xE0420001u, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    return FALSE;
}

BOOL __stdcall EmptyProvider(
    const Provider::Query*,
    Provider::Snapshot*)
{
    ++g_providerCallCount;
    return TRUE;
}
}

int main()
{
    using namespace BridgePerformance;

    bool ok = true;

    static_assert(
        std::is_same_v<decltype(ModuleIdentity::moduleBase), std::uintptr_t>);
    static_assert(
        std::is_same_v<decltype(ModuleIdentity::imageSize), std::uintptr_t>);

    const ModuleIdentity sampleIdentity{};
    const ModuleSample moduleSample{sampleIdentity, 1};
    const StackSample stackSample{0, {0, 0, 0}, 1};
    const CaptureSamples captureSamples{
        1,
        {moduleSample},
        {stackSample}};
    ok &= Check(
        captureSamples.modules.size() == 1 &&
            captureSamples.stacks.size() == 1,
        "frozen performance sample data structures are available");

    ok &= Check(
        NormalizeModulePath("./Drivers\\D3D9/../D3D9.dll") ==
            "drivers/d3d9.dll",
        "module paths lowercase, normalize slashes, and lexically collapse dot segments");
    ok &= Check(
        ModuleBasename("C:\\Windows\\System32\\d3d9.dll") == "d3d9.dll",
        "module basename removes the directory portion");
    ok &= Check(
        NormalizeModulePath("\\\\Server\\Share\\Folder\\..\\d3d9.dll") ==
            "//server/share/d3d9.dll",
        "UNC paths preserve their double leading slash and server/share root");
    ok &= Check(
        NormalizeModulePath("\\\\Server\\Share\\Folder\\..\\d3d9.dll") !=
            NormalizeModulePath("/server/share/d3d9.dll"),
        "UNC paths remain distinct from single-slash absolute paths");
    ok &= Check(
        NormalizeModulePath("\\\\Server\\Share\\..\\d3d9.dll") ==
            "//server/share/d3d9.dll",
        "UNC parent traversal cannot pop the share root");
    ok &= Check(
        NormalizeModulePath("\\\\Server\\Share\\..\\..\\d3d9.dll") ==
            "//server/share/d3d9.dll",
        "multiple UNC parent traversals cannot pop the share root");

    const std::vector<ModuleIdentity> moduleSnapshot = {
        {
            NormalizeModulePath("D:\\GTA San Andreas\\gta_sa.exe"),
            "gta_sa.exe",
            0x00400000u,
            0x00500000u,
        },
        {
            NormalizeModulePath(
                "D:\\GTA San Andreas\\modloader\\Proper Shaders\\"
                "ProperShaders.asi"),
            "propershaders.asi",
            0x10000000u,
            0x00020000u,
        },
        {
            NormalizeModulePath(
                "D:\\GTA San Andreas\\dxvk-3.0.1-merged\\d3d9.dll"),
            "d3d9.dll",
            0x20000000u,
            0x00800000u,
        },
        {
            "invalid-overflow.dll",
            "invalid-overflow.dll",
            (std::numeric_limits<std::uintptr_t>::max)() - 0x10u,
            0x40u,
        },
    };

    const ModuleIdentity* snapshotByAddress = FindSnapshotModuleByAddress(
        moduleSnapshot,
        0x10001234u);
    ok &= Check(
        snapshotByAddress != nullptr &&
            snapshotByAddress->basename == "propershaders.asi",
        "capture addresses resolve against the capture-start module snapshot");
    ok &= Check(
        FindSnapshotModuleByAddress(moduleSnapshot, 0x10020000u) == nullptr,
        "capture-start module ranges exclude their end address");
    ok &= Check(
        FindSnapshotModuleByAddress(
            moduleSnapshot,
            (std::numeric_limits<std::uintptr_t>::max)() - 1u) == nullptr,
        "overflowing capture-start module ranges are rejected");

    AdapterDefinition providerDefinition = Definition(
        "ProperShaders",
        1,
        AdapterType::Hybrid,
        true,
        {"ProperShaders.asi"},
        {"d3d9.dll"});
    providerDefinition.providerExport = "BridgePerf_QueryV1";
    const ModuleIdentity* providerModule = FindSnapshotProviderModule(
        providerDefinition,
        moduleSnapshot);
    ok &= Check(
        providerModule != nullptr &&
            providerModule->moduleBase == 0x10000000u,
        "provider lookup selects the adapter's capture-start owner module");

    providerDefinition.modules = {
        "D:/Other Game/modloader/Proper Shaders/ProperShaders.asi"};
    ok &= Check(
        FindSnapshotProviderModule(providerDefinition, moduleSnapshot) ==
            nullptr,
        "provider full-path registrations do not degrade to basename matches");

    providerDefinition.modules = {"missing-provider.asi"};
    ok &= Check(
        FindSnapshotProviderModule(providerDefinition, moduleSnapshot) ==
            nullptr,
        "shared render modules are never selected as an adapter provider");

    const auto registry = BuildRegistry({
        Definition("owner-b", 20, AdapterType::ModuleGroup, true,
                   {"C:\\Game\\b.dll"}),
        Definition("owner-a", 10, AdapterType::ModuleGroup, true,
                   {"C:\\Game\\a.dll"}),
        Definition("layer-a", 30, AdapterType::SharedLayer, true, {},
                   {"C:\\Game\\d3d9.dll"}),
    });
    ok &= Check(
        registry.adapters.size() == 3 &&
            registry.adapters[0].name == "owner-a" &&
            registry.adapters[1].name == "owner-b" &&
            registry.adapters[2].name == "layer-a",
        "registry sorts adapters by stable registration order");

    const auto equalOrderRegistry = BuildRegistry({
        Definition("equal-first", 40, AdapterType::ModuleGroup, true,
                   {"C:\\Game\\equal-first.dll"}),
        Definition("equal-second", 40, AdapterType::ModuleGroup, true,
                   {"C:\\Game\\equal-second.dll"}),
    });
    ok &= Check(
        equalOrderRegistry.adapters[0].name == "equal-first" &&
            equalOrderRegistry.adapters[1].name == "equal-second",
        "equal registration orders preserve input order");

    const auto d3d9Identity = Identity("c:/game/d3d9.dll", "d3d9.dll");
    const auto duplicateRegistry = BuildRegistry({
        Definition("first-owner", 1, AdapterType::ModuleGroup, true,
                   {"C:\\Game\\d3d9.dll"}),
        Definition("second-owner", 2, AdapterType::ModuleGroup, true,
                   {"C:\\Game\\d3d9.dll"}),
        Definition("third-owner", 3, AdapterType::ModuleGroup, true,
                   {"C:\\Game\\d3d9.dll"}),
    });
    ok &= Check(
        FindExclusiveOwner(duplicateRegistry, d3d9Identity) != nullptr &&
            FindExclusiveOwner(duplicateRegistry, d3d9Identity)->name ==
                "first-owner",
        "duplicate owner keeps the first registration");
    ok &= Check(
        duplicateRegistry.warnings.size() == 2 &&
            duplicateRegistry.warnings[0].code == "duplicate-owner" &&
            duplicateRegistry.warnings[0].adapter == "second-owner" &&
            duplicateRegistry.warnings[1].code == "duplicate-owner" &&
            duplicateRegistry.warnings[1].adapter == "third-owner",
        "each rejected duplicate owner emits an ordered warning");

    const auto duplicateNameRegistry = BuildRegistry({
        Definition("Owner", 1, AdapterType::ModuleGroup, true,
                   {"C:\\Game\\owner.dll"}),
        Definition("owner", 2, AdapterType::ModuleGroup, true,
                   {"C:\\Game\\other-owner.dll"}),
    });
    ok &= Check(
        duplicateNameRegistry.adapters.size() == 1 &&
            duplicateNameRegistry.adapters[0].name == "Owner" &&
            duplicateNameRegistry.warnings.size() == 1 &&
            duplicateNameRegistry.warnings[0].code ==
                "duplicate-adapter-name" &&
            duplicateNameRegistry.warnings[0].adapter == "owner",
        "case-insensitive duplicate adapter names keep the first definition");
    const AttributionResult duplicateNameAttribution = AttributeCapture(
        duplicateNameRegistry,
        CaptureSamples{});
    ok &= Check(
        FindSummary(duplicateNameAttribution, "Owner") != nullptr &&
            FindSummary(duplicateNameAttribution, "owner") == nullptr,
        "duplicate adapter names keep attribution summaries unambiguous");

    const auto duplicateSharedLayerRegistry = BuildRegistry({
        Definition("first-shared-layer", 1, AdapterType::SharedLayer, true,
                   {"C:\\Game\\shared.dll"}),
        Definition("second-shared-layer", 2, AdapterType::SharedLayer, true,
                   {"C:\\Game\\shared.dll"}),
    });
    const auto sharedLayerIdentity = Identity(
        "c:/game/shared.dll", "shared.dll");
    ok &= Check(
        FindSharedLayer(duplicateSharedLayerRegistry, sharedLayerIdentity) !=
                nullptr &&
            FindSharedLayer(duplicateSharedLayerRegistry, sharedLayerIdentity)
                    ->name == "first-shared-layer" &&
            duplicateSharedLayerRegistry.warnings.size() == 1 &&
            duplicateSharedLayerRegistry.warnings[0].code ==
                "duplicate-shared-layer" &&
            duplicateSharedLayerRegistry.warnings[0].adapter ==
                "second-shared-layer",
        "duplicate SharedLayer fallback claims keep the first registration");

    const auto invalidBaseRegistry = BuildRegistry({
        Definition("invalid-base-owner", 1, AdapterType::ModuleGroup, true,
                   {"C:\\Game\\invalid-base.dll"}),
    });
    auto invalidBaseIdentity = Identity(
        "c:/game/invalid-base.dll", "invalid-base.dll");
    invalidBaseIdentity.moduleBase = 0;
    const AttributionResult invalidBaseAttribution = AttributeCapture(
        invalidBaseRegistry,
        CaptureSamples{
            9,
            {{invalidBaseIdentity, 9}},
            {{0, {0, 0, 0}, 9}},
        });
    ok &= Check(
        invalidBaseAttribution.exclusiveAssignedSamples == 0 &&
            invalidBaseAttribution.warnings.size() == 1 &&
            invalidBaseAttribution.warnings[0].code == "invalid-module-base",
        "zero module bases are skipped from direct attribution and lookup");

    const auto duplicateBaseRegistry = BuildRegistry({
        Definition("duplicate-base-layer", 1, AdapterType::SharedLayer, true,
                   {"C:\\Game\\first-shared.dll"}),
        Definition("duplicate-base-owner", 2, AdapterType::ModuleGroup, true,
                   {"C:\\Game\\second-owner.dll"}),
    });
    auto firstBaseIdentity = Identity(
        "c:/game/first-shared.dll", "first-shared.dll");
    auto secondBaseIdentity = Identity(
        "c:/game/second-owner.dll", "second-owner.dll");
    firstBaseIdentity.moduleBase = 0x9000;
    secondBaseIdentity.moduleBase = 0x9000;
    const AttributionResult duplicateBaseAttribution = AttributeCapture(
        duplicateBaseRegistry,
        CaptureSamples{
            7,
            {
                {firstBaseIdentity, 3},
                {secondBaseIdentity, 4},
            },
            {{0x9000, {0, 0, 0}, 5}},
        });
    ok &= Check(
        duplicateBaseAttribution.exclusiveAssignedSamples == 0 &&
            duplicateBaseAttribution.unresolvedSharedSamples == 5 &&
            duplicateBaseAttribution.unassignedSharedSamples == 0 &&
            duplicateBaseAttribution.warnings.size() == 1 &&
            duplicateBaseAttribution.warnings[0].code ==
                "duplicate-module-base",
        "duplicate nonzero bases keep the first module identity and sample row");

    const std::uint64_t maxSamples =
        std::numeric_limits<std::uint64_t>::max();
    const auto overflowExclusiveRegistry = BuildRegistry({
        Definition("overflow-owner", 1, AdapterType::ModuleGroup, true,
                   {"C:\\Game\\overflow-first.dll",
                    "C:\\Game\\overflow-second.dll"}),
    });
    auto overflowFirstIdentity = Identity(
        "c:/game/overflow-first.dll", "overflow-first.dll");
    auto overflowSecondIdentity = Identity(
        "c:/game/overflow-second.dll", "overflow-second.dll");
    overflowFirstIdentity.moduleBase = 0xa001;
    overflowSecondIdentity.moduleBase = 0xa002;
    const AttributionResult overflowExclusiveAttribution = AttributeCapture(
        overflowExclusiveRegistry,
        CaptureSamples{
            maxSamples,
            {
                {overflowFirstIdentity, maxSamples},
                {overflowSecondIdentity, 1},
            },
            {},
        });
    const AdapterSummary* overflowExclusiveSummary = FindSummary(
        overflowExclusiveAttribution,
        "overflow-owner");
    ok &= Check(
        overflowExclusiveSummary != nullptr &&
            overflowExclusiveSummary->exclusiveSamples == maxSamples &&
            overflowExclusiveAttribution.exclusiveAssignedSamples ==
                maxSamples &&
            overflowExclusiveAttribution.warnings.size() == 1 &&
            overflowExclusiveAttribution.warnings[0].code ==
                "sample-count-overflow",
        "exclusive sample totals saturate and warn on overflow");

    const auto overflowAttributedRegistry = BuildRegistry({
        Definition("overflow-attributed-owner", 1, AdapterType::ModuleGroup,
                   true, {"C:\\Game\\overflow-attributed-owner.dll"}),
        Definition("overflow-attributed-layer", 2, AdapterType::SharedLayer,
                   true, {"C:\\Game\\overflow-attributed-shared.dll"}),
    });
    auto overflowAttributedOwnerIdentity = Identity(
        "c:/game/overflow-attributed-owner.dll",
        "overflow-attributed-owner.dll");
    auto overflowAttributedSharedIdentity = Identity(
        "c:/game/overflow-attributed-shared.dll",
        "overflow-attributed-shared.dll");
    overflowAttributedOwnerIdentity.moduleBase = 0xa101;
    overflowAttributedSharedIdentity.moduleBase = 0xa102;
    const AttributionResult overflowAttributedAttribution = AttributeCapture(
        overflowAttributedRegistry,
        CaptureSamples{
            0,
            {
                {overflowAttributedOwnerIdentity, 0},
                {overflowAttributedSharedIdentity, 0},
            },
            {
                {0xa102, {0xa101, 0, 0}, maxSamples},
                {0xa102, {0xa101, 0, 0}, 1},
            },
        });
    const AdapterSummary* overflowAttributedSummary = FindSummary(
        overflowAttributedAttribution,
        "overflow-attributed-owner");
    ok &= Check(
        overflowAttributedSummary != nullptr &&
            overflowAttributedSummary->attributedSharedSamples == maxSamples &&
            overflowAttributedAttribution.attributedSharedSamples == maxSamples &&
            overflowAttributedAttribution.warnings.size() == 1 &&
            overflowAttributedAttribution.warnings[0].code ==
                "sample-count-overflow",
        "attributed shared totals saturate and warn on overflow");

    const auto overflowUnresolvedRegistry = BuildRegistry({
        Definition("overflow-unresolved-layer", 1, AdapterType::SharedLayer,
                   true, {"C:\\Game\\overflow-unresolved-shared.dll"}),
    });
    auto overflowUnresolvedIdentity = Identity(
        "c:/game/overflow-unresolved-shared.dll",
        "overflow-unresolved-shared.dll");
    overflowUnresolvedIdentity.moduleBase = 0xa201;
    const AttributionResult overflowUnresolvedAttribution = AttributeCapture(
        overflowUnresolvedRegistry,
        CaptureSamples{
            0,
            {{overflowUnresolvedIdentity, 0}},
            {
                {0xa201, {0, 0, 0}, maxSamples},
                {0xa201, {0, 0, 0}, 1},
            },
        });
    const AdapterSummary* overflowUnresolvedSummary = FindSummary(
        overflowUnresolvedAttribution,
        "overflow-unresolved-layer");
    ok &= Check(
        overflowUnresolvedSummary != nullptr &&
            overflowUnresolvedSummary->unresolvedSharedSamples == maxSamples &&
            overflowUnresolvedAttribution.unresolvedSharedSamples == maxSamples &&
            overflowUnresolvedAttribution.warnings.size() == 1 &&
            overflowUnresolvedAttribution.warnings[0].code ==
                "sample-count-overflow",
        "unresolved shared totals saturate and warn on overflow");

    const auto overflowUnassignedRegistry = BuildRegistry({
        Definition("overflow-unassigned-hybrid", 1, AdapterType::Hybrid, true,
                   {}, {"C:\\Game\\overflow-unassigned-shared.dll"}),
    });
    auto overflowUnassignedIdentity = Identity(
        "c:/game/overflow-unassigned-shared.dll",
        "overflow-unassigned-shared.dll");
    overflowUnassignedIdentity.moduleBase = 0xa301;
    const AttributionResult overflowUnassignedAttribution = AttributeCapture(
        overflowUnassignedRegistry,
        CaptureSamples{
            0,
            {{overflowUnassignedIdentity, 0}},
            {
                {0xa301, {0, 0, 0}, maxSamples},
                {0xa301, {0, 0, 0}, 1},
            },
        });
    ok &= Check(
        overflowUnassignedAttribution.unassignedSharedSamples == maxSamples &&
            overflowUnassignedAttribution.warnings.size() == 1 &&
            overflowUnassignedAttribution.warnings[0].code ==
                "sample-count-overflow",
        "unassigned shared totals saturate and warn on overflow");

    const auto overflowSortRegistry = BuildRegistry({
        Definition("late-overflow", 2, AdapterType::ModuleGroup, true,
                   {"C:\\Game\\late-overflow.dll"}),
        Definition("early-overflow", 1, AdapterType::Hybrid, true,
                   {"C:\\Game\\early-overflow.dll"},
                   {"C:\\Game\\early-overflow-shared.dll"}),
    });
    auto earlyOverflowIdentity = Identity(
        "c:/game/early-overflow.dll", "early-overflow.dll");
    auto lateOverflowIdentity = Identity(
        "c:/game/late-overflow.dll", "late-overflow.dll");
    auto earlyOverflowSharedIdentity = Identity(
        "c:/game/early-overflow-shared.dll", "early-overflow-shared.dll");
    earlyOverflowIdentity.moduleBase = 0xa401;
    lateOverflowIdentity.moduleBase = 0xa402;
    earlyOverflowSharedIdentity.moduleBase = 0xa403;
    const AttributionResult overflowSortAttribution = AttributeCapture(
        overflowSortRegistry,
        CaptureSamples{
            maxSamples,
            {
                {earlyOverflowIdentity, maxSamples - 5},
                {lateOverflowIdentity, maxSamples},
                {earlyOverflowSharedIdentity, 0},
            },
            {{0xa403, {0xa401, 0, 0}, 10}},
        });
    bool hasSummaryEvidenceOverflowWarning = false;
    for (const AdapterWarning& warning : overflowSortAttribution.warnings)
    {
        if (warning.code == "sample-count-overflow" &&
            warning.detail.find("summaryEvidence") != std::string::npos)
        {
            hasSummaryEvidenceOverflowWarning = true;
        }
    }
    ok &= Check(
        overflowSortAttribution.summaries.size() >= 2 &&
            overflowSortAttribution.summaries[0].name == "early-overflow" &&
            overflowSortAttribution.summaries[1].name == "late-overflow" &&
            hasSummaryEvidenceOverflowWarning,
        "summary evidence sorting saturates before registration-order tie-break");

    const auto disabledRegistry = BuildRegistry({
        Definition("disabled-owner", 1, AdapterType::ModuleGroup, false,
                   {"C:\\Game\\disabled.dll"}),
        Definition("disabled-layer", 2, AdapterType::SharedLayer, false, {},
                   {"C:\\Game\\disabled.dll"}),
    });
    const auto disabledIdentity = Identity("c:/game/disabled.dll", "disabled.dll");
    ok &= Check(
        FindExclusiveOwner(disabledRegistry, disabledIdentity) == nullptr &&
            FindSharedLayer(disabledRegistry, disabledIdentity) == nullptr,
        "disabled adapters claim nothing");

    const auto exactSharedRegistry = BuildRegistry({
        Definition("shared-layer-exact", 1, AdapterType::SharedLayer, true,
                   {"C:\\Game\\d3d9.dll"}),
    });
    ok &= Check(
        FindExclusiveOwner(exactSharedRegistry, d3d9Identity) == nullptr,
        "SharedLayer modules never become exclusive owners");
    ok &= Check(
        FindSharedLayer(exactSharedRegistry, d3d9Identity) != nullptr,
        "shared layer matches its exact module identity");
    ok &= Check(
        FindSharedLayer(
            exactSharedRegistry,
            Identity("c:/other/d3d9.dll", "d3d9.dll")) == nullptr,
        "SharedLayer full-path modules do not match another path");

    const auto basenameSharedRegistry = BuildRegistry({
        Definition("shared-layer-basename", 1, AdapterType::SharedLayer, true,
                   {"d3d9.dll"}),
    });
    ok &= Check(
        FindSharedLayer(
            basenameSharedRegistry,
            Identity("c:/other/d3d9.dll", "d3d9.dll")) != nullptr,
        "SharedLayer basename modules match both d3d9.dll paths");

    const auto hybridRegistry = BuildRegistry({
        Definition("hybrid", 1, AdapterType::Hybrid, true,
                   {"C:\\Game\\hybrid.dll"}, {"C:\\Game\\shared.dll"}),
    });
    ok &= Check(
        FindExclusiveOwner(
            hybridRegistry,
            Identity("c:/game/hybrid.dll", "hybrid.dll")) != nullptr,
        "hybrid modules are exclusive claims");
    ok &= Check(
        FindExclusiveOwner(
            hybridRegistry,
            Identity("c:/game/shared.dll", "shared.dll")) == nullptr,
        "hybrid sharedModules are never exclusive claims");

    const auto precedenceRegistry = BuildRegistry({
        Definition("basename-layer", 1, AdapterType::SharedLayer, true, {},
                   {"d3d9.dll"}),
        Definition("full-path-layer", 2, AdapterType::SharedLayer, true, {},
                   {"C:\\Game\\d3d9.dll"}),
    });
    const auto* exactMatch = FindSharedLayer(precedenceRegistry, d3d9Identity);
    const auto* otherPathMatch = FindSharedLayer(
        precedenceRegistry,
        Identity("c:/other/d3d9.dll", "d3d9.dll"));
    ok &= Check(
        exactMatch != nullptr && exactMatch->name == "full-path-layer",
        "exact full path outranks a basename match");
    ok &= Check(
        otherPathMatch != nullptr && otherPathMatch->name == "basename-layer",
        "two d3d9.dll full paths remain distinct");

    const auto exclusivePrecedenceRegistry = BuildRegistry({
        Definition("basename-owner", 1, AdapterType::ModuleGroup, true,
                   {"d3d9.dll"}),
        Definition("exact-owner", 2, AdapterType::ModuleGroup, true,
                   {"C:\\Game\\d3d9.dll"}),
    });
    const auto* exactOwner = FindExclusiveOwner(
        exclusivePrecedenceRegistry,
        d3d9Identity);
    const auto* otherBasenameOwner = FindExclusiveOwner(
        exclusivePrecedenceRegistry,
        Identity("c:/other/d3d9.dll", "d3d9.dll"));
    ok &= Check(
        exactOwner != nullptr && exactOwner->name == "exact-owner",
        "exact full-path ModuleGroup owner wins over basename owner");
    ok &= Check(
        otherBasenameOwner != nullptr &&
            otherBasenameOwner->name == "basename-owner",
        "basename ModuleGroup owner matches a different d3d9.dll path");

    ok &= Check(
        FindExclusiveOwner(
            registry,
            Identity("c:/missing/missing.dll", "missing.dll")) == nullptr &&
            FindSharedLayer(
                registry,
                Identity("c:/missing/missing.dll", "missing.dll")) == nullptr,
        "missing lookups return nullable null pointers");

    const auto properShaders = Definition(
        "ProperShaders", 1, AdapterType::Hybrid, true,
        {"C:\\Game\\ProperShaders.asi"},
        {"C:\\Game\\d3dx9_43.dll", "C:\\Game\\d3d9.dll"});
    const auto renderShared = Definition(
        "RenderShared", 6, AdapterType::SharedLayer, true,
        {"C:\\Game\\d3dx9_43.dll", "C:\\Game\\d3d9.dll"});
    const auto attributionRegistry = BuildRegistry({
        properShaders,
        renderShared,
    });

    auto properShadersIdentity = Identity(
        "c:/game/ProperShaders.asi", "ProperShaders.asi");
    auto d3dxIdentity = Identity(
        "c:/game/d3dx9_43.dll", "d3dx9_43.dll");
    auto gtaIdentity = Identity("c:/game/gta_sa.exe", "gta_sa.exe");
    auto d3d9IdentityWithBase = Identity(
        "c:/game/d3d9.dll", "d3d9.dll");
    properShadersIdentity.moduleBase = 0x1000;
    d3dxIdentity.moduleBase = 0x2000;
    d3d9IdentityWithBase.moduleBase = 0x3000;
    gtaIdentity.moduleBase = 0x4000;

    const CaptureSamples attributionCapture{
        100,
        {
            {properShadersIdentity, 10},
            {d3dxIdentity, 40},
            {gtaIdentity, 50},
            {d3d9IdentityWithBase, 0},
        },
        {
            {0x2000, {0x1000, 0x3000, 0x4000}, 25},
            {0x2000, {0x3000, 0x4000, 0}, 15},
            {0x4000, {0x1000, 0, 0}, 7},
        }};

    ok &= Check(
        IsSharedModule(attributionRegistry, d3dxIdentity) &&
            IsSharedModule(attributionRegistry, d3d9IdentityWithBase) &&
            !IsSharedModule(attributionRegistry, gtaIdentity),
        "shared module detection includes sharedModules and SharedLayer entries");

    const AttributionResult attribution = AttributeCapture(
        attributionRegistry,
        attributionCapture);
    const AdapterSummary* properSummary = FindSummary(
        attribution,
        "ProperShaders");
    const AdapterSummary* renderSummary = FindSummary(
        attribution,
        "RenderShared");
    ok &= Check(
        properSummary != nullptr && renderSummary != nullptr &&
            properSummary->registrationOrder == 1 &&
            properSummary->exclusiveSamples == 10 &&
            properSummary->attributedSharedSamples == 25 &&
            properSummary->unresolvedSharedSamples == 0 &&
            renderSummary->registrationOrder == 6 &&
            renderSummary->exclusiveSamples == 0 &&
            renderSummary->attributedSharedSamples == 0 &&
            renderSummary->unresolvedSharedSamples == 15,
        "shared stacks resolve to the first owned caller and unresolved stacks belong to the fallback layer");
    ok &= Check(
        attribution.rawTotalSamples == 100 &&
            attribution.exclusiveAssignedSamples == 10 &&
            attribution.attributedSharedSamples == 25 &&
            attribution.unresolvedSharedSamples == 15 &&
            attribution.unassignedSharedSamples == 0,
        "attribution preserves raw totals and does not double-count module samples");
    ok &= Check(
        duplicateNameAttribution.warnings.size() ==
                duplicateNameRegistry.warnings.size() &&
            duplicateNameAttribution.warnings.size() == 1 &&
            duplicateNameAttribution.warnings[0].code ==
                "duplicate-adapter-name",
        "registry warnings are copied into attribution results");
    ok &= Check(
        FindSummary(attribution, "missing-adapter") == nullptr,
        "missing attribution summaries return null");

    const auto firstCallerRegistry = BuildRegistry({
        Definition("first-owned", 1, AdapterType::ModuleGroup, true,
                   {"C:\\Game\\first.dll"}),
        Definition("second-owned", 2, AdapterType::ModuleGroup, true,
                   {"C:\\Game\\second.dll"}),
        Definition("shared-layer", 3, AdapterType::SharedLayer, true, {},
                   {"C:\\Game\\shared.dll"}),
    });
    auto firstSharedIdentity = Identity("c:/game/shared.dll", "shared.dll");
    auto firstOwnedIdentity = Identity("c:/game/first.dll", "first.dll");
    auto secondOwnedIdentity = Identity("c:/game/second.dll", "second.dll");
    firstSharedIdentity.moduleBase = 0x5000;
    firstOwnedIdentity.moduleBase = 0x1000;
    secondOwnedIdentity.moduleBase = 0x2000;
    const AttributionResult firstCallerAttribution = AttributeCapture(
        firstCallerRegistry,
        CaptureSamples{
            9,
            {
                {firstSharedIdentity, 9},
                {firstOwnedIdentity, 0},
                {secondOwnedIdentity, 0},
            },
            {{0x5000, {0x1000, 0x2000, 0}, 9}},
        });
    ok &= Check(
        FindSummary(firstCallerAttribution, "first-owned") != nullptr &&
            FindSummary(firstCallerAttribution, "first-owned")
                    ->attributedSharedSamples == 9 &&
            FindSummary(firstCallerAttribution, "second-owned")
                    ->attributedSharedSamples == 0,
        "the first owned caller wins when multiple owned callers appear");

    const auto nonSharedRegistry = BuildRegistry({
        Definition("owner", 1, AdapterType::ModuleGroup, true,
                   {"C:\\Game\\owner.dll"}),
    });
    auto nonSharedGtaIdentity = Identity(
        "c:/game/gta_sa.exe", "gta_sa.exe");
    nonSharedGtaIdentity.moduleBase = 0x4000;
    const AttributionResult nonSharedAttribution = AttributeCapture(
        nonSharedRegistry,
        CaptureSamples{
            4,
            {{nonSharedGtaIdentity, 4}},
            {{0x4000, {0x1000, 0, 0}, 4}},
        });
    ok &= Check(
        nonSharedAttribution.attributedSharedSamples == 0 &&
            nonSharedAttribution.unresolvedSharedSamples == 0 &&
            nonSharedAttribution.unassignedSharedSamples == 0,
        "a non-shared instruction module produces no shared attribution");

    const auto tieRegistry = BuildRegistry({
        Definition("late", 20, AdapterType::Hybrid, true,
                   {"C:\\Game\\late.dll"}, {"C:\\Game\\late-shared.dll"}),
        Definition("early", 10, AdapterType::Hybrid, true,
                   {"C:\\Game\\early.dll"}, {"C:\\Game\\early-shared.dll"}),
    });
    auto lateIdentity = Identity("c:/game/late.dll", "late.dll");
    auto earlyIdentity = Identity("c:/game/early.dll", "early.dll");
    auto lateSharedIdentity = Identity(
        "c:/game/late-shared.dll", "late-shared.dll");
    auto earlySharedIdentity = Identity(
        "c:/game/early-shared.dll", "early-shared.dll");
    lateIdentity.moduleBase = 0x1000;
    earlyIdentity.moduleBase = 0x2000;
    lateSharedIdentity.moduleBase = 0x6000;
    earlySharedIdentity.moduleBase = 0x7000;
    const AttributionResult tieAttribution = AttributeCapture(
        tieRegistry,
        CaptureSamples{
            10,
            {
                {lateIdentity, 5},
                {earlyIdentity, 5},
                {lateSharedIdentity, 0},
                {earlySharedIdentity, 0},
            },
            {
                {0x6000, {0x1000, 0, 0}, 5},
                {0x7000, {0x2000, 0, 0}, 5},
            },
        });
    ok &= Check(
        tieAttribution.summaries.size() >= 2 &&
            tieAttribution.summaries[0].name == "early" &&
            tieAttribution.summaries[1].name == "late",
        "summary evidence ties break by registration order after descending evidence");

    const auto noFallbackRegistry = BuildRegistry({
        Definition("hybrid-shared-only", 1, AdapterType::Hybrid, true,
                   {}, {"C:\\Game\\shared-only.dll"}),
    });
    auto sharedOnlyIdentity = Identity(
        "c:/game/shared-only.dll", "shared-only.dll");
    sharedOnlyIdentity.moduleBase = 0x8000;
    ok &= Check(
        IsSharedModule(noFallbackRegistry, sharedOnlyIdentity) &&
            FindSharedLayer(noFallbackRegistry, sharedOnlyIdentity) == nullptr,
        "Hybrid sharedModules-only matches have no SharedLayer fallback");
    const AttributionResult noFallbackAttribution = AttributeCapture(
        noFallbackRegistry,
        CaptureSamples{
            3,
            {{sharedOnlyIdentity, 3}},
            {{0x8000, {0, 0, 0}, 3}},
        });
    ok &= Check(
        noFallbackAttribution.unassignedSharedSamples == 3,
        "shared modules without a SharedLayer increment unassigned samples");

    const auto mixedClaimRegistry = BuildRegistry({
        Definition("exclusive-claim", 1, AdapterType::ModuleGroup, true,
                   {"C:\\Game\\mixed.dll"}, {"C:\\Game\\other.dll"}),
        Definition("shared-claim", 2, AdapterType::SharedLayer, true, {},
                   {"C:\\Game\\mixed.dll"}),
    });
    const auto mixedIdentity = Identity("c:/game/mixed.dll", "mixed.dll");
    ok &= Check(
        IsSharedModule(mixedClaimRegistry, mixedIdentity) &&
            FindExclusiveOwner(mixedClaimRegistry, mixedIdentity) == nullptr,
        "shared modules are never exclusive owners");

    const ProcessMemoryEvidence syntheticMemory = AccumulateMemoryRegions(
        0x1000,
        0x8000,
        {
            {0x1000, 0x1000, MemoryRegionState::Committed},
            {0x2000, 0x1000, MemoryRegionState::Reserved},
            {0x3000, 0x2000, MemoryRegionState::Free},
            {0x5000, 0x3000, MemoryRegionState::Free},
        });
    ok &= Check(
        syntheticMemory.virtualBytes == 0x2000 &&
            syntheticMemory.freeBytes == 0x5000 &&
            syntheticMemory.largestFreeRegion == 0x3000 &&
            syntheticMemory.userAddressSpaceBytes == 0x7000,
        "memory region aggregation counts non-free, free, largest-free, and user bounds exactly");

    const AttributionResult reportAttribution{
        {
            {"ProperShaders", 1, 10, 25, 0},
            {"RenderShared", 6, 0, 0, 15},
        },
        {},
        100,
        10,
        25,
        15,
        0,
    };
    const CaptureMetadata reportMetadata{7, 3000, 180, 60.0};
    const ProcessMemoryEvidence reportMemory{
        11,
        22,
        33,
        44,
        55,
        66,
        false,
    };
    const std::vector<AdapterMetricRow> reportMetrics{
        {"ProperShaders", "directBatchesPerFrame", 6661},
    };
    const std::vector<AdapterConfigRow> reportConfigs{
        {"Streaming", "StreamMemoryForced", "2000", "ImprovedStreaming.ini"},
    };
    const std::vector<AdapterWarning> reportWarnings{
        {"duplicate-owner", "Render\"Shared", "line one\nline two"},
    };
    FILE* reportFile = std::tmpfile();
    ok &= Check(reportFile != nullptr, "report test creates a temporary stream");
    if (reportFile != nullptr)
    {
        AppendAdapterReport(
            reportFile,
            reportMetadata,
            reportAttribution,
            reportMemory,
            reportMetrics,
            reportConfigs,
            reportWarnings);
        std::fflush(reportFile);
        std::fseek(reportFile, 0, SEEK_END);
        const long reportSize = std::ftell(reportFile);
        std::rewind(reportFile);
        std::string report;
        if (reportSize > 0)
        {
            report.resize(static_cast<std::size_t>(reportSize));
            std::fread(&report[0], 1, report.size(), reportFile);
        }
        std::fclose(reportFile);
        ok &= Check(
            report.find("adapterRank=1 name=ProperShaders") !=
                std::string::npos &&
                report.find("exclusivePercent=10.00") != std::string::npos &&
                report.find("attributedSharedPercent=25.00") !=
                    std::string::npos,
            "report keeps raw sample percentages auditable");
        ok &= Check(
            report.find(
                "adapterMetric name=ProperShaders key=directBatchesPerFrame value=6661") !=
                std::string::npos &&
                report.find(
                    "adapterConfig name=Streaming key=StreamMemoryForced value=2000 source=ImprovedStreaming.ini") !=
                    std::string::npos,
            "report emits metric and config rows");
        ok &= Check(
            report.find(
                "processMemory privateBytes=11 workingSet=22 virtualBytes=33 freeBytes=44 largestFreeRegion=55 userAddressSpaceBytes=66") !=
                std::string::npos,
            "report emits process-memory evidence");
        ok &= Check(
            report.find(
                "adapterWarning name=Render\\\"Shared code=duplicate-owner detail=\"line one\\nline two\"") !=
                std::string::npos,
            "report escapes quotes and newlines in warning fields");
    }

    static_assert(sizeof(Provider::Query) == 32);
    static_assert(offsetof(Provider::Query, frameCount) == 16);
    static_assert(offsetof(Provider::Query, fps) == 24);
    static_assert(sizeof(Provider::Metric) == 64);
    static_assert(offsetof(Provider::Metric, value) == 56);
    static_assert(sizeof(Provider::Snapshot) == 2064);

    Provider::Query providerQuery{};
    providerQuery.size = sizeof(providerQuery);
    providerQuery.apiVersion = Provider::ApiVersion;
    providerQuery.captureId = 7;
    providerQuery.elapsedMs = 3000;
    providerQuery.frameCount = 180;
    providerQuery.fps = 60.0;

    Provider::Snapshot validSnapshot{};
    validSnapshot.size = sizeof(validSnapshot);
    validSnapshot.apiVersion = Provider::ApiVersion;
    validSnapshot.metricCount = 2;
    FillMetric(
        validSnapshot.metrics[0],
        "draws",
        Provider::MetricKind::Counter,
        1);
    FillMetric(
        validSnapshot.metrics[1],
        "gpuUs",
        Provider::MetricKind::Microseconds,
        2);
    ok &= Check(
        Provider::ValidateSnapshot(validSnapshot).code ==
            Provider::ValidationCode::Valid,
        "valid Provider V1 snapshot passes pure validation");

    Provider::Snapshot wrongSize = validSnapshot;
    wrongSize.size = sizeof(wrongSize) - 1;
    ok &= Check(
        Provider::ValidateSnapshot(wrongSize).code ==
            Provider::ValidationCode::SnapshotSize,
        "snapshot size mismatch is rejected");
    Provider::Snapshot wrongVersion = validSnapshot;
    wrongVersion.apiVersion = Provider::ApiVersion + 1;
    ok &= Check(
        Provider::ValidateSnapshot(wrongVersion).code ==
            Provider::ValidationCode::SnapshotVersion,
        "snapshot version mismatch is rejected");
    Provider::Snapshot tooManyMetrics = validSnapshot;
    tooManyMetrics.metricCount = Provider::MaxMetrics + 1;
    ok &= Check(
        Provider::ValidateSnapshot(tooManyMetrics).code ==
            Provider::ValidationCode::MetricCount,
        "metric count over the fixed capacity is rejected");
    Provider::Snapshot emptyMetricName = validSnapshot;
    emptyMetricName.metrics[0].name[0] = '\0';
    ok &= Check(
        Provider::ValidateSnapshot(emptyMetricName).code ==
            Provider::ValidationCode::EmptyMetricName,
        "empty metric names are rejected");
    Provider::Snapshot unterminatedMetricName = validSnapshot;
    std::memset(
        unterminatedMetricName.metrics[0].name,
        'x',
        Provider::MetricNameCapacity);
    ok &= Check(
        Provider::ValidateSnapshot(unterminatedMetricName).code ==
            Provider::ValidationCode::UnterminatedMetricName,
        "unterminated metric names are rejected");
    Provider::Snapshot unknownKind = validSnapshot;
    unknownKind.metrics[0].kind = 99;
    ok &= Check(
        Provider::ValidateSnapshot(unknownKind).code ==
            Provider::ValidationCode::UnknownMetricKind,
        "unknown metric kinds are rejected");
    Provider::Snapshot nonzeroReserved = validSnapshot;
    nonzeroReserved.metrics[0].reserved = 1;
    ok &= Check(
        Provider::ValidateSnapshot(nonzeroReserved).code ==
            Provider::ValidationCode::NonzeroReserved,
        "nonzero reserved fields are rejected");

    Provider::ProviderHealth healthyState{};
    g_providerCallCount = 0;
    const Provider::CallResult healthyCall = Provider::Invoke(
        HealthyProvider,
        providerQuery,
        2000,
        healthyState);
    ok &= Check(
        healthyCall.status == Provider::CallStatus::Accepted &&
            healthyCall.called &&
            !healthyState.quarantined &&
            healthyCall.snapshot.metricCount == 2 &&
            g_providerCallCount == 1,
        "healthy provider is accepted and remains enabled");

    Provider::ProviderHealth emptyState{};
    g_providerCallCount = 0;
    const Provider::CallResult emptyCall = Provider::Invoke(
        EmptyProvider,
        providerQuery,
        2000,
        emptyState);
    ok &= Check(
        emptyCall.status == Provider::CallStatus::InvalidSnapshot &&
            emptyState.quarantined &&
            emptyCall.called &&
            g_providerCallCount == 1,
        "provider that returns an uninitialized snapshot is quarantined");

    Provider::ProviderHealth slowState{};
    g_providerCallCount = 0;
    const Provider::CallResult slowCall = Provider::Invoke(
        SlowProvider,
        providerQuery,
        2000,
        slowState);
    const Provider::CallResult slowRetry = Provider::Invoke(
        SlowProvider,
        providerQuery,
        2000,
        slowState);
    ok &= Check(
        slowCall.status == Provider::CallStatus::SlowProvider &&
            slowCall.called &&
            slowState.quarantined &&
            slowRetry.status == Provider::CallStatus::SkippedQuarantined &&
            !slowRetry.called &&
            g_providerCallCount == 1,
        "slow provider is quarantined and never queried twice");

    Provider::ProviderHealth crashingState{};
    g_providerCallCount = 0;
    const Provider::CallResult crashingCall = Provider::Invoke(
        CrashingProvider,
        providerQuery,
        2000,
        crashingState);
    const Provider::CallResult crashingRetry = Provider::Invoke(
        CrashingProvider,
        providerQuery,
        2000,
        crashingState);
    ok &= Check(
        crashingCall.status == Provider::CallStatus::ProviderException &&
            crashingCall.called &&
            crashingState.quarantined &&
            crashingRetry.status == Provider::CallStatus::SkippedQuarantined &&
            !crashingRetry.called &&
            g_providerCallCount == 1,
        "crashing provider is contained and quarantined");

    Provider::ProviderHealth invalidFunctionState{};
    const Provider::CallResult invalidFunctionCall = Provider::Invoke(
        nullptr,
        providerQuery,
        2000,
        invalidFunctionState);
    ok &= Check(
        invalidFunctionCall.status == Provider::CallStatus::InvalidFunction &&
            invalidFunctionState.quarantined &&
            !invalidFunctionCall.called,
        "null provider exports fail closed");

    if (!ok) return 1;
    std::puts("PASS: BridgeD3D9 performance adapters");
    return 0;
}
