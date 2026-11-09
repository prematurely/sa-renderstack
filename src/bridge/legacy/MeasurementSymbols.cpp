#include "MeasurementSymbols.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#ifdef _MSC_VER
#pragma comment(lib, "bcrypt.lib")
#endif

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <utility>
#include <vector>

namespace BridgeMeasurement {
namespace {
constexpr size_t MaxBinaryBytes = 512u * 1024u * 1024u;
constexpr size_t MaxSidecarBytes = 64u * 1024u * 1024u;
constexpr size_t MaxLineBytes = 16384;
constexpr size_t MaxSymbols = 250000;
constexpr size_t MaxNameBytes = 4096;

struct Section { uint32_t begin = 0, end = 0; };
struct FileSection { Section range; uint32_t rawOffset = 0, rawSize = 0; };
struct ExportLayout {
  uint32_t directoryRva = 0, directorySize = 0, headersSize = 0;
  std::vector<FileSection> sections;
};
struct Symbol {
  uint32_t begin = 0, end = 0, prefixEnd = 0;
  SymbolQuality quality = SymbolQuality::Missing;
  std::string name;
  std::string rawName;
  std::vector<std::string> aliases;
};

std::string FullPathUtf8(std::string_view path) {
  const auto encoded = std::filesystem::absolute(std::filesystem::u8path(path)).lexically_normal().u8string();
  // u8string stores UTF-8 bytes as char in C++17 and char8_t in C++20+.
  return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

bool ReadFile(const std::filesystem::path& path, size_t limit, std::vector<unsigned char>& data) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) return false;
  const auto length = input.tellg();
  if (length < 0 || static_cast<uint64_t>(length) > limit) return false;
  data.resize(static_cast<size_t>(length));
  input.seekg(0);
  return data.empty() || static_cast<bool>(input.read(reinterpret_cast<char*>(data.data()), data.size()));
}

bool U16(const std::vector<unsigned char>& data, size_t at, uint16_t& value) {
  if (at > data.size() || data.size() - at < 2) return false;
  value = static_cast<uint16_t>(data[at] | data[at + 1] << 8);
  return true;
}

bool U32(const std::vector<unsigned char>& data, size_t at, uint32_t& value) {
  if (at > data.size() || data.size() - at < 4) return false;
  value = uint32_t(data[at]) | uint32_t(data[at + 1]) << 8 | uint32_t(data[at + 2]) << 16 | uint32_t(data[at + 3]) << 24;
  return true;
}

bool ReadPe(const std::vector<unsigned char>& data, uint32_t& imageSize, std::vector<Section>& sections,
            ExportLayout* exportLayout = nullptr) {
  uint16_t dos = 0, count = 0, optionalSize = 0, magic = 0;
  uint32_t pe = 0, signature = 0;
  if (!U16(data, 0, dos) || dos != 0x5a4d || !U32(data, 0x3c, pe) ||
      pe > data.size() || data.size() - pe < 24 || !U32(data, pe, signature) || signature != 0x4550 ||
      !U16(data, pe + 6, count) || !count || count > 96 || !U16(data, pe + 20, optionalSize) ||
      !U16(data, pe + 24, magic) || (magic != 0x10b && magic != 0x20b) ||
      optionalSize < (magic == 0x10b ? 96 : 112) || !U32(data, pe + 80, imageSize) || !imageSize) return false;
  const size_t table = static_cast<size_t>(pe) + 24 + optionalSize;
  if (table > data.size() || data.size() - table < size_t(count) * 40) return false;
  if (exportLayout) {
    uint32_t directoryCount = 0;
    const size_t directoryOffset = magic == 0x10b ? 96 : 112;
    if (!U32(data, static_cast<size_t>(pe) + 24 + directoryOffset - 4, directoryCount) ||
        !U32(data, static_cast<size_t>(pe) + 24 + 60, exportLayout->headersSize)) return false;
    if (exportLayout->headersSize > imageSize) return false;
    if (directoryCount) {
      if (optionalSize < directoryOffset + 8 ||
          !U32(data, static_cast<size_t>(pe) + 24 + directoryOffset, exportLayout->directoryRva) ||
          !U32(data, static_cast<size_t>(pe) + 28 + directoryOffset, exportLayout->directorySize)) return false;
      if (exportLayout->directoryRva || exportLayout->directorySize) {
        if (!exportLayout->directoryRva || exportLayout->directorySize < 40 ||
            exportLayout->directoryRva >= imageSize || exportLayout->directorySize > imageSize - exportLayout->directoryRva) return false;
      }
    }
  }
  for (size_t i = 0; i < count; ++i) {
    uint32_t size = 0, start = 0, raw = 0, rawOffset = 0, flags = 0;
    const size_t at = table + i * 40;
    if (!U32(data, at + 8, size) || !U32(data, at + 12, start) ||
        !U32(data, at + 16, raw) || !U32(data, at + 20, rawOffset) || !U32(data, at + 36, flags)) return false;
    if (!size) size = raw;
    if (start >= imageSize || size > imageSize - start) return false;
    if ((flags & 0x20000000) && size) sections.push_back({start, start + size});
    if (exportLayout && size) exportLayout->sections.push_back({{start, start + size}, rawOffset, raw});
  }
  std::sort(sections.begin(), sections.end(), [](const Section& a, const Section& b) { return a.begin < b.begin; });
  for (size_t i = 1; i < sections.size(); ++i) if (sections[i].begin < sections[i - 1].end) return false;
  return true;
}

// A metadata span must be backed by one unambiguous section (or PE headers).
bool MapRva(const std::vector<unsigned char>& data, const ExportLayout& layout,
            uint32_t rva, size_t count, size_t& offset, size_t* available = nullptr) {
  size_t remaining = 0;
  bool mapped = false;
  if (rva < layout.headersSize && rva < data.size()) {
    offset = rva;
    remaining = (std::min)(size_t(layout.headersSize - rva), data.size() - rva);
    mapped = true;
  }
  for (const auto& section : layout.sections) {
    if (rva < section.range.begin || rva >= section.range.end) continue;
    if (mapped) return false;
    const uint32_t delta = rva - section.range.begin;
    if (delta >= section.rawSize || section.rawOffset > data.size() || delta > data.size() - section.rawOffset) return false;
    offset = size_t(section.rawOffset) + delta;
    remaining = (std::min)(size_t(section.range.end - rva), size_t(section.rawSize - delta));
    remaining = (std::min)(remaining, data.size() - offset);
    mapped = true;
  }
  if (!mapped || count > remaining) return false;
  if (available) *available = remaining;
  return true;
}

bool ReadExportString(const std::vector<unsigned char>& data, const ExportLayout& layout,
                      uint32_t rva, size_t limit, std::string& text) {
  size_t offset = 0, available = 0;
  if (!MapRva(data, layout, rva, 1, offset, &available)) return false;
  const size_t length = (std::min)(available, limit + 1);
  const auto begin = data.begin() + offset;
  const auto end = std::find(begin, begin + length, 0);
  if (end == begin || end == begin + length) return false;
  text.assign(begin, end);
  if (text.find_first_of("\t\r\n") != std::string::npos) return false;
  return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0) > 0;
}

