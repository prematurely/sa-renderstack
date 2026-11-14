#include <cstdint>
#include <list>
#include <unordered_map>

namespace dxvk {

  template<typename T>
  class lru_list {

  public:
    typedef typename std::list<T>::const_iterator const_iterator;

    void insert(T value, bool mostRecent = true) {
      auto cacheIter = m_cache.find(value);
      if (cacheIter != m_cache.end()) {
        if (mostRecent)
          m_list.splice(m_list.end(), m_list, cacheIter->second);
        return;
      }

      auto iter = m_list.insert(mostRecent ? m_list.cend() : m_list.cbegin(), value);
      try {
        m_cache.emplace(value, iter);
      } catch (...) {
        m_list.erase(iter);
        throw;
      }
    }

    void remove(const T& value) {
      auto cacheIter = m_cache.find(value);
      if (cacheIter == m_cache.end())
        return;

      m_list.erase(cacheIter->second);
      m_cache.erase(cacheIter);
    }

    const_iterator remove(const_iterator iter) {
      auto cacheIter = m_cache.find(*iter);
      m_cache.erase(cacheIter);
      return m_list.erase(iter);
    }

    void touch(const T& value) {
      auto cacheIter = m_cache.find(value);
      if (cacheIter == m_cache.end())
        return;

      m_list.splice(m_list.end(), m_list, cacheIter->second);
    }

    const_iterator leastRecentlyUsedIter() const {
      return m_list.cbegin();
    }

    const_iterator leastRecentlyUsedEndIter() const {
      return m_list.cend();
    }

    uint32_t size() const noexcept {
      return m_list.size();
    }

  private:
    std::list<T> m_list;
    std::unordered_map<T, const_iterator> m_cache;

  };

}
