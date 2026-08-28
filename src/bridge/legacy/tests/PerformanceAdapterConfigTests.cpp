#include "../PerformanceAdapterConfig.h"

#include "../PerformanceAdapters.h"

#include <cstdio>
#include <fstream>
#include <string>

namespace
{
bool Check(bool condition, const char* message)
{
    if (condition)
    {
        return true;
    }

    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

bool HasWarning(
    const std::vector<BridgePerformance::AdapterWarning>& warnings,
    const char* code,
    const char* adapter = nullptr)
{
    for (const BridgePerformance::AdapterWarning& warning : warnings)
    {
        if (warning.code == code &&
            (adapter == nullptr || warning.adapter == adapter))
        {
            return true;
        }
    }

    return false;
}

bool HasWarning(
    const BridgePerformance::AdapterRuntimeConfig& config,
    const char* code,
    const char* adapter = nullptr)
{
    return HasWarning(config.warnings, code, adapter);
}

const BridgePerformance::AdapterDefinition* FindAdapter(
    const BridgePerformance::AdapterRuntimeConfig& config,
    const char* name)
{
    for (const BridgePerformance::AdapterDefinition& adapter :
         config.registry.adapters)
    {
        if (adapter.name == name)
        {
            return &adapter;
        }
    }

    return nullptr;
}
}

int main()
{
    using namespace BridgePerformance;

    const std::string fixturePath =
        "D:\\GTA San Andreas\\Temp\\BridgeD3D9Tests\\"
        "PerformanceAdapterConfigTests.ini";
    const std::string gameDirectory =
        "D:\\GTA San Andreas\\FixtureGame\\..\\FixtureGame";

    const char* fixture =
        "; parser fixture\n"
        "[PerformanceDiagnostics]\n"
        "Enable=1\n"
        "IncludeProcessMemory=0\n"
        "IncludeConfigSnapshots=1\n"
        "EnableProviders=0\n"
        "ProviderSlowWarningUs=2000\n"
        "\n"
        "[PerformanceRegister]\n"
        "1=Good\n"
        "2=\n"
        "3=MissingSection\n"
        "4=UnknownType\n"
        "5=Malformed\n"
        "6=Disabled\n"
        "7=DuplicateName\n"
        "8=duplicatename\n"
        "9=OwnerOne\n"
        "10=OwnerTwo\n"
        "11=Snapshot\n"
        "12=SharedLayer\n"
        "129=Ignored\n"
        "\n"
        "[Performance.Good]\n"
        "Enable=1\n"
        "Type=hYbRiD\n"
        "Modules= .\\Mods\\Good.ASI ; GOOD2.DLL ; ;\n"
        "SharedModules=Shared\\Layer.DLL; d3d9.dll\n"
        "ProviderExport=BridgePerf_QueryV1\n"
        "Config1=modloader\\Good\\Good.ini\n"
        "Config2=C:\\External\\Good.ini\n"
        "Config3=\\\\Server\\Share\\Good.ini\n"
        "Config17=ignored.ini\n"
        "\n"
        "[Performance.UnknownType]\n"
        "Enable=1\n"
        "Type=NotAType\n"
        "Modules=unknown.dll\n"
        "\n"
        "[Performance.Malformed]\n"
        "Enable=1\n"
        "Type=ModuleGroup\n"
        "Modules=valid.dll;;\n"
        "\n"
        "[Performance.Disabled]\n"
        "Enable=0\n"
        "Type=ModuleGroup\n"
        "Modules=disabled.dll\n"
        "\n"
        "[Performance.DuplicateName]\n"
        "Enable=1\n"
        "Type=ModuleGroup\n"
        "Modules=duplicate-name.dll\n"
        "\n"
        "[Performance.duplicatename]\n"
        "Enable=1\n"
        "Type=ModuleGroup\n"
        "Modules=other-duplicate-name.dll\n"
        "\n"
        "[Performance.OwnerOne]\n"
        "Enable=1\n"
        "Type=ModuleGroup\n"
        "Modules=\\\\Shared\\\\Owned.DLL\n"
        "\n"
        "[Performance.OwnerTwo]\n"
        "Enable=1\n"
        "Type=ModuleGroup\n"
        "Modules=\\\\shared\\owned.dll\n"
        "\n"
        "[Performance.Snapshot]\n"
        "Enable=1\n"
        "Type=CONFIGsnapshot\n"
        "Config1=snapshots\\state.ini\n"
        "\n"
        "[Performance.SharedLayer]\n"
        "Enable=1\n"
        "Type=sharedLAYER\n"
        "Modules=shared-layer.dll\n";

    std::ofstream output(fixturePath, std::ios::binary | std::ios::trunc);
    bool ok = Check(output.good(), "temporary INI fixture opens");
    if (ok)
    {
        output << fixture;
        ok &= Check(output.good(), "temporary INI fixture is written");
    }
    output.close();

    const AdapterRuntimeConfig config = LoadAdapterConfig(
        fixturePath,
        gameDirectory);
    std::remove(fixturePath.c_str());

    ok &= Check(config.enabled, "PerformanceDiagnostics Enable parses exactly");
    ok &= Check(
        !config.includeProcessMemory,
        "IncludeProcessMemory parses exactly");
    ok &= Check(
        config.includeConfigSnapshots,
        "IncludeConfigSnapshots parses exactly");
    ok &= Check(
        !config.enableProviders,
        "EnableProviders parses exactly");
    ok &= Check(
        config.providerSlowWarningUs == 2000,
        "ProviderSlowWarningUs parses exactly");

    const AdapterDefinition* good = FindAdapter(config, "Good");
    ok &= Check(good != nullptr, "valid adapter survives parsing");
    if (good != nullptr)
    {
        ok &= Check(
            good->registrationOrder == 1,
            "registration order equals numeric key");
        ok &= Check(
            good->type == AdapterType::Hybrid,
            "Type accepts case-insensitive Hybrid");
        ok &= Check(good->enabled, "adapter Enable parses exactly");
        ok &= Check(
            good->modules.size() == 2 &&
                good->modules[0] == "mods/good.asi" &&
                good->modules[1] == "good2.dll",
            "module tokens trim and normalize through core helpers");
        ok &= Check(
            good->sharedModules.size() == 2 &&
                good->sharedModules[0] == "shared/layer.dll" &&
                good->sharedModules[1] == "d3d9.dll",
            "shared module tokens parse and normalize");
        ok &= Check(
            good->providerExport == "BridgePerf_QueryV1",
            "ProviderExport parses exactly");
        ok &= Check(
            good->configPaths.size() == 3 &&
                good->configPaths[0] ==
                    "d:/gta san andreas/fixturegame/modloader/good/good.ini" &&
                good->configPaths[1] == "c:/external/good.ini" &&
                good->configPaths[2] == "//server/share/good.ini",
            "Config1..Config16 resolve relative paths and preserve absolutes");
    }

    ok &= Check(
        FindAdapter(config, "MissingSection") == nullptr,
        "missing adapter section is skipped");
    ok &= Check(
        FindAdapter(config, "UnknownType") == nullptr,
        "unknown adapter type is skipped fail-closed");
    ok &= Check(
        HasWarning(config, "empty-registration"),
        "missing registration section emits empty-registration");
    ok &= Check(
        HasWarning(config, "missing-section", "MissingSection"),
        "missing adapter section emits missing-section");
    ok &= Check(
        HasWarning(config, "unknown-type", "UnknownType"),
        "unknown type emits unknown-type");
    ok &= Check(
        HasWarning(config, "empty-module-token", "Malformed"),
        "empty module token emits empty-module-token");

    const AdapterDefinition* disabled = FindAdapter(config, "Disabled");
    ok &= Check(
        disabled != nullptr && !disabled->enabled,
        "disabled adapter remains defined");
    if (disabled != nullptr)
    {
        const AdapterRegistry disabledRegistry = BuildRegistry({*disabled});
        const ModuleIdentity disabledModule{
            "disabled.dll", "disabled.dll", 1, 1};
        ok &= Check(
            FindExclusiveOwner(disabledRegistry, disabledModule) == nullptr,
            "disabled adapter claims nothing");
    }

    ok &= Check(
        FindAdapter(config, "DuplicateName") != nullptr &&
            FindAdapter(config, "duplicatename") == nullptr,
        "duplicate adapter names are rejected by BuildRegistry");
    ok &= Check(
        HasWarning(config, "duplicate-adapter-name", "duplicatename"),
        "duplicate adapter name warning propagates");
    ok &= Check(
        HasWarning(config, "duplicate-owner", "OwnerTwo"),
        "duplicate exclusive ownership warning propagates");

    const AdapterDefinition* snapshot = FindAdapter(config, "Snapshot");
    const AdapterDefinition* sharedLayer = FindAdapter(config, "SharedLayer");
    ok &= Check(
        snapshot != nullptr && snapshot->type == AdapterType::ConfigSnapshot,
        "Type accepts case-insensitive ConfigSnapshot");
    ok &= Check(
        sharedLayer != nullptr && sharedLayer->type == AdapterType::SharedLayer,
        "Type accepts case-insensitive SharedLayer");

    const AdapterDefinition* ownerOne = FindAdapter(config, "OwnerOne");
    const AdapterDefinition* ownerTwo = FindAdapter(config, "OwnerTwo");
    ok &= Check(
        ownerOne != nullptr && ownerTwo != nullptr &&
            ownerOne->registrationOrder == 9 &&
            ownerTwo->registrationOrder == 10,
        "numeric registration keys preserve order across gaps");

    const auto writeFixture = [&](const std::string& path,
                                  const std::string& contents)
    {
        std::ofstream fixtureOutput(
            path,
            std::ios::binary | std::ios::trunc);
        bool fixtureOk = Check(
            fixtureOutput.good(),
            "quality fixture opens");
        if (fixtureOk)
        {
            fixtureOutput << contents;
            fixtureOk &= Check(
                fixtureOutput.good(),
                "quality fixture is written");
        }
        fixtureOutput.close();
        return fixtureOk;
    };

    const std::string giantFixturePath =
        "D:\\GTA San Andreas\\Temp\\BridgeD3D9Tests\\"
        "PerformanceAdapterConfigTestsGiant.ini";
    const std::string giantFixture =
        "[PerformanceRegister]\n"
        "1=Giant\n"
        "[Performance.Giant]\n"
        "Type=ModuleGroup\n"
        "Modules=" + std::string(70u * 1024u, 'x') + "\n";
    if (writeFixture(giantFixturePath, giantFixture))
    {
        const AdapterRuntimeConfig giant = LoadAdapterConfig(
            giantFixturePath,
            gameDirectory);
        ok &= Check(
            HasWarning(giant, "section-truncated", "Giant"),
            "oversized adapter section emits section-truncated");
        ok &= Check(
            !HasWarning(giant, "missing-section", "Giant"),
            "oversized adapter section is not reported missing");
    }
    std::remove(giantFixturePath.c_str());

    const std::string missingSectionsFixturePath =
        "D:\\GTA San Andreas\\Temp\\BridgeD3D9Tests\\"
        "PerformanceAdapterConfigTestsMissingSections.ini";
    if (writeFixture(missingSectionsFixturePath, "[Other]\nKey=Value\n"))
    {
        const AdapterRuntimeConfig missingSections = LoadAdapterConfig(
            missingSectionsFixturePath,
            gameDirectory);
        ok &= Check(
            HasWarning(missingSections, "missing-diagnostics-section"),
            "missing diagnostics section emits a distinct warning");
        ok &= Check(
            HasWarning(missingSections, "missing-register-section"),
            "missing register section emits a distinct warning");
        ok &= Check(
            !missingSections.enabled &&
                missingSections.includeProcessMemory &&
                missingSections.includeConfigSnapshots &&
                missingSections.enableProviders &&
                missingSections.providerSlowWarningUs == 2000 &&
                missingSections.registry.adapters.empty(),
            "missing sections retain safe defaults and empty registry");
    }
    std::remove(missingSectionsFixturePath.c_str());

    const std::string invalidValuesFixturePath =
        "D:\\GTA San Andreas\\Temp\\BridgeD3D9Tests\\"
        "PerformanceAdapterConfigTestsInvalidValues.ini";
    const char* invalidValuesFixture =
        "[PerformanceDiagnostics]\n"
        "Enable=maybe\n"
        "EnableProviders=wat\n"
        "ProviderSlowWarningUs=abc\n"
        "[PerformanceRegister]\n"
        "1=Invalid\n"
        "[Performance.Invalid]\n"
        "Type=ModuleGroup\n"
        "Enable=maybe\n"
        "Modules=valid.dll\n";
    if (writeFixture(invalidValuesFixturePath, invalidValuesFixture))
    {
        const AdapterRuntimeConfig invalidValues = LoadAdapterConfig(
            invalidValuesFixturePath,
            gameDirectory);
        const AdapterDefinition* invalid = FindAdapter(
            invalidValues,
            "Invalid");
        ok &= Check(
            HasWarning(invalidValues, "invalid-boolean"),
            "invalid boolean values emit invalid-boolean");
        ok &= Check(
            HasWarning(invalidValues, "invalid-number"),
            "invalid numeric values emit invalid-number");
        ok &= Check(
            !invalidValues.enabled &&
                !invalidValues.enableProviders &&
                invalidValues.includeProcessMemory &&
                invalidValues.includeConfigSnapshots &&
                invalidValues.providerSlowWarningUs == 2000,
            "invalid diagnostics values fail closed or retain defaults");
        ok &= Check(
            invalid != nullptr && !invalid->enabled,
            "invalid adapter Enable fails closed");
    }
    std::remove(invalidValuesFixturePath.c_str());

    const std::string invalidRegistrationsFixturePath =
        "D:\\GTA San Andreas\\Temp\\BridgeD3D9Tests\\"
        "PerformanceAdapterConfigTestsInvalidRegistrations.ini";
    const char* invalidRegistrationsFixture =
        "[PerformanceRegister]\n"
        "not-a-number=Foo\n"
        "129=OutOfRange\n"
        "1=A\n"
        "1=B\n"
        "[Performance.A]\n"
        "Type=ModuleGroup\n"
        "Modules=a.dll\n"
        "[Performance.B]\n"
        "Type=ModuleGroup\n"
        "Modules=b.dll\n";
    if (writeFixture(
            invalidRegistrationsFixturePath,
            invalidRegistrationsFixture))
    {
        const AdapterRuntimeConfig invalidRegistrations = LoadAdapterConfig(
            invalidRegistrationsFixturePath,
            gameDirectory);
        const AdapterDefinition* first = FindAdapter(
            invalidRegistrations,
            "A");
        ok &= Check(
            HasWarning(invalidRegistrations, "invalid-registration-key"),
            "malformed registration keys emit invalid-registration-key");
        ok &= Check(
            HasWarning(invalidRegistrations, "registration-out-of-range"),
            "out-of-range registrations emit registration-out-of-range");
        ok &= Check(
            HasWarning(invalidRegistrations, "duplicate-registration-key"),
            "duplicate registrations emit duplicate-registration-key");
        ok &= Check(
            first != nullptr && first->registrationOrder == 1 &&
                first->enabled &&
                FindAdapter(invalidRegistrations, "B") == nullptr,
            "first duplicate registration wins and defaults enabled");
    }
    std::remove(invalidRegistrationsFixturePath.c_str());

    const std::string snapshotFixturePath =
        "D:\\GTA San Andreas\\Temp\\BridgeD3D9Tests\\"
        "PerformanceAdapterConfigTestsSnapshot.ini";
    const char* snapshotFixture =
        "; whole-line comment\n"
        "# another whole-line comment\n"
        "[First]\n"
        "Key1=one ; inline comment\n"
        "Quoted=\"keep; # inside quotes\" # trailing comment\n"
        "Blank=\n"
        "[First]\n"
        "Key2=two\n"
        "not-a-key-line\n"
        "[Second]\n"
        "Value=three\n";
    if (writeFixture(snapshotFixturePath, snapshotFixture))
    {
        const ConfigSnapshot snapshotResult = ReadConfigSnapshot(
            snapshotFixturePath);
        ok &= Check(
            snapshotResult.warnings.empty() &&
                !snapshotResult.truncated,
            "config snapshot reads a normal INI without warnings");
        ok &= Check(
            snapshotResult.entries.size() == 5,
            "config snapshot keeps active keys in input order");
        if (snapshotResult.entries.size() == 5)
        {
            ok &= Check(
                snapshotResult.entries[0].section == "First" &&
                    snapshotResult.entries[0].key == "Key1" &&
                    snapshotResult.entries[0].value == "one",
                "snapshot strips a semicolon inline comment");
            ok &= Check(
                snapshotResult.entries[1].value ==
                    "keep; # inside quotes",
                "snapshot preserves comment markers inside quotes");
            ok &= Check(
                snapshotResult.entries[3].section == "First" &&
                    snapshotResult.entries[3].key == "Key2" &&
                    snapshotResult.entries[4].section == "Second",
                "repeated sections remain section-qualified and ordered");
            ok &= Check(
                snapshotResult.entries[0].source.find(":4") !=
                    std::string::npos,
                "snapshot source records the source line");
        }
    }
    std::remove(snapshotFixturePath.c_str());

    const std::string snapshotLimitFixturePath =
        "D:\\GTA San Andreas\\Temp\\BridgeD3D9Tests\\"
        "PerformanceAdapterConfigTestsSnapshotLimit.ini";
    const char* snapshotLimitFixture =
        "[Limit]\n"
        "A=1\n"
        "B=2\n"
        "C=3\n";
    if (writeFixture(snapshotLimitFixturePath, snapshotLimitFixture))
    {
        const ConfigSnapshotLimits entryLimits{1024u, 2u};
        const ConfigSnapshot limitedByEntries = ReadConfigSnapshot(
            snapshotLimitFixturePath,
            entryLimits);
        ok &= Check(
            limitedByEntries.truncated &&
                HasWarning(limitedByEntries.warnings, "snapshot-truncated"),
            "snapshot stops at the active-entry limit");
        ok &= Check(
            limitedByEntries.entries.size() == 2,
            "snapshot entry limit does not emit a partial third row");

        const ConfigSnapshotLimits byteLimits{10u, 512u};
        const ConfigSnapshot limitedByBytes = ReadConfigSnapshot(
            snapshotLimitFixturePath,
            byteLimits);
        ok &= Check(
            limitedByBytes.truncated &&
                !limitedByBytes.warnings.empty(),
            "snapshot stops at the byte limit");
    }
    std::remove(snapshotLimitFixturePath.c_str());

    const ConfigSnapshot missingSnapshot = ReadConfigSnapshot(
        "D:\\GTA San Andreas\\Temp\\BridgeD3D9Tests\\does-not-exist.ini");
        ok &= Check(
        HasWarning(missingSnapshot.warnings, "missing-file"),
        "missing snapshot file emits missing-file");

    return ok ? 0 : 1;
}