bool ParseExports(const std::vector<unsigned char>& data, const ExportLayout& layout,
                  const std::vector<Section>& executableSections, uint32_t imageSize,
                  std::vector<Symbol>& symbols) {
  if (!layout.directoryRva) return true;
  size_t directory = 0;
  if (!MapRva(data, layout, layout.directoryRva, 40, directory)) return false;
  uint32_t functionCount = 0, nameCount = 0, functionsRva = 0, namesRva = 0, ordinalsRva = 0;
  if (!U32(data, directory + 20, functionCount) || !U32(data, directory + 24, nameCount) ||
      !U32(data, directory + 28, functionsRva) || !U32(data, directory + 32, namesRva) ||
      !U32(data, directory + 36, ordinalsRva) || functionCount > MaxSymbols || nameCount > MaxSymbols ||
      (!functionCount && nameCount)) return false;
  size_t functions = 0, names = 0, ordinals = 0;
  if ((functionCount && !MapRva(data, layout, functionsRva, size_t(functionCount) * 4, functions)) ||
      (nameCount && (!MapRva(data, layout, namesRva, size_t(nameCount) * 4, names) ||
                     !MapRva(data, layout, ordinalsRva, size_t(nameCount) * 2, ordinals)))) return false;
  struct Entry { uint32_t rva = 0; bool forwarded = false; };
  std::vector<Entry> entries(functionCount);
  std::vector<uint32_t> starts;
  const uint32_t directoryEnd = layout.directoryRva + layout.directorySize;
  auto executable = [&](uint32_t rva) {
    return std::find_if(executableSections.begin(), executableSections.end(), [&](const Section& section) {
      return rva >= section.begin && rva < section.end;
    });
  };
  for (size_t i = 0; i < functionCount; ++i) {
    if (!U32(data, functions + i * 4, entries[i].rva) || entries[i].rva >= imageSize) return false;
    const uint32_t rva = entries[i].rva;
    if (!rva) continue; // Empty ordinal slots are not executable entries.
    entries[i].forwarded = rva >= layout.directoryRva && rva < directoryEnd;
    if (entries[i].forwarded) {
      std::string target;
      if (!ReadExportString(data, layout, rva, (std::min)(MaxNameBytes, size_t(directoryEnd - rva - 1)), target)) return false;
    }
    if (executable(rva) != executableSections.end()) starts.push_back(rva);
  }
  std::sort(starts.begin(), starts.end());
  starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
  std::map<uint32_t, std::vector<std::string>> groups;
  for (size_t i = 0; i < nameCount; ++i) {
    uint32_t nameRva = 0;
    uint16_t ordinal = 0;
    std::string name;
    if (!U32(data, names + i * 4, nameRva) || !U16(data, ordinals + i * 2, ordinal) || ordinal >= functionCount ||
        !entries[ordinal].rva || !ReadExportString(data, layout, nameRva, MaxNameBytes, name)) return false;
    const auto& entry = entries[ordinal];
    if (!entry.forwarded && executable(entry.rva) != executableSections.end()) groups[entry.rva].push_back(std::move(name));
  }
  uint32_t prefixEnd = 0;
  for (auto& group : groups) {
    Symbol symbol;
    symbol.begin = group.first;
    const auto section = executable(symbol.begin);
    const auto next = std::upper_bound(starts.begin(), starts.end(), symbol.begin);
    symbol.end = next == starts.end() ? section->end : (std::min)(section->end, *next);
    // An executable-marked export directory still contains metadata/forwarders.
    if (symbol.begin < layout.directoryRva && layout.directoryRva < symbol.end) symbol.end = layout.directoryRva;
    auto& aliases = group.second;
    std::sort(aliases.begin(), aliases.end());
    aliases.erase(std::unique(aliases.begin(), aliases.end()), aliases.end());
    if (aliases.size() == 1) { symbol.name = std::move(aliases.front()); symbol.rawName = symbol.name; }
    else symbol.aliases = std::move(aliases);
    symbol.quality = SymbolQuality::Inferred;
    prefixEnd = (std::max)(prefixEnd, symbol.end);
    symbol.prefixEnd = prefixEnd;
    symbols.push_back(std::move(symbol));
  }
  return true;
}

