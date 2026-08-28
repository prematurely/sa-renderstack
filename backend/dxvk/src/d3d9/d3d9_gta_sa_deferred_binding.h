#pragma once

#include <cstdint>

namespace dxvk {

  enum class D3D9GtaSaDeferredBindingDecision : uint8_t {
    None,
    Bind,
    Coalesced,
  };

  template <typename Token>
  class D3D9GtaSaDeferredBindingTracker {

  public:

    void markDirty() {
      m_dirty = true;
    }

    void invalidate() {
      m_valid = false;
      m_dirty = true;
    }

    D3D9GtaSaDeferredBindingDecision resolve(const Token& desired) {
      if (!m_dirty)
        return D3D9GtaSaDeferredBindingDecision::None;

      m_dirty = false;
      if (m_valid && m_bound == desired)
        return D3D9GtaSaDeferredBindingDecision::Coalesced;

      m_bound = desired;
      m_valid = true;
      return D3D9GtaSaDeferredBindingDecision::Bind;
    }

  private:

    Token m_bound = { };
    bool  m_valid = false;
    bool  m_dirty = false;
  };

}
