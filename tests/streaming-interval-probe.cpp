// Offline consumer / decision-fragment probe. Never loads an ASI or the game.
#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include "vendor/loadwholemap-ini/IniReader.h"

static_assert(sizeof(void*) == 4, "The captured decision fragment is x86");

class IntervalGate {
  uint32_t now_ = 0, last_ = 0, interval_ = 0;
  void* code_ = nullptr;
public:
  IntervalGate() {
    // ImprovedStreaming.SA.asi SHA256 D7BD005EDE20F00B358B75C4D4844618024F2A74AAF60957553EBD964BEE527C,
    // RVA 0x7F4B..0x7F63. Only relocate the three data operands and the final
    // jbe destination (to our false return); replace callees with bool returns.
    // The original fragment and relocation offsets are recorded in README.md.
    unsigned char bytes[] = {
      0xA1,0x84,0xCB,0xB7,0x00,
      0x2B,0x05,0xD0,0x08,0x05,0x10,
      0x3B,0x05,0xE4,0x08,0x05,0x10,
      0x77,0x06,0x66,0x0F,0x2F,0xC8,0x76,0x32,
      0xB8,0x01,0x00,0x00,0x00,0xC3,0x31,0xC0,0xC3
    };
    const uint32_t addresses[] = {uint32_t(uintptr_t(&now_)),
      uint32_t(uintptr_t(&last_)), uint32_t(uintptr_t(&interval_))};
    const size_t offsets[] = {1, 7, 13};
    for (size_t i = 0; i < 3; ++i) std::memcpy(bytes + offsets[i], &addresses[i], 4);
    bytes[24] = 6;
    code_ = VirtualAlloc(nullptr, sizeof(bytes), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!code_) throw std::runtime_error("VirtualAlloc failed");
    std::memcpy(code_, bytes, sizeof(bytes));
    DWORD previous = 0;
    if (!VirtualProtect(code_, sizeof(bytes), PAGE_EXECUTE_READ, &previous) ||
        !FlushInstructionCache(GetCurrentProcess(), code_, sizeof(bytes))) {
      VirtualFree(code_, 0, MEM_RELEASE);
      throw std::runtime_error("Cannot prepare decision fragment");
    }
  }
  IntervalGate(const IntervalGate&) = delete;
  IntervalGate& operator=(const IntervalGate&) = delete;
  ~IntervalGate() { VirtualFree(code_, 0, MEM_RELEASE); }
  bool permits(uint32_t now, uint32_t last, uint32_t interval, double ram, double cap) {
    now_ = now; last_ = last; interval_ = interval;
    int result;
    // Inputs match the installed callback at RVA 0x7F4B: xmm1=RAM, xmm0=cap.
    asm volatile("movsd %[cap], %%xmm0\n\t"
                 "movsd %[ram], %%xmm1\n\t"
                 "call *%[entry]"
                 : "=a"(result)
                 : [entry]"r"(code_), [cap]"m"(cap), [ram]"m"(ram)
                 : "ecx", "edx", "xmm0", "xmm1", "cc", "memory");
    return result != 0;
  }
};

static int replay(IntervalGate& gate, int interval, double ram) {
  uint32_t last = 0;
  int attempts = 0;
  // A controlled 10-second, 16-ms callback trace, with pressure already true.
  // Counts gate permits, not model evictions, I/O, conversion work, or FPS.
  for (uint32_t now = 16; now <= 10000; now += 16) {
    if (gate.permits(now, last, uint32_t(interval), ram, 3500.0)) {
      ++attempts;
      last = now;
    }
  }
  return attempts;
}

static void check(bool passed, const char* message) {
  if (!passed) throw std::runtime_error(message);
}

int main(int argc, char** argv) {
  try {
    IntervalGate gate;
    if (argc == 2 && std::string(argv[1]) == "--self-test") {
      check(!gate.permits(60, 0, 60, 1000, 3500), "60 equality");
      check(gate.permits(61, 0, 60, 1000, 3500), "60 plus one");
      check(!gate.permits(750, 0, 750, 1000, 3500), "750 equality");
      check(gate.permits(751, 0, 750, 1000, 3500), "750 plus one");
      check(!gate.permits(0, 0, 0, 1000, 3500), "zero same tick");
      check(gate.permits(1, 0, 0, 1000, 3500), "zero next tick");
      check(!gate.permits(1, 0, 750, 3500, 3500), "RAM equality");
      check(gate.permits(1, 0, 750, 3501, 3500), "RAM bypass");
      check(!gate.permits(0x10, 0xfffffff0u, 60, 1000, 3500), "wrap before interval");
      check(gate.permits(0x50, 0xfffffff0u, 60, 1000, 3500), "wrap after interval");
      check(replay(gate, 60, 1000) == 156, "baseline replay");
      check(replay(gate, 750, 1000) == 13, "candidate replay");
      check(replay(gate, 60, 3501) == 625, "baseline RAM replay");
      check(replay(gate, 750, 3501) == 625, "candidate RAM replay");
      std::cout << "PASS 14 native decision-fragment checks\n";
      return 0;
    }
    if (argc != 2) throw std::runtime_error("Usage: probe <absolute-ini-path>|--self-test");
    CIniReader reader(argv[1]);
    const int interval = reader.ReadInteger("Settings", "RemoveUnusedInterval", 60);
    // Also read unrelated budgets and flags through the real consumer.
    std::cout << "{\"intervalMs\":" << interval
      << ",\"streamMemory\":" << reader.ReadInteger("Settings", "StreamMemoryForced", -1)
      << ",\"doubleLimit\":" << reader.ReadInteger("Settings", "DoubleStreamingMemoryLimit", -1)
      << ",\"maxRam\":" << reader.ReadInteger("Settings", "MaxRAM", -1)
      << ",\"preloadLods\":" << reader.ReadInteger("Settings", "PreLoadLODs", -1)
      << ",\"pressurePermits\":" << replay(gate, interval, 1000)
      << ",\"ramPermits\":" << replay(gate, interval, 3501) << "}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