std::string Hex(const unsigned char* data, size_t count) {
  static const char digits[] = "0123456789abcdef";
  std::string text;
  text.reserve(count * 2);
  for (size_t i = 0; i < count; ++i) { text += digits[data[i] >> 4]; text += digits[data[i] & 15]; }
  return text;
}

bool Sha256(const std::vector<unsigned char>& data, std::string& digest) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  DWORD objectSize = 0, written = 0;
  bool okay = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
  if (okay) okay = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &written, 0) >= 0;
  std::vector<unsigned char> object(okay ? objectSize : 0);
  std::array<unsigned char, 32> result{};
  if (okay) okay = BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) >= 0;
  if (okay) okay = BCryptHashData(hash, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0) >= 0;
  if (okay) okay = BCryptFinishHash(hash, result.data(), static_cast<ULONG>(result.size()), 0) >= 0;
  if (hash) BCryptDestroyHash(hash);
  if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
  if (okay) digest = Hex(result.data(), result.size());
  return okay;
}

int Digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

bool ParseHex(std::string_view text, uint32_t& value) {
  if (text.size() != 8) return false;
  value = 0;
  for (char c : text) { const int digit = Digit(c); if (digit < 0) return false; value = (value << 4) | uint32_t(digit); }
  return true;
}

bool DigestValid(std::string_view digest) {
  return digest.size() == 64 && std::all_of(digest.begin(), digest.end(), [](char c) { return Digit(c) >= 0; });
}

