#pragma once

#include "../util/util_bit.h"

namespace dxvk {

  // Return true only when a non-empty batch has been consumed by bindOne.
  template<typename BindOne>
  bool TryD3D9SingleBinding(uint32_t mask, const BindOne& bindOne) {
    if (!mask || (mask & (mask - 1u)))
      return false;

    bindOne(bit::tzcnt(mask));
    return true;
  }

}
