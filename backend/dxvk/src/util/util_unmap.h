#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "../dxvk/dxvk_allocator.h"
#include "../dxvk/dxvk_telemetry.h"

#include "thread.h"

#if defined(_WIN32) && !defined(_WIN64)
  #define DXVK_USE_UNMAPPABLE_MEMORY
#endif

namespace dxvk {

  class DxvkTelemetry;

  /**
   * \brief Unmappable memory statistics
   */
  struct DxvkMemoryFileStats {
    uint64_t memoryAllocated = 0u;
    uint64_t memoryUsed = 0u;
    uint64_t memoryMapped = 0u;
  };

#ifdef DXVK_USE_UNMAPPABLE_MEMORY
  /**
   * \brief Unmappable memory chunk
   *
   * Provides a file mapping that can be mapped into virtual
   * address space. Mappings are not cached and must respect
   * the system's minimum mapping granularity.
   */
  class MemoryFile {

  public:

    MemoryFile() = default;

    MemoryFile(size_t size, DxvkTelemetry* telemetry);

    MemoryFile(MemoryFile&& other);
    MemoryFile& operator = (MemoryFile&& other);

    ~MemoryFile();

    /**
     * \brief Mapping granularity, in bytes
     *
     * Offset and size must be multiples of this when mapping the
     * file, except for size when offset + size is equal to the
     * file size.
     */
    size_t granularity() const;

    /**
     * \brief Maps file to virtual address space
     *
     * \param [in] offset Offset, in bytes
     * \param [in] size Size, in bytes
     * \returns Pointer to mapped memory region
     */
    void* map(size_t offset, size_t size);

    /**
     * \brief Unmaps file
     * \param [in] ptr Pointer to mapped memory region
     */
    static void unmap(void* ptr, size_t size, DxvkTelemetry* telemetry);

    /**
     * \brief Checks whether the file is valid
     * \returns \c true for a valid file
     */
    explicit operator bool () const {
      return m_data.size != 0u;
    }

  private:

    struct Data {
      size_t size = 0u;

      #ifdef DXVK_USE_UNMAPPABLE_MEMORY
      HANDLE mapping = INVALID_HANDLE_VALUE;
      #else
      void* memory = nullptr;
      #endif
    };

    Data m_data;
    DxvkTelemetry* m_telemetry = nullptr;

    void destroy();

    void create(size_t size);

  };


  /**
   * \brief Unmappable memory allocator
   *
   * Manages unmappable memory files.
   */
  class MemoryFilePool {
    static constexpr size_t DefaultChunkSize = 16ull << 20u;
  public:

    MemoryFilePool();

    ~MemoryFilePool();

    MemoryFilePool(const MemoryFilePool&) = delete;
    MemoryFilePool& operator = (const MemoryFilePool&) = delete;
    void setTelemetry(DxvkTelemetry* telemetry) { m_telemetry = telemetry; }
    void enableRetirement() { m_allowRetirement = true; }
    void drainRetired();

    /**
     * \brief Allocates unmappable memory
     *
     * \param [in] size Size, in bytes
     * \returns Allocation index, or -1 on error
     */
    int32_t alloc(size_t size, bool* initiallyZero = nullptr);

    /**
     * \brief Frees unmappable memory
     * \returns Allocation index
     */
    void free(int32_t allocation);

    /**
     * \brief Maps memory into address space
     *
     * \param [in] allocation Allocation index
     * \returns Pointer to mapped memory region
     */
    void* map(int32_t allocation);

    /**
     * \brief Unmaps previously mapped memory
     *
     * \param [in] allocation Allocation index
     * \returns Pointer to mapped memory region
     */
    void unmap(int32_t allocation);

    /**
     * \brief Queries memory allocation statistics
     *
     * Note that this may not return consistent results, e.g. used
     * memory may be greater than allocated if there is a data race.
     * This is somewhat intended since some use benefit from not
     * locking the object and don't care about minor issues.
     * \returns Memory usage statistics
     */
    DxvkMemoryFileStats getStats() const {
      DxvkMemoryFileStats result = {};
      result.memoryAllocated = m_memoryAllocated.load(std::memory_order_relaxed);
      result.memoryUsed = m_memoryUsed.load(std::memory_order_relaxed);
      result.memoryMapped = m_memoryMapped.load(std::memory_order_relaxed);
      return result;
    }

  private:

    struct PageInfo {
      void*       mapPtr    = nullptr;
      size_t      mapCount  = 0u;
    };

    struct ChunkInfo {
      MemoryFile  file = {};
      size_t      size = 0u;
      // Exclusive end of every range ever handed out in this backing file.
      // Holes below this mark are conservatively treated as recycled memory.
      size_t      allocatedEnd = 0u;

      std::vector<PageInfo> pages;
    };

    struct AllocInfo {
      MemoryFile  file      = {};
      uint64_t    address   = 0u;
      size_t      size      = 0u;
      void*       mapPtr    = nullptr;
      size_t      mapCount  = 0u;
      uint64_t    cookie    = 0u;
    };

    dxvk::mutex m_mutex;
    DxvkTelemetry* m_telemetry = nullptr;
    uint32_t m_nextCookie = 0;

    DxvkPageAllocator m_pageAllocator;
    DxvkPoolAllocator m_poolAllocator;