bool NameFromHex(std::string_view text, std::string& name) {
  if (text.empty() || text.size() % 2 || text.size() > MaxNameBytes * 2) return false;
  name.reserve(text.size() / 2);
  for (size_t i = 0; i < text.size(); i += 2) {
    const int high = Digit(text[i]), low = Digit(text[i + 1]);
    if (high < 0 || low < 0) return false;
    const char c = static_cast<char>((high << 4) | low);
    if (c == '\0' || c == '\t' || c == '\r' || c == '\n') return false;
    name += c;
  }
  // Reject malformed UTF-8 before names can reach reports.
  return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), nullptr, 0) > 0;
}

SymbolLoadStatus ParseSidecar(const std::vector<unsigned char>& data, uint32_t imageSize,
                              const std::vector<Section>& peSections, const std::string& digest,
                              std::vector<Symbol>& symbols, SymbolProvenance& provenance) {
  const std::string text(data.begin(), data.end());
  std::istringstream input(text);
  std::string line;
  auto next = [&]() {
    if (!std::getline(input, line) || line.size() > MaxLineBytes || line.find('\0') != std::string::npos) return false;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return true;
  };
  if (!next() || line != "RSYM1" || !next() || line.rfind("sha256 ", 0) != 0 || !DigestValid(std::string_view(line).substr(7))) return SymbolLoadStatus::InvalidSidecar;
  const bool identityMatches = line.substr(7) == digest;
  uint32_t declaredSize = 0;
  if (!next() || line.rfind("image_size ", 0) != 0 || !ParseHex(std::string_view(line).substr(11), declaredSize)) return SymbolLoadStatus::InvalidSidecar;
  if (declaredSize != imageSize) return SymbolLoadStatus::IdentityMismatch;
  if (!next() || (line != "source_kind coff" && line != "source_kind map")) return SymbolLoadStatus::InvalidSidecar;
  const bool mapSource = line == "source_kind map";
  if (!next() || line.rfind("source_sha256 ", 0) != 0 || !DigestValid(std::string_view(line).substr(14))) return SymbolLoadStatus::InvalidSidecar;
  const bool sourceMatches = mapSource || line.substr(14) == digest;
  std::vector<Section> sections;
  bool ended = false, sawSymbols = false;
  while (next()) {
    if (line == "end") { ended = true; break; }
    std::istringstream row(line);
    std::string kind, beginText, endText, quality, name, rawName, extra;
    uint32_t begin = 0, end = 0;
    if (!(row >> kind >> beginText >> endText) || !ParseHex(beginText, begin) || !ParseHex(endText, end) || begin >= end) return SymbolLoadStatus::InvalidSidecar;
    if (kind == "section" && !sawSymbols) {
      if (row >> extra || sections.size() >= 96) return SymbolLoadStatus::InvalidSidecar;
      sections.push_back({begin, end});
    } else if (kind == "symbol") {
      sawSymbols = true;
      Symbol symbol;
      if (!(row >> quality >> name) || (quality != "E" && quality != "I") ||
          (mapSource && quality == "E") || symbols.size() >= MaxSymbols || !NameFromHex(name, symbol.name)) return SymbolLoadStatus::InvalidSidecar;
      if (row >> rawName) {
        if ((row >> extra) || !NameFromHex(rawName, symbol.rawName)) return SymbolLoadStatus::InvalidSidecar;
      } else symbol.rawName = symbol.name; // Original RSYM1 records had one name.
      if (!std::any_of(peSections.begin(), peSections.end(), [&](const Section& section) { return begin >= section.begin && end <= section.end; })) return SymbolLoadStatus::InvalidSidecar;
      symbol.begin = begin; symbol.end = end;
      symbol.quality = quality == "E" ? SymbolQuality::Exact : SymbolQuality::Inferred;
      symbols.push_back(std::move(symbol));
    } else return SymbolLoadStatus::InvalidSidecar;
  }
  if (!ended || input.peek() != std::char_traits<char>::eof() || sections.size() != peSections.size()) return SymbolLoadStatus::InvalidSidecar;
  for (size_t i = 0; i < sections.size(); ++i) if (sections[i].begin != peSections[i].begin || sections[i].end != peSections[i].end) return SymbolLoadStatus::InvalidSidecar;
  if (!identityMatches || !sourceMatches) return SymbolLoadStatus::IdentityMismatch;
  provenance = mapSource ? SymbolProvenance::LinkerMap : SymbolProvenance::Coff;
  std::sort(symbols.begin(), symbols.end(), [](const Symbol& a, const Symbol& b) { return a.begin < b.begin; });
  uint32_t prefixEnd = 0;
  for (auto& symbol : symbols) { prefixEnd = (std::max)(prefixEnd, symbol.end); symbol.prefixEnd = prefixEnd; }
  return symbols.empty() ? SymbolLoadStatus::Missing : SymbolLoadStatus::Ready;
}
} // namespace

