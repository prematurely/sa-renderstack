#pragma once

#include "../util/util_bit.h"
#include <algorithm>

namespace dxvk {

  // Enumerate only first writes in an already validated register interval.
  // The callback may mark its range captured. Whole captured words are skipped.
  template<typename Mask, typename Capture>
  void ForEachD3D9UncapturedRange(Mask& captured, uint32_t start, uint32_t count,
                                const Capture& capture) {
    if (count == 1) {
      if (!captured.get(start))
        capture(start, 1u);
      return;
    }
    const uint32_t end = start + count;
    while (start < end) {
      const uint32_t word = start / 32;
      const uint32_t pending = ~captured.dword(word) & (~0u << (start % 32));
      if (!pending) {
        start = (word + 1) * 32;
        continue;
      }
      start = word * 32 + bit::tzcnt(pending);
      if (start >= end)
        break;
      uint32_t rangeEnd = start;
      while (rangeEnd < end) {
        const uint32_t offset = rangeEnd % 32;
        const uint32_t limit = std::min(32u - offset, end - rangeEnd);
        const uint32_t length = std::min(bit::tzcnt(captured.dword(rangeEnd / 32) >> offset), limit);
        rangeEnd += length;
        if (length < limit)
          break;
      }
      capture(start, rangeEnd - start);
      start = rangeEnd;
    }
  }

}
