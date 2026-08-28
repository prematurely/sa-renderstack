#pragma once

namespace ProperShadersBindingLookup
{
template <typename Binding>
class LastHitCache
{
public:
    Binding* Find(void* effect) const noexcept
    {
        return effect && m_binding && m_binding->effect == effect
            ? m_binding
            : nullptr;
    }

    void Remember(Binding* binding) noexcept
    {
        m_binding = binding;
    }

    void Forget(Binding* binding) noexcept
    {
        if (m_binding == binding) m_binding = nullptr;
    }

private:
    Binding* m_binding = nullptr;
};
}