struct MeasurementSymbolResolver::Impl {
  struct Module {
    uintptr_t base = 0;
    uint32_t imageSize = 0;
    std::string path, digest;
    SymbolQuality unavailable = SymbolQuality::Missing;
    SymbolProvenance provenance = SymbolProvenance::None;
    std::vector<Symbol> symbols;
  };
  std::vector<Module> modules;
};

MeasurementSymbolResolver::MeasurementSymbolResolver() : m_impl(std::make_unique<Impl>()) {}
MeasurementSymbolResolver::~MeasurementSymbolResolver() = default;

SymbolLoadStatus MeasurementSymbolResolver::LoadModule(uintptr_t base, std::string_view binaryPath, std::string_view sidecarPath) {
  try {
    Impl::Module module;
    module.base = base;
    module.path = FullPathUtf8(binaryPath);
    std::vector<unsigned char> binary;
    std::vector<Section> sections;
    if (!ReadFile(std::filesystem::u8path(module.path), MaxBinaryBytes, binary) || !ReadPe(binary, module.imageSize, sections) ||
        module.imageSize > (std::numeric_limits<uintptr_t>::max)() - base || !Sha256(binary, module.digest)) return SymbolLoadStatus::BinaryUnavailable;
    for (const auto& prior : m_impl->modules) {
      if (base < prior.base + prior.imageSize && prior.base < base + module.imageSize) return SymbolLoadStatus::ModuleConflict;
    }
    std::vector<unsigned char> sidecar;
    SymbolLoadStatus status = SymbolLoadStatus::Missing;
    if (ReadFile(std::filesystem::u8path(sidecarPath), MaxSidecarBytes, sidecar)) {
      status = ParseSidecar(sidecar, module.imageSize, sections, module.digest, module.symbols, module.provenance);
    } else if (std::filesystem::exists(std::filesystem::u8path(sidecarPath))) status = SymbolLoadStatus::InvalidSidecar;
    if (status == SymbolLoadStatus::IdentityMismatch) module.unavailable = SymbolQuality::IdentityMismatch;
    else if (status == SymbolLoadStatus::InvalidSidecar) module.unavailable = SymbolQuality::InvalidSidecar;
    if (status != SymbolLoadStatus::Ready) module.symbols.clear();
    m_impl->modules.push_back(std::move(module));
    return status;
  } catch (const std::exception&) {
    return SymbolLoadStatus::BinaryUnavailable;
  }
}

