#pragma once
#include <windows.h>

// Apply only after device/worker warmup and restore before fixture teardown.
// Timing controls share one allowed logical CPU; runtime checks use normal affinity.
class BenchmarkThreadAffinity {
public:
    BenchmarkThreadAffinity() {
        DWORD_PTR process = 0, system = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system) && process) {
            const DWORD_PTR selected = process & (~process + 1);
            previous_ = SetThreadAffinityMask(GetCurrentThread(), selected);
        }
    }
    ~BenchmarkThreadAffinity() {
        if (previous_)
            SetThreadAffinityMask(GetCurrentThread(), previous_);
    }
    bool valid() const { return previous_ != 0; }
private:
    DWORD_PTR previous_ = 0;
};
