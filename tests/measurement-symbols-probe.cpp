#if __has_include("MeasurementSymbols.h")
#include "MeasurementSymbols.h"
#include <cstdlib>
#include <iostream>

template<class Resolver>
auto LoadExportsOrAbsent(Resolver& resolver, uintptr_t base, const char* path, int)
  -> decltype(resolver.LoadExports(base, path), static_cast<const char*>(nullptr)) {
  return BridgeMeasurement::SymbolLoadStatusName(resolver.LoadExports(base, path));
}
template<class Resolver>
const char* LoadExportsOrAbsent(Resolver&, uintptr_t, const char*, long) { return "exports-unavailable"; }

template<class Result>
auto PrintEvidence(const Result& result, int) -> decltype((void)result.provenance, (void)result.aliases, void()) {
  std::cout << '\t' << SymbolProvenanceName(result.provenance) << '\t';
  for (size_t i = 0; i < result.aliases.size(); ++i) {
    if (i) std::cout << '|';
    std::cout << result.aliases[i];
  }
}
template<class Result>
void PrintEvidence(const Result&, long) { std::cout << "\tnone\t"; }
template<class Result>
auto PrintRawName(const Result& result, int) -> decltype((void)result.rawName, void()) { std::cout << '\t' << result.rawName; }
template<class Result>
void PrintRawName(const Result&, long) { std::cout << '\t'; }

int main(int argc, char** argv) {
  BridgeMeasurement::MeasurementSymbolResolver resolver;
  int i = 1;
  for (; i + 2 < argc && std::string(argv[i]) != "--"; i += 3) {
    const auto base = std::strtoull(argv[i + 2], nullptr, 0);
    const char* status = std::string(argv[i + 1]) == "@exports"
      ? LoadExportsOrAbsent(resolver, base, argv[i], 0)
      : BridgeMeasurement::SymbolLoadStatusName(resolver.LoadModule(base, argv[i], argv[i + 1]));
    std::cout << "load\t" << status << '\n';
  }
  for (++i; i < argc; ++i) {
    const auto result = resolver.Resolve(std::strtoull(argv[i], nullptr, 0));
    std::cout << "resolve\t" << BridgeMeasurement::SymbolQualityName(result.quality)
      << '\t' << result.moduleBase << '\t' << result.rva << '\t' << result.offset
      << '\t' << result.modulePath << '\t' << result.name;
    PrintEvidence(result, 0);
    PrintRawName(result, 0);
    std::cout << '\n';
  }
}
#else
#include <iostream>
int main() { std::cerr << "FAIL: exact-build measurement symbol resolver is absent\n"; return 1; }
#endif
