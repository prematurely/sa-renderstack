#include "test_check.hpp"
#include "../../BridgeLog.h"
#include <io.h>
#include <cstring>
#include <cstdlib>

void invalid_parameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) { }

int main() {
  _set_invalid_parameter_handler(invalid_parameter);
  FILE* output = tmpfile();
  CHECK(output != nullptr);
  if (!output) return api_test::finish();
  BridgeDiagnostics::Write(output, "device {}", 42);
  rewind(output);
  char line[160]{};
  CHECK(fgets(line,sizeof(line),output) != nullptr);
  CHECK(std::strstr(line, "device 42\n") != nullptr);
  fclose(output);

  output = tmpfile();
  CHECK(output != nullptr);
  if (!output) return api_test::finish();
  CHECK(setvbuf(output,nullptr,_IONBF,0) == 0);
  // Actual CRT I/O failure, not a throwing mock: close the underlying file
  // descriptor while retaining the FILE object that the logger writes through.
  CHECK(_close(_fileno(output)) == 0);
  bool threw = false;
  try { BridgeDiagnostics::Write(output, "release {}", 1); }
  catch (...) { threw = true; }
  CHECK(!threw);
  CHECK(ferror(output) != 0);
  fclose(output);
  return api_test::finish();
}