    std::atomic<uint64_t> m_memoryAllocated = { 0u };
    std::atomic<uint64_t> m_memoryUsed = { 0u };
    std::atomic<uint64_t> m_memoryMapped = { 0u };

    std::vector<ChunkInfo>  m_chunks;
    std::vector<PageInfo>   m_pageInfo;
    std::vector<AllocInfo>  m_allocations;
    std::vector<int32_t>    m_freeList;

    // Only unreachable, finally released views may enter this queue. Live
    // texture budget eviction remains synchronous. No allocation on enqueue.
    struct Retired {
      MemoryFile file;
      void* view = nullptr;
      size_t viewBytes = 0;
      uint64_t mappedBytes = 0;
      uint64_t allocatedBytes = 0;
      uint64_t identity = 0;
      DxvkTelemetry::Token token;
    };
    static constexpr size_t RetireCapacity = 128;
    static constexpr uint64_t RetireByteLimit = 128ull << 20;
    static constexpr uint64_t RetireWorkingSet = 8ull << 20;
    std::array<Retired, RetireCapacity> m_retired;
    dxvk::mutex m_retireMutex;
    dxvk::condition_variable m_retireChanged;
    dxvk::thread m_retireThread;
    size_t m_retireHead = 0;
    std::atomic<size_t> m_retireCount = { 0 };
    uint64_t m_retireMappedBytes = 0, m_retireAllocatedBytes = 0;
    bool m_allowRetirement = false, m_retireStopped = false;

    Retired detachMapping(AllocInfo& info);
    bool retire(Retired&& entry);
    void finishRetired(Retired& entry);
    void retirementWorker();

    void addChunk(size_t minSize);

  };


  /**
   * \brief RAII wrapper around unmappable memory
   */
  class MemoryFileRegion {

  public:

    MemoryFileRegion() = default;

    MemoryFileRegion(MemoryFilePool& pool, size_t size)
    : m_pool      (&pool)
    , m_allocation(pool.alloc(size, &m_initiallyZero)) { }

    ~MemoryFileRegion() {
      free();
    }

    MemoryFileRegion(MemoryFileRegion&& other)
    : m_pool        (std::exchange(other.m_pool,        nullptr))
    , m_initiallyZero(std::exchange(other.m_initiallyZero, false))
    , m_allocation  (std::exchange(other.m_allocation,  -1))
    , m_mapPtr      (std::exchange(other.m_mapPtr,      nullptr)) {

    }

    MemoryFileRegion& operator = (MemoryFileRegion&& other) {
      free();

      m_pool        = std::exchange(other.m_pool,        nullptr);
      m_initiallyZero = std::exchange(other.m_initiallyZero, false);
      m_allocation  = std::exchange(other.m_allocation,  -1);
      m_mapPtr      = std::exchange(other.m_mapPtr,      nullptr);
      return *this;
    }

    /**
     * \brief Maps or retrieves mapped pointer
     * \returns Pointer to mapped memory region
     */
    void* map() {
      if (!m_mapPtr && m_allocation >= 0)
        m_mapPtr = m_pool->map(m_allocation);

      return m_mapPtr;
    }

    /**
     * \brief Unmaps memory
     */
    void unmap() {
      if (m_mapPtr) {
        m_pool->unmap(m_allocation);
        m_mapPtr = nullptr;
      }
    }

    /**
     * \brief Checks whether memory is currently mapped
     * \returns \c true if memory is currently mapped
     */
    bool isMapped() const {
      return m_mapPtr != nullptr;
    }

    // Allocation provenance, not a claim about contents after the first write.
    bool wasInitiallyZero() const { return m_initiallyZero; }

    /**
     * \brief Checks whether allocation is valid
     * \returns \c true if allocation is valid
     */
    explicit operator bool () const {
      return m_allocation > 0;;
    }

  private:

    MemoryFilePool* m_pool        = nullptr;
    bool            m_initiallyZero = false;
    int32_t         m_allocation  = -1;
    void*           m_mapPtr      = nullptr;

    void free() {
      if (m_allocation >= 0) {
        m_pool->free(m_allocation);
      }
    }

  };
#else
  /**
   * \brief Dummy memory file pool
   */
  class MemoryFilePool {

  public:

    void setTelemetry(DxvkTelemetry*) { }

    DxvkMemoryFileStats getStats() const {
      return DxvkMemoryFileStats();
    }

  };

  /**
   * \brief RAII wrapper around memory allocation
   *
   * No need to unmap on 64-bit platforms, so
   * just allocate and free memory directly.
   */
  class MemoryFileRegion {

  public:

    MemoryFileRegion() = default;

    MemoryFileRegion(MemoryFilePool& pool, size_t size)
    : m_data(std::malloc(size)) { }

    ~MemoryFileRegion() {
      if (m_data)
        std::free(m_data);
    }

    MemoryFileRegion(MemoryFileRegion&& other)
    : m_data(std::exchange(other.m_data, nullptr)) { }

    MemoryFileRegion& operator = (MemoryFileRegion&& other) {
      if (m_data)
        std::free(m_data);

      m_data = std::exchange(other.m_data, nullptr);
      return *this;
    }

    void* map() {
      return m_data;
    }

    void unmap() { }

    bool isMapped() const {
      return true;
    }

    bool wasInitiallyZero() const { return false; }

    explicit operator bool () const {
      return m_data != nullptr;
    }

  private:

    void* m_data = nullptr;

  };
#endif

}
