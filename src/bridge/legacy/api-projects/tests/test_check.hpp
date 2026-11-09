#pragma once

#include <cstdio>
#include <atomic>

namespace api_test {
inline std::atomic<unsigned> checks = 0;
inline std::atomic<unsigned> failures = 0;

inline void check(bool value, const char* expression, const char* file, int line) {
  ++checks;
  if (!value) {
    ++failures;
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
  }
}

inline int finish() {
  std::printf("checks=%u failures=%u\n", checks.load(), failures.load());
  return failures.load() ? 1 : 0;
}
}

// Unlike assert(), these checks and their side effects execute in Release.
#define CHECK(...) ::api_test::check(bool(__VA_ARGS__), #__VA_ARGS__, __FILE__, __LINE__)
