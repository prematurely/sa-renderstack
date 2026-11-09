#pragma once
#include <windows.h>
#include <cstdio>
#include <format>
#include <print>
#include <utility>

namespace BridgeDiagnostics {
template <class Operation>
void BestEffort(Operation&& operation) noexcept
{
    try { operation(); } catch (...) { }
}
template <class... Args>
void Write(FILE* output, std::format_string<Args...> fmt, Args&&... args) noexcept
{
    if (!output) return;
    BestEffort([&] {
        SYSTEMTIME time;
        GetLocalTime(&time);
        std::print(output, "[{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}] ",
            time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
        std::vprint_nonunicode(output, fmt.get(), std::make_format_args(args...));
        fputc('\n', output);
        fflush(output);
    });
}
}
