#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace BridgeMeasurement {

enum class SymbolQuality {
  Exact, Inferred, Ambiguous, Missing, IdentityMismatch, InvalidSidecar, ModuleNotFound
};

enum class SymbolLoadStatus {
  Ready, Missing, IdentityMismatch, InvalidSidecar, BinaryUnavailable, ModuleConflict
};

// Exports always have inferred range boundaries, including ambiguous aliases.
enum class SymbolProvenance { None, Coff, LinkerMap, Exports };

const char* SymbolQualityName(SymbolQuality quality) noexcept;
const char* SymbolLoadStatusName(SymbolLoadStatus status) noexcept;
const char* SymbolProvenanceName(SymbolProvenance provenance) noexcept;

struct SymbolResult {
  std::string modulePath;
  std::string binarySha256;
  uintptr_t moduleBase = 0;
  uint32_t rva = 0;
  std::string name;
  uint32_t offset = 0;
  SymbolQuality quality = SymbolQuality::ModuleNotFound;
  SymbolProvenance provenance = SymbolProvenance::None;
  // Export names sharing this RVA; populated instead of choosing a primary name.
  std::vector<std::string> aliases;
  // Original metadata spelling; name may be a build-time demangled display name.
  std::string rawName;
};

// Initialize on a background worker before publishing to sampling/report code.
// LoadModule reads/hashes disk files and allocates. A registered module is never
// replaced; missing/invalid sidecars retain its path/base/hash and explicit state.
// Once initialization ends, const Resolve calls are thread safe. Do not call
// LoadModule concurrently with Resolve. Resolve uses only prepared memory, but
// returns owning strings: call after resuming the sampled thread, outside the HUD
// per-frame path. Exact means a COFF explicit size, not an inferred next boundary.
class MeasurementSymbolResolver {
public:
  MeasurementSymbolResolver();
  ~MeasurementSymbolResolver();
  MeasurementSymbolResolver(const MeasurementSymbolResolver&) = delete;
  MeasurementSymbolResolver& operator=(const MeasurementSymbolResolver&) = delete;

  SymbolLoadStatus LoadModule(uintptr_t base, std::string_view binaryPath,
                              std::string_view sidecarPath);
  // Explicit opt-in for caller-selected system DLLs. Reads the actual PE export
  // table on the worker; skips forwarders/non-executable entries, and bounds each
  // named range by the next distinct export RVA or executable-section end.
  // Never replaces a registered module, even when its sidecar mismatched.
  SymbolLoadStatus LoadExports(uintptr_t base, std::string_view binaryPath);
  SymbolResult Resolve(uintptr_t address) const;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace BridgeMeasurement
