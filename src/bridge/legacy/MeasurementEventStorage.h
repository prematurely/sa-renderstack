#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include "../../../backend/dxvk/src/d3d9/d3d9_bridge_telemetry.h"

namespace BridgeMeasurement::detail {

#if defined(RENDERSTACK_MEASUREMENT_STORAGE_TEST_SYNC)
struct MeasurementEventStorage;
void MeasurementStorageEmptyHeadSnapshot(MeasurementEventStorage&) noexcept;
#endif

// Capture-private SPSC staging. The device callback only copies into the ring;
// its managed worker owns all file creation, writes and cleanup.
struct MeasurementEventStorage {
  using Event = D3D9BridgeTelemetryEvent;
  static constexpr size_t Capacity = 32768;
  static constexpr size_t BlockEvents = 256;
  static constexpr UINT64 FileLimit = 128ull * 1024 * 1024;
  static_assert((Capacity + 2 * BlockEvents) * sizeof(Event) <= 2 * 1024 * 1024);
  enum Error : unsigned { Open = 1, Write = 2, Read = 4, Cap = 8, Queue = 16, Worker = 32, Cancelled = 64, Report = 128, Accounting = 256 };
  std::unique_ptr<Event[]> ring{new Event[Capacity]};
  std::atomic<UINT64> head{0}, tail{0}, received{0}, accepted{0}, written{0}, omitted{0}, reported{0};
  std::atomic<UINT64> finalizedReported{0};
  std::atomic<bool> reportFinished{false};
  std::atomic<unsigned> errors{0};
  std::atomic<bool> sealed{false}, ready{true}, consumed{false}, abort{false};
  // 0: no reader yet; 1: report owns reading; 2: shutdown won ownership.
  // Shutdown and the report must exchange ownership before touching the handle.
  std::atomic<unsigned> readerOwner{0};
  bool ClaimReader() noexcept {
    unsigned expected = 0;
    if (readerOwner.compare_exchange_strong(expected, 1)) return true;
    return expected == 1;
  }
  HANDLE file = INVALID_HANDLE_VALUE; // worker publishes before ready; reader finishes before consumed
  HANDLE stop = nullptr;

  void Push(const Event& event) noexcept {
    received.fetch_add(1, std::memory_order_relaxed);
    if (errors.load() & Worker) { omitted.fetch_add(1); return; }
    const auto h = head.load(std::memory_order_relaxed);
    if (h - tail.load(std::memory_order_acquire) == Capacity) {
      errors.fetch_or(Queue); omitted.fetch_add(1); return;
    }
    ring[h % Capacity] = event;
    head.store(h + 1, std::memory_order_release);
    accepted.fetch_add(1, std::memory_order_relaxed);
  }
  void CheckTerminalAccounting() noexcept {
    // A stopped producer has published all counters before seal/abort. Keep raw
    // omissions distinct from unread records: flag discrepancies, never count a
    // missing or duplicated record a second time to make the totals agree.
    const auto r = received.load(), a = accepted.load(), w = written.load();
    if (a > r || w > a || w > r || omitted.load() != r - w ||
        head.load(std::memory_order_acquire) != tail.load() || reported.load() > w)
      errors.fetch_or(Accounting);
  }
  void Run() noexcept {
    wchar_t directory[MAX_PATH]{}, path[MAX_PATH]{};
    const auto pathLength = GetTempPathW(MAX_PATH, directory);
    if (!pathLength || pathLength >= MAX_PATH || !GetTempFileNameW(directory, L"F8e", 0, path)) errors.fetch_or(Open);
    else {
      file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
      if (file == INVALID_HANDLE_VALUE) { DeleteFileW(path); errors.fetch_or(Open); }
    }
    std::array<Event, BlockEvents> block;
    for (;;) {
      const auto t = tail.load(std::memory_order_relaxed);
      const auto h = head.load(std::memory_order_acquire);
      const auto count = (std::min)(UINT64(BlockEvents), h - t);
      if (!count) {
#if defined(RENDERSTACK_MEASUREMENT_STORAGE_TEST_SYNC)
        MeasurementStorageEmptyHeadSnapshot(*this);
#endif
        if (sealed.load(std::memory_order_acquire) || abort.load(std::memory_order_acquire)) {
          // The producer may have published its final batch after the empty
          // snapshot above. Only a head observed AFTER stop can prove drained.
          if (head.load(std::memory_order_acquire) == t) break;
          continue;
        }
        Sleep(1); continue;
      }
      for (UINT64 i = 0; i < count; ++i) block[size_t(i)] = ring[size_t((t + i) % Capacity)];
      tail.store(t + count, std::memory_order_release);
      const DWORD bytes = DWORD(count * sizeof(Event));
      if (abort.load()) { errors.fetch_or(Cancelled); omitted.fetch_add(count); }
      else if (errors.load() & (Open | Write | Cap)) omitted.fetch_add(count);
      else if ((written.load() + count) * sizeof(Event) > FileLimit) { errors.fetch_or(Cap); omitted.fetch_add(count); }
      else {
        DWORD actual = 0;
        const bool ok = WriteFile(file, block.data(), bytes, &actual, nullptr) != FALSE;
        const auto complete = actual / sizeof(Event);
        written.fetch_add(complete);
        if (!ok || actual != bytes) { errors.fetch_or(Write); omitted.fetch_add(count - complete); }
      }
    }
    CheckTerminalAccounting();
    ready.store(true, std::memory_order_release);
    while (!consumed.load(std::memory_order_acquire) && !abort.load()) {
      if (stop && WaitForSingleObject(stop, 0) != WAIT_TIMEOUT) {
        unsigned expected = 0;
        if (readerOwner.compare_exchange_strong(expected, 2)) { errors.fetch_or(Cancelled); break; }
      }
      Sleep(1);
    }
    if (file != INVALID_HANDLE_VALUE) { CloseHandle(file); file = INVALID_HANDLE_VALUE; }
  }
  template<typename F> void ForEach(F callback) {
    const bool owned = ClaimReader();
    while (!ready.load(std::memory_order_acquire)) Sleep(1);
    if (!owned) { errors.fetch_or(Cancelled); return; }
    if (file == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER offset{};
    if (!SetFilePointerEx(file, offset, nullptr, FILE_BEGIN)) { errors.fetch_or(Read); return; }
    std::array<Event, BlockEvents> block;
    const auto total = written.load();
    for (UINT64 index = 0; index < total;) {
      const auto count = (std::min)(UINT64(BlockEvents), total - index);
      DWORD actual = 0;
      if (!ReadFile(file, block.data(), DWORD(count * sizeof(Event)), &actual, nullptr) || actual != count * sizeof(Event)) {
        errors.fetch_or(Read); break;
      }
      for (UINT64 i = 0; i < count; ++i) { callback(size_t(index + i), block[size_t(i)]); reported.fetch_add(1); }
      index += count;
    }
  }
};

} // namespace BridgeMeasurement::detail
