#include "PerformanceAdapterConfig.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <fstream>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace BridgePerformance
{
namespace
{
constexpr DWORD InitialSectionCapacity = 256;
constexpr DWORD MaximumSectionCapacity = 64u * 1024u;

struct SectionEntry
{
    std::string key;
    std::string value;
};

struct SectionData
{
    bool present = false;
    bool truncated = false;
    std::vector<SectionEntry> entries;
};

struct RawSectionInfo
{
    bool present = false;
    bool truncated = false;
};

bool IsAsciiWhitespace(const char character)
{
    return character == ' ' || character == '\t' ||
        character == '\r' || character == '\n' ||
        character == '\f' || character == '\v';
}

std::string TrimAscii(const std::string& value)
{
    std::size_t first = 0;
    while (first < value.size() && IsAsciiWhitespace(value[first]))
    {
        ++first;
    }

    std::size_t last = value.size();
    while (last > first && IsAsciiWhitespace(value[last - 1]))
    {
        --last;
    }

    return value.substr(first, last - first);
}

char LowerAscii(const char character)
{
    return character >= 'A' && character <= 'Z'
        ? static_cast<char>(character - 'A' + 'a')
        : character;
}

bool EqualsInsensitive(
    const std::string& left,
    const char* right)
{
    std::size_t position = 0;
    while (right[position] != '\0' && position < left.size() &&
           LowerAscii(left[position]) == LowerAscii(right[position]))
    {
        ++position;
    }

    return position == left.size() && right[position] == '\0';
}

bool IsReadableFile(const std::string& path)
{
    const DWORD attributes = GetFileAttributesA(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
    {
        return false;
    }

    const HANDLE file = CreateFileA(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    CloseHandle(file);
    return true;
}

bool TryReadSectionHeader(
    const std::string& line,
    std::string& sectionName)
{
    const std::string trimmed = TrimAscii(line);
    if (trimmed.size() < 2 || trimmed.front() != '[' ||
        trimmed.back() != ']')
    {
        return false;
    }

    sectionName = TrimAscii(trimmed.substr(1, trimmed.size() - 2));
    return !sectionName.empty();
}

RawSectionInfo ScanRawSection(
    const std::string& iniPath,
    const std::string& section)
{
    RawSectionInfo result;
    std::ifstream input(iniPath, std::ios::binary);
    if (!input.good())
    {
        return result;
    }

    bool inTargetSection = false;
    std::size_t sectionBytes = 0;
    std::string line;
    while (std::getline(input, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }

        std::string sectionName;
        if (TryReadSectionHeader(line, sectionName))
        {
            if (inTargetSection)
            {
                break;
            }

            inTargetSection = EqualsInsensitive(sectionName, section.c_str());
            if (inTargetSection)
            {
                result.present = true;
                sectionBytes = 0;
            }
            continue;
        }

        if (!inTargetSection)
        {
            continue;
        }

        const std::size_t lineBytes = line.size() + 1;
        const std::size_t maximumSectionBytes =
            static_cast<std::size_t>(MaximumSectionCapacity);
        if (lineBytes > maximumSectionBytes - sectionBytes)
        {
            result.truncated = true;
            return result;
        }
        sectionBytes += lineBytes;
    }

    return result;
}

SectionData ReadSection(
    const std::string& iniPath,
    const std::string& section)
{
    SectionData result;
    const RawSectionInfo raw = ScanRawSection(iniPath, section);
    if (!raw.present)
    {
        return result;
    }
    if (raw.truncated)
    {
        result.present = true;
        result.truncated = true;
        return result;
    }

    DWORD capacity = InitialSectionCapacity;
    std::vector<char> buffer;

    for (;;)
    {
        buffer.assign(capacity, '\0');
        const DWORD length = GetPrivateProfileSectionA(
            section.c_str(),
            buffer.data(),
            capacity,
            iniPath.c_str());
        if (length == 0)
        {
            return result;
        }

        result.present = true;

        if (length < capacity - 2 || capacity == MaximumSectionCapacity)
        {
            if (length >= capacity - 2)
            {
                result.truncated = true;
                return result;
            }

            std::size_t position = 0;
            while (position < length)
            {
                const std::size_t remaining = length - position;
                const std::size_t recordLength =
                    std::char_traits<char>::length(buffer.data() + position);
                if (recordLength > remaining)
                {
                    break;
                }

                const std::string record(
                    buffer.data() + position,
                    recordLength);
                const std::size_t equals = record.find('=');
                if (equals == std::string::npos)
                {
                    result.entries.push_back({TrimAscii(record), std::string()});
                }
                else
                {
                    result.entries.push_back({
                        TrimAscii(record.substr(0, equals)),
                        record.substr(equals + 1)});
                }
                position += recordLength + 1;
            }
            return result;
        }

        capacity = std::min(
            MaximumSectionCapacity,
            capacity > MaximumSectionCapacity / 2
                ? MaximumSectionCapacity
                : capacity * 2);
    }
}

void AddWarning(
    AdapterRuntimeConfig& config,
    const char* code,
    const std::string& adapter,
    const std::string& detail)
{
    config.warnings.push_back({code, adapter, detail});
}

const SectionEntry* FindEntry(
    const SectionData& section,
    const char* key)
{
    for (const SectionEntry& entry : section.entries)
    {
        if (EqualsInsensitive(entry.key, key))
        {
            return &entry;
        }
    }
    return nullptr;
}

bool ParseBoolean(
    const SectionData& section,
    const char* key,
    const bool defaultValue,
    bool& valid)
{
    valid = true;
    const SectionEntry* entry = FindEntry(section, key);
    if (entry == nullptr)
    {
        return defaultValue;
    }

    const std::string value = TrimAscii(entry->value);
    if (value == "1" || EqualsInsensitive(value, "true") ||
        EqualsInsensitive(value, "yes") || EqualsInsensitive(value, "on"))
    {
        return true;
    }
    if (value == "0" || EqualsInsensitive(value, "false") ||
        EqualsInsensitive(value, "no") || EqualsInsensitive(value, "off"))
    {
        return false;
    }

    valid = false;
    return false;
}

bool ParseUnsigned32(
    const SectionData& section,
    const char* key,
    std::uint32_t& output)
{
    const SectionEntry* entry = FindEntry(section, key);
    if (entry == nullptr)
    {
        return true;
    }

    const std::string value = TrimAscii(entry->value);
    if (value.empty())
    {
        return false;
    }

    std::uint64_t parsed = 0;
    for (const char character : value)
    {
        if (character < '0' || character > '9')
        {
            return false;
        }
        const std::uint64_t digit =
            static_cast<std::uint64_t>(character - '0');
        if (parsed >
            (std::numeric_limits<std::uint32_t>::max() - digit) / 10u)
        {
            return false;
        }
        parsed = parsed * 10u + digit;
    }

    output = static_cast<std::uint32_t>(parsed);
    return true;
}

enum class RegistrationKeyResult
{
    Valid,
    Invalid,
    OutOfRange,
};

RegistrationKeyResult ParseRegistrationNumber(
    const std::string& key,
    unsigned& output)
{
    const std::string trimmed = TrimAscii(key);
    if (trimmed.empty())
    {
        return RegistrationKeyResult::Invalid;
    }

    unsigned parsed = 0;
    for (const char character : trimmed)
    {
        if (character < '0' || character > '9')
        {
            return RegistrationKeyResult::Invalid;
        }
        const unsigned digit = static_cast<unsigned>(character - '0');
        if (parsed > (128u - digit) / 10u)
        {
            return RegistrationKeyResult::OutOfRange;
        }
        parsed = parsed * 10u + digit;
    }

    if (parsed == 0)
    {
        return RegistrationKeyResult::OutOfRange;
    }

    output = parsed;
    return RegistrationKeyResult::Valid;
}

AdapterType ParseAdapterType(
    const std::string& value,
    bool& valid)
{
    const std::string trimmed = TrimAscii(value);
    valid = true;
    if (EqualsInsensitive(trimmed, "ModuleGroup"))
    {
        return AdapterType::ModuleGroup;
    }
    if (EqualsInsensitive(trimmed, "Hybrid"))
    {
        return AdapterType::Hybrid;
    }
    if (EqualsInsensitive(trimmed, "ConfigSnapshot"))
    {
        return AdapterType::ConfigSnapshot;
    }
    if (EqualsInsensitive(trimmed, "SharedLayer"))
    {
        return AdapterType::SharedLayer;
    }

    valid = false;
    return AdapterType::ModuleGroup;
}

void ParseModuleList(
    const SectionData& section,
    const char* key,
    const std::string& adapterName,
    AdapterRuntimeConfig& config,
    std::vector<std::string>& output)
{
    const SectionEntry* entry = FindEntry(section, key);
    if (entry == nullptr)
    {
        return;
    }

    const std::string value = entry->value;
    const bool hasSeparator = value.find(';') != std::string::npos;
    std::size_t start = 0;
    for (;;)
    {
        const std::size_t separator = value.find(';', start);
        const std::string token = TrimAscii(value.substr(
            start,
            separator == std::string::npos
                ? std::string::npos
                : separator - start));
        if (token.empty())
        {
            if (hasSeparator)
            {
                AddWarning(
                    config,
                    "empty-module-token",
                    adapterName,
                    std::string("empty item in ") + key);
            }
        }
        else
        {
            output.push_back(NormalizeModulePath(token));
        }

        if (separator == std::string::npos)
        {
            break;
        }
        start = separator + 1;
    }
}

bool IsAbsoluteWindowsPath(const std::string& path)
{
    if (!path.empty() && (path.front() == '/' || path.front() == '\\'))
    {
        return true;
    }

    return path.size() >= 3 &&
        path[1] == ':' && (path[2] == '/' || path[2] == '\\');
}

std::string ResolveConfigPath(
    const std::string& value,
    const std::string& gameDirectory)
{
    const std::string trimmed = TrimAscii(value);
    if (IsAbsoluteWindowsPath(trimmed))
    {
        return NormalizeModulePath(trimmed);
    }

    const std::string root = NormalizeModulePath(gameDirectory);
    if (root.empty())
    {
        return NormalizeModulePath(trimmed);
    }

    return NormalizeModulePath(root + "/" + trimmed);
}

void ParseConfigPaths(
    const SectionData& section,
    const std::string& gameDirectory,
    AdapterDefinition& definition)
{
    for (unsigned index = 1; index <= 16; ++index)
    {
        const std::string key = "Config" + std::to_string(index);
        const SectionEntry* entry = FindEntry(section, key.c_str());
        if (entry == nullptr)
        {
            continue;
        }

        const std::string value = TrimAscii(entry->value);
        if (!value.empty())
        {
            definition.configPaths.push_back(
                ResolveConfigPath(value, gameDirectory));
        }
    }
}

std::string StripSnapshotComment(const std::string& value)
{
    bool quoted = false;
    char quote = '\0';
    bool escaped = false;
    for (std::size_t position = 0; position < value.size(); ++position)
    {
        const char character = value[position];
        if (quoted)
        {
            if (escaped)
            {
                escaped = false;
            }
            else if (character == '\\')
            {
                escaped = true;
            }
            else if (character == quote)
            {
                quoted = false;
                quote = '\0';
            }
            continue;
        }

        if (character == '"' || character == '\'')
        {
            quoted = true;
            quote = character;
        }
        else if (character == ';' || character == '#')
        {
            return value.substr(0, position);
        }
    }

    return value;
}

std::string UnquoteSnapshotValue(const std::string& value)
{
    const std::string trimmed = TrimAscii(value);
    if (trimmed.size() >= 2 &&
        ((trimmed.front() == '"' && trimmed.back() == '"') ||
         (trimmed.front() == '\'' && trimmed.back() == '\'')))
    {
        return trimmed.substr(1, trimmed.size() - 2);
    }
    return trimmed;
}

void AddSnapshotWarning(
    ConfigSnapshot& snapshot,
    const char* code,
    const std::string& detail)
{
    snapshot.warnings.push_back({code, std::string(), detail});
}
}

AdapterRuntimeConfig LoadAdapterConfig(
    const std::string& iniPath,
    const std::string& gameDirectory)
{
    AdapterRuntimeConfig config;
    if (!IsReadableFile(iniPath))
    {
        AddWarning(
            config,
            "config-open-failed",
            std::string(),
            "unable to open performance adapter INI");
        return config;
    }

    const SectionData diagnostics = ReadSection(
        iniPath,
        "PerformanceDiagnostics");
    if (!diagnostics.present)
    {
        AddWarning(
            config,
            "missing-diagnostics-section",
            std::string(),
            "section 'PerformanceDiagnostics' is missing");
    }
    if (diagnostics.truncated)
    {
        AddWarning(
            config,
            "section-truncated",
            std::string(),
            "PerformanceDiagnostics exceeds the 64 KiB section limit");
    }

    bool valueValid = true;
    config.enabled = ParseBoolean(
        diagnostics,
        "Enable",
        false,
        valueValid);
    if (!valueValid)
    {
        AddWarning(
            config,
            "invalid-boolean",
            std::string(),
            "PerformanceDiagnostics.Enable is invalid");
    }

    config.includeProcessMemory = ParseBoolean(
        diagnostics,
        "IncludeProcessMemory",
        true,
        valueValid);
    if (!valueValid)
    {
        config.includeProcessMemory = false;
        AddWarning(
            config,
            "invalid-boolean",
            std::string(),
            "PerformanceDiagnostics.IncludeProcessMemory is invalid");
    }

    config.includeConfigSnapshots = ParseBoolean(
        diagnostics,
        "IncludeConfigSnapshots",
        true,
        valueValid);
    if (!valueValid)
    {
        config.includeConfigSnapshots = false;
        AddWarning(
            config,
            "invalid-boolean",
            std::string(),
            "PerformanceDiagnostics.IncludeConfigSnapshots is invalid");
    }

    config.enableProviders = ParseBoolean(
        diagnostics,
        "EnableProviders",
        true,
        valueValid);
    if (!valueValid)
    {
        config.enableProviders = false;
        AddWarning(
            config,
            "invalid-boolean",
            std::string(),
            "PerformanceDiagnostics.EnableProviders is invalid");
    }

    if (!ParseUnsigned32(
        diagnostics,
        "ProviderSlowWarningUs",
        config.providerSlowWarningUs))
    {
        AddWarning(
            config,
            "invalid-number",
            std::string(),
            "PerformanceDiagnostics.ProviderSlowWarningUs is invalid");
    }

    const SectionData registrations = ReadSection(
        iniPath,
        "PerformanceRegister");
    if (!registrations.present)
    {
        AddWarning(
            config,
            "missing-register-section",
            std::string(),
            "section 'PerformanceRegister' is missing");
    }
    if (registrations.truncated)
    {
        AddWarning(
            config,
            "section-truncated",
            std::string(),
            "PerformanceRegister exceeds the 64 KiB section limit");
    }

    std::map<unsigned, std::string> registeredAdapters;
    for (const SectionEntry& entry : registrations.entries)
    {
        unsigned registrationOrder = 0;
        const RegistrationKeyResult keyResult = ParseRegistrationNumber(
            entry.key,
            registrationOrder);
        if (keyResult == RegistrationKeyResult::Invalid)
        {
            AddWarning(
                config,
                "invalid-registration-key",
                std::string(),
                "registration key '" + entry.key + "' is not numeric");
            continue;
        }
        if (keyResult == RegistrationKeyResult::OutOfRange)
        {
            AddWarning(
                config,
                "registration-out-of-range",
                std::string(),
                "registration key '" + entry.key +
                    "' is outside 1..128");
            continue;
        }
        if (registeredAdapters.find(registrationOrder) ==
            registeredAdapters.end())
        {
            registeredAdapters.emplace(
                registrationOrder,
                TrimAscii(entry.value));
        }
        else
        {
            AddWarning(
                config,
                "duplicate-registration-key",
                std::string(),
                "registration key '" + entry.key +
                    "' is duplicated; first value is kept");
        }
    }

    std::vector<AdapterDefinition> definitions;
    for (const auto& registration : registeredAdapters)
    {
        const unsigned registrationOrder = registration.first;
        const std::string& adapterName = registration.second;
        if (adapterName.empty())
        {
            AddWarning(
                config,
                "empty-registration",
                std::string(),
                "registration key " + std::to_string(registrationOrder) +
                    " has an empty adapter name");
            continue;
        }

        const std::string sectionName = "Performance." + adapterName;
        const SectionData adapterSection = ReadSection(
            iniPath,
            sectionName);
        if (!adapterSection.present)
        {
            AddWarning(
                config,
                "missing-section",
                adapterName,
                "section '" + sectionName + "' is missing");
            continue;
        }
        if (adapterSection.truncated)
        {
            AddWarning(
                config,
                "section-truncated",
                adapterName,
                "adapter section exceeds the 64 KiB section limit");
            continue;
        }

        const SectionEntry* typeEntry = FindEntry(adapterSection, "Type");
        bool typeValid = false;
        const AdapterType type = ParseAdapterType(
            typeEntry == nullptr ? std::string() : typeEntry->value,
            typeValid);
        if (!typeValid)
        {
            AddWarning(
                config,
                "unknown-type",
                adapterName,
                "missing or unsupported adapter Type");
            continue;
        }

        AdapterDefinition definition;
        definition.name = adapterName;
        definition.registrationOrder = registrationOrder;
        definition.type = type;
        definition.enabled = ParseBoolean(
            adapterSection,
            "Enable",
            true,
            valueValid);
        if (!valueValid)
        {
            AddWarning(
                config,
                "invalid-boolean",
                adapterName,
                "Enable is invalid");
            definition.enabled = false;
        }
        ParseModuleList(
            adapterSection,
            "Modules",
            adapterName,
            config,
            definition.modules);
        ParseModuleList(
            adapterSection,
            "SharedModules",
            adapterName,
            config,
            definition.sharedModules);
        const SectionEntry* provider = FindEntry(
            adapterSection,
            "ProviderExport");
        if (provider != nullptr)
        {
            definition.providerExport = TrimAscii(provider->value);
        }
        ParseConfigPaths(adapterSection, gameDirectory, definition);
        definitions.push_back(std::move(definition));
    }

    if (!IsReadableFile(iniPath))
    {
        config.enabled = false;
        AddWarning(
            config,
            "config-open-failed",
            std::string(),
            "performance adapter INI became unreadable");
    }

    config.registry = BuildRegistry(definitions);
    config.warnings.insert(
        config.warnings.end(),
        config.registry.warnings.begin(),
        config.registry.warnings.end());
    return config;
}

ConfigSnapshot ReadConfigSnapshot(
    const std::string& path,
    const ConfigSnapshotLimits& limits)
{
    constexpr std::size_t MaximumSnapshotBytes = 1024u * 1024u;
    constexpr std::size_t MaximumSnapshotEntries = 512u;

    ConfigSnapshot snapshot;
    const std::size_t byteLimit =
        (std::min)(limits.maxBytes, MaximumSnapshotBytes);
    const std::size_t entryLimit =
        (std::min)(limits.maxEntries, MaximumSnapshotEntries);

    std::ifstream input(path, std::ios::binary);
    if (!input.good())
    {
        const DWORD attributes = GetFileAttributesA(path.c_str());
        AddSnapshotWarning(
            snapshot,
            attributes == INVALID_FILE_ATTRIBUTES
                ? "missing-file"
                : "read-failed",
            "unable to read config snapshot '" + path + "'");
        return snapshot;
    }

    const std::size_t readCapacity = byteLimit ==
        std::numeric_limits<std::size_t>::max()
        ? byteLimit
        : byteLimit + 1u;
    if (readCapacity >
        static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()))
    {
        AddSnapshotWarning(
            snapshot,
            "read-failed",
            "config snapshot size is not representable");
        return snapshot;
    }

    std::string contents(readCapacity, '\0');
    input.read(contents.data(), static_cast<std::streamsize>(readCapacity));
    const std::streamsize count = input.gcount();
    if (input.bad())
    {
        AddSnapshotWarning(
            snapshot,
            "read-failed",
            "I/O error while reading config snapshot '" + path + "'");
        return snapshot;
    }

    std::size_t contentBytes = count < 0
        ? 0u
        : static_cast<std::size_t>(count);
    if (contentBytes > byteLimit)
    {
        snapshot.truncated = true;
        contentBytes = byteLimit;
        AddSnapshotWarning(
            snapshot,
            "snapshot-truncated",
            "config snapshot exceeds the byte limit");
    }
    snapshot.bytesRead = contentBytes;

    std::string currentSection;
    std::size_t position = 0;
    std::size_t lineNumber = 1;
    while (position < contentBytes)
    {
        const std::size_t newline = contents.find('\n', position);
        const bool hasCompleteLine = newline != std::string::npos &&
            newline < contentBytes;
        const std::size_t lineEnd = hasCompleteLine
            ? newline
            : contentBytes;
        if (!hasCompleteLine && snapshot.truncated)
        {
            break;
        }

        std::string line = contents.substr(position, lineEnd - position);
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        line = TrimAscii(StripSnapshotComment(line));

        if (!line.empty() && line.front() == '[' && line.back() == ']')
        {
            currentSection = TrimAscii(line.substr(1, line.size() - 2));
        }
        else if (!line.empty())
        {
            const std::size_t equals = line.find('=');
            if (equals != std::string::npos)
            {
                const std::string key = TrimAscii(line.substr(0, equals));
                if (!key.empty())
                {
                    if (snapshot.entries.size() >= entryLimit)
                    {
                        if (!snapshot.truncated)
                        {
                            snapshot.truncated = true;
                            AddSnapshotWarning(
                                snapshot,
                                "snapshot-truncated",
                                "config snapshot exceeds the active-key limit");
                        }
                        break;
                    }

                    snapshot.entries.push_back({
                        currentSection,
                        key,
                        UnquoteSnapshotValue(line.substr(equals + 1)),
                        path + ":" + std::to_string(lineNumber)});
                }
            }
        }

        position = hasCompleteLine ? newline + 1u : contentBytes;
        ++lineNumber;
    }

    return snapshot;
}
}