SymbolLoadStatus MeasurementSymbolResolver::LoadExports(uintptr_t base, std::string_view binaryPath) {
  try {
    Impl::Module module;
    module.base = base;
    module.path = FullPathUtf8(binaryPath);
    std::vector<unsigned char> binary;
    std::vector<Section> sections;
    ExportLayout layout;
    if (!ReadFile(std::filesystem::u8path(module.path), MaxBinaryBytes, binary) || !ReadPe(binary, module.imageSize, sections, &layout) ||
        module.imageSize > (std::numeric_limits<uintptr_t>::max)() - base || !Sha256(binary, module.digest)) return SymbolLoadStatus::BinaryUnavailable;
    for (const auto& prior : m_impl->modules) {
      if (base < prior.base + prior.imageSize && prior.base < base + module.imageSize) return SymbolLoadStatus::ModuleConflict;
    }
    if (!ParseExports(binary, layout, sections, module.imageSize, module.symbols)) return SymbolLoadStatus::BinaryUnavailable;
    module.provenance = SymbolProvenance::Exports;
    const auto status = module.symbols.empty() ? SymbolLoadStatus::Missing : SymbolLoadStatus::Ready;
    m_impl->modules.push_back(std::move(module));
    return status;
  } catch (const std::exception&) {
    return SymbolLoadStatus::BinaryUnavailable;
  }
}

SymbolResult MeasurementSymbolResolver::Resolve(uintptr_t address) const {
  SymbolResult result;
  for (const auto& module : m_impl->modules) {
    if (address < module.base || address - module.base >= module.imageSize) continue;
    result.modulePath = module.path;
    result.binarySha256 = module.digest;
    result.moduleBase = module.base;
    result.rva = static_cast<uint32_t>(address - module.base);
    result.quality = module.unavailable;
    result.provenance = module.provenance;
    const auto& symbols = module.symbols;
    auto after = std::upper_bound(symbols.begin(), symbols.end(), result.rva, [](uint32_t rva, const Symbol& symbol) { return rva < symbol.begin; });
    const Symbol* found = nullptr;
    while (after != symbols.begin()) {
      --after;
      if (after->prefixEnd <= result.rva) break;
      if (after->begin <= result.rva && result.rva < after->end) {
        if (found) { result.quality = SymbolQuality::Ambiguous; return result; }
        found = &*after;
      }
    }
    if (found) {
      result.name = found->name;
      result.rawName = found->rawName;
      result.offset = result.rva - found->begin;
      result.aliases = found->aliases;
      result.quality = result.aliases.size() > 1 ? SymbolQuality::Ambiguous : found->quality;
    }
    return result;
  }
  return result;
}

const char* SymbolQualityName(SymbolQuality quality) noexcept {
  switch (quality) {
    case SymbolQuality::Exact: return "exact";
    case SymbolQuality::Inferred: return "inferred";
    case SymbolQuality::Ambiguous: return "ambiguous";
    case SymbolQuality::Missing: return "missing";
    case SymbolQuality::IdentityMismatch: return "identity-mismatch";
    case SymbolQuality::InvalidSidecar: return "invalid-sidecar";
    default: return "module-not-found";
  }
}

const char* SymbolLoadStatusName(SymbolLoadStatus status) noexcept {
  switch (status) {
    case SymbolLoadStatus::Ready: return "ready";
    case SymbolLoadStatus::Missing: return "missing";
    case SymbolLoadStatus::IdentityMismatch: return "identity-mismatch";
    case SymbolLoadStatus::InvalidSidecar: return "invalid-sidecar";
    case SymbolLoadStatus::ModuleConflict: return "module-conflict";
    default: return "binary-unavailable";
  }
}

const char* SymbolProvenanceName(SymbolProvenance provenance) noexcept {
  switch (provenance) {
    case SymbolProvenance::Coff: return "coff";
    case SymbolProvenance::LinkerMap: return "linker-map";
    case SymbolProvenance::Exports: return "exports";
    default: return "none";
  }
}
} // namespace BridgeMeasurement
