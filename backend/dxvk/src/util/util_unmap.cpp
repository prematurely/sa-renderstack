#include "util_unmap.h"
#include "../dxvk/dxvk_telemetry.h"
#include <optional>

#include "./log/log.h"

#ifdef _WIN32
#include "./com/com_include.h"
#endif

namespace dxvk {

#ifdef DXVK_USE_UNMAPPABLE_MEMORY
  MemoryFile::MemoryFile(size_t size, DxvkTelemetry* telemetry) : m_telemetry(telemetry) {
    create(size);
  }


  MemoryFile::MemoryFile(MemoryFile&& other)
  : m_data(std::exchange(other.m_data, Data()))
  , m_telemetry(std::exchange(other.m_telemetry, nullptr)) { }


  MemoryFile& MemoryFile::operator = (MemoryFile&& other) {
    destroy();

    m_data = std::exchange(other.m_data, Data());
    m_telemetry = std::exchange(other.m_telemetry, nullptr);
    return *this;
  }


  MemoryFile::~MemoryFile() {
    destroy();
  }


  size_t MemoryFile::granularity() const {
    static std::atomic<size_t> s_granularity = { 0u };

    size_t result = s_granularity.load(std::memory_order_relaxed);

    if (result)
      return result;

    SYSTEM_INFO sysInfo = { };
    GetSystemInfo(&sysInfo);

    // Bump granularity to the allocator's page size for consistenty
    result = sysInfo.dwAllocationGranularity;
    result = std::max(result, uint32_t(DxvkPageAllocator::PageSize));

    s_granularity.store(result, std::memory_order_relaxed);
    return result;
  }


  void* MemoryFile::map(size_t offset, size_t size) {
    DxvkTelemetryScope scope(m_telemetry,D3D9_BRIDGE_TELEMETRY_MAP,0,size,
      D3D9_BRIDGE_TELEMETRY_FILE_BACKED|D3D9_BRIDGE_TELEMETRY_OS_MAPPING);
    auto* result=MapViewOfFile(m_data.mapping, FILE_MAP_ALL_ACCESS, 0, offset, size);
    scope.identity(reinterpret_cast<uintptr_t>(result));
    if (!result) scope.fail(int32_t(GetLastError()));
    return result;
  }


  void MemoryFile::unmap(void* ptr, size_t size, DxvkTelemetry* telemetry) {
    DxvkTelemetryScope scope(telemetry,D3D9_BRIDGE_TELEMETRY_UNMAP,reinterpret_cast<uintptr_t>(ptr),size,
      D3D9_BRIDGE_TELEMETRY_FILE_BACKED|D3D9_BRIDGE_TELEMETRY_OS_MAPPING);
    if (!UnmapViewOfFile(ptr)) scope.fail(int32_t(GetLastError()));
  }


  void MemoryFile::destroy() {
    if (!m_data.size)
      return;

    CloseHandle(m_data.mapping);
  }


  void MemoryFile::create(size_t size) {
    m_data.mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT, 0, size, nullptr);
    m_data.size = size;

    if (!m_data.mapping)
      throw DxvkError("Failed to create file mapping.");
  }




  MemoryFilePool::MemoryFilePool()
  : m_poolAllocator(m_pageAllocator) {

  }


  MemoryFilePool::~MemoryFilePool() {
    if (m_retireThread.joinable()) {
      if (this_thread::isInModuleDetachment()) {
        m_retireThread.detach();
        return;
      }
      { std::lock_guard lock(m_retireMutex); m_retireStopped = true; }
      m_retireChanged.notify_all();
      m_retireThread.join();
    }
  }

  void MemoryFilePool::drainRetired() {
    std::unique_lock lock(m_retireMutex);
    m_retireChanged.wait(lock, [this] { return !m_retireCount; });
  }

  void MemoryFilePool::finishRetired(Retired& entry) {
    if (entry.view)
      MemoryFile::unmap(entry.view, entry.viewBytes, m_telemetry);
    m_memoryMapped -= entry.mappedBytes;
    entry.file = MemoryFile();
    m_memoryAllocated -= entry.allocatedBytes;
    if (entry.token.generation)
      m_telemetry->finish(entry.token,D3D9_BRIDGE_TELEMETRY_MAPPING_RETIRE,
        entry.identity,entry.mappedBytes,0,D3D9_BRIDGE_TELEMETRY_FILE_BACKED|D3D9_BRIDGE_TELEMETRY_DEFERRED);
  }

  void MemoryFilePool::retirementWorker() {
    env::setThreadName("dxvk-map-free");
    for (;;) {
      Retired entry;
      { std::unique_lock lock(m_retireMutex);
        m_retireChanged.wait(lock, [this] { return m_retireCount || m_retireStopped; });
        if (!m_retireCount) return;
        entry = std::move(m_retired[m_retireHead]);
      }
      finishRetired(entry);
      { std::lock_guard lock(m_retireMutex);
        m_retireMappedBytes -= entry.viewBytes;
        m_retireAllocatedBytes -= entry.allocatedBytes;
        m_retireHead = (m_retireHead + 1) % RetireCapacity;
        --m_retireCount;
      }
      m_retireChanged.notify_all();
    }
  }

  bool MemoryFilePool::retire(Retired&& entry) {
    if (!entry.view && !entry.file) {
      m_memoryMapped -= entry.mappedBytes;
      return false;
    }
    // A small create/release loop would immediately wait for this same worker
    // when acquiring its next chunk. Keep those releases synchronous; continue
    // an already queued burst until it is fully retired.
    if (m_allowRetirement
        && (m_memoryMapped.load(std::memory_order_relaxed) >= RetireWorkingSet
          || m_retireCount.load(std::memory_order_relaxed))
        && entry.viewBytes <= RetireByteLimit && entry.allocatedBytes <= RetireByteLimit) {
      std::unique_lock lock(m_retireMutex);
      if (!m_retireThread.joinable()) {
        try {
          m_retireThread = dxvk::thread([this] { retirementWorker(); });
        } catch (...) {
          // Final resource release must still succeed if worker creation fails.
          m_allowRetirement = false;
        }
      }
      if (m_allowRetirement) {
        m_retireChanged.wait(lock, [this, &entry] {
          return m_retireCount < RetireCapacity
            && m_retireMappedBytes + entry.viewBytes <= RetireByteLimit
            && m_retireAllocatedBytes + entry.allocatedBytes <= RetireByteLimit;
        });
        m_retireMappedBytes += entry.viewBytes;
        m_retireAllocatedBytes += entry.allocatedBytes;
        if (m_telemetry) entry.token = m_telemetry->begin();
        m_retired[(m_retireHead + m_retireCount) % RetireCapacity] = std::move(entry);
        ++m_retireCount;
        lock.unlock();
        m_retireChanged.notify_all();
        return true;
      }
    }
    finishRetired(entry);
    return false;
  }


  int32_t MemoryFilePool::alloc(size_t size, bool* initiallyZero) {
    DxvkTelemetryScope scope(m_telemetry,D3D9_BRIDGE_TELEMETRY_ALLOC,0,size,D3D9_BRIDGE_TELEMETRY_FILE_BACKED);
    std::lock_guard lock(m_mutex);

    if (initiallyZero)
      *initiallyZero = false;

    int32_t index;
    const bool newSlot = m_freeList.empty();

    if (!m_freeList.empty()) {
      index = m_freeList.back();
      m_freeList.pop_back();
    } else {
      index = int32_t(m_allocations.size());
      m_allocations.emplace_back();
    }

    auto& allocInfo = m_allocations.at(index);

    try {
      if (size <= DxvkPageAllocator::MaxChunkSize) {
        auto address = (size <= DxvkPoolAllocator::MaxSize)
          ? m_poolAllocator.alloc(size)
          : m_pageAllocator.alloc(size, 1u);

        if (address < 0) {
          addChunk(size);

          address = (size <= DxvkPoolAllocator::MaxSize)
            ? m_poolAllocator.alloc(size)
            : m_pageAllocator.alloc(size, 1u);
        }

        if (address < 0) {
          if (newSlot)
            m_allocations.pop_back();
          else
            m_freeList.push_back(index);
          scope.fail(-1);
          return -1;
        }

        allocInfo.address = address;
        allocInfo.size = size;
        auto& chunk = m_chunks.at(address >> DxvkPageAllocator::ChunkAddressBits);
        auto offset = size_t(address & DxvkPageAllocator::ChunkAddressMask);
        if (initiallyZero)
          *initiallyZero = offset >= chunk.allocatedEnd;
        chunk.allocatedEnd = std::max(chunk.allocatedEnd, offset + size);
      } else {
        drainRetired();
        allocInfo.file = MemoryFile(size,m_telemetry);
        allocInfo.size = size;

        m_memoryAllocated += size;
        if (initiallyZero)
          *initiallyZero = true;
      }
    } catch (...) {
      // A failed backing-file creation must not strand an allocation slot.
      if (newSlot)
        m_allocations.pop_back();
      else
        m_freeList.push_back(index);
      scope.fail(-1);
      throw;
    }

    if (++m_nextCookie==0) ++m_nextCookie;
    allocInfo.cookie=(uint64_t(reinterpret_cast<uintptr_t>(this))<<32)|m_nextCookie;
    scope.identity(allocInfo.cookie);
    m_memoryUsed += size;
    return index;
  }


  void MemoryFilePool::free(int32_t allocation) {
    DxvkTelemetryScope scope(m_telemetry,D3D9_BRIDGE_TELEMETRY_FREE,0,0,D3D9_BRIDGE_TELEMETRY_FILE_BACKED);
    std::lock_guard lock(m_mutex);

    auto& allocInfo = m_allocations.at(allocation);
    scope.identity(allocInfo.cookie); scope.bytes(allocInfo.size);
    m_memoryUsed -= allocInfo.size;
    Retired retired;
    std::optional<DxvkTelemetryScope> unmapScope;
    if (allocInfo.mapCount) {
      unmapScope.emplace(m_telemetry,D3D9_BRIDGE_TELEMETRY_UNMAP,
        allocInfo.cookie,allocInfo.size,D3D9_BRIDGE_TELEMETRY_FILE_BACKED);
      retired = detachMapping(allocInfo);
    }
    retired.identity = allocInfo.cookie;

    if (allocInfo.size <= DxvkPageAllocator::MaxChunkSize) {
      auto chunkIndex = allocInfo.address >> DxvkPageAllocator::ChunkAddressBits;

      if (allocInfo.size <= DxvkPoolAllocator::MaxSize)
        m_poolAllocator.free(allocInfo.address, allocInfo.size);
      else
        m_pageAllocator.free(allocInfo.address, allocInfo.size);

      if (!m_pageAllocator.pagesUsed(chunkIndex)) {
        auto& chunk = m_chunks.at(chunkIndex);
        retired.file = std::move(chunk.file);
        retired.allocatedBytes = chunk.size;
        m_pageAllocator.removeChunk(chunkIndex);
        chunk = ChunkInfo();
      }
    } else {
      retired.file = std::move(allocInfo.file);
      retired.allocatedBytes = allocInfo.size;
    }

    allocInfo = AllocInfo();

    m_freeList.push_back(allocation);
    const bool deferred = retire(std::move(retired));
    if (deferred && unmapScope)
      unmapScope->flags(D3D9_BRIDGE_TELEMETRY_DEFERRED);
  }


  void* MemoryFilePool::map(int32_t allocation) {
    DxvkTelemetryScope scope(m_telemetry,D3D9_BRIDGE_TELEMETRY_MAP,0,0,D3D9_BRIDGE_TELEMETRY_FILE_BACKED);
    std::lock_guard lock(m_mutex);

    auto& allocInfo = m_allocations.at(allocation);
    scope.identity(allocInfo.cookie); scope.bytes(allocInfo.size);

    if (!allocInfo.mapCount) {
      auto mapFile = [this] (MemoryFile& file, size_t offset, size_t size) {
        const bool pending = m_retireCount.load(std::memory_order_relaxed) != 0;
        auto ptr = file.map(offset, size);
        if (!ptr && pending) {
          // A final-release view may still occupy scarce 32-bit address space.
          // Complete it and retry once before reporting an allocation failure.
          drainRetired();
          ptr = file.map(offset, size);
        }
        return ptr;
      };
      if (allocInfo.file) {
        // Map entire file if the allocation owns it
        allocInfo.mapPtr = mapFile(allocInfo.file, 0u, allocInfo.size);
      } else {
        // Check whether the allocation is contained inside one page
        auto& chunk = m_chunks.at(allocInfo.address >> DxvkPageAllocator::ChunkAddressBits);
        auto chunkOffset = allocInfo.address & DxvkPageAllocator::ChunkAddressMask;

        auto pageSize = chunk.file.granularity();
        auto pageOffset = (chunkOffset % pageSize);

        auto pageLo = (chunkOffset / pageSize);
        auto pageHi = (chunkOffset + allocInfo.size - 1u) / pageSize;

        if (pageLo == pageHi) {
          if (chunk.pages.empty())
            chunk.pages.resize((chunk.size + pageSize - 1u) / pageSize);

          auto& page = chunk.pages.at(pageLo);

          if (!page.mapCount)
            page.mapPtr = mapFile(chunk.file, chunkOffset - pageOffset, pageSize);

          if (page.mapPtr) {
            ++page.mapCount;
            allocInfo.mapPtr = reinterpret_cast<char*>(page.mapPtr) + pageOffset;
          }
        } else {
          // We're straddling multiple pages, map them all and offset the base address
          auto mapPtr = mapFile(chunk.file, pageLo * pageSize, (pageHi - pageLo + 1u) * pageSize);
          if (mapPtr)
            allocInfo.mapPtr = reinterpret_cast<char*>(mapPtr) + pageOffset;
        }
      }

      if (!allocInfo.mapPtr) {
        scope.fail(-1);
        return nullptr;
      }

      // Failed views own no reference and consume no residency budget. In
      // particular, never turn a failed shared-page map into a non-null offset.
      m_memoryMapped += allocInfo.size;
    }

    ++allocInfo.mapCount;
    return allocInfo.mapPtr;
  }


  void MemoryFilePool::unmap(int32_t allocation) {
    DxvkTelemetryScope scope(m_telemetry,D3D9_BRIDGE_TELEMETRY_UNMAP,0,0,D3D9_BRIDGE_TELEMETRY_FILE_BACKED);
    std::lock_guard lock(m_mutex);

    auto& allocInfo = m_allocations.at(allocation);
    scope.identity(allocInfo.cookie); scope.bytes(allocInfo.size);

    auto retired = detachMapping(allocInfo);
    finishRetired(retired);
  }

  MemoryFilePool::Retired MemoryFilePool::detachMapping(AllocInfo& allocInfo) {
    Retired retired;

    if (!(--allocInfo.mapCount)) {
      retired.mappedBytes = allocInfo.size;

      if (allocInfo.file) {
        // Unmap entire file
        retired.view = allocInfo.mapPtr;
        retired.viewBytes = allocInfo.size;
      } else {
        // Check whether the allocation is contained inside one page
        auto& chunk = m_chunks.at(allocInfo.address >> DxvkPageAllocator::ChunkAddressBits);
        auto chunkOffset = allocInfo.address & DxvkPageAllocator::ChunkAddressMask;

        auto pageSize = chunk.file.granularity();
        auto pageOffset = (chunkOffset % pageSize);

        auto pageLo = (chunkOffset / pageSize);
        auto pageHi = (chunkOffset + allocInfo.size - 1u) / pageSize;

        if (pageLo == pageHi) {
          auto& page = chunk.pages.at(pageLo);

          if (!(--page.mapCount)) {
            retired.view = page.mapPtr;
            retired.viewBytes = pageSize;
            page.mapPtr = nullptr;
          }
        } else {
          // Apply page offset to mapped pointer and unmap all pages
          auto basePtr = reinterpret_cast<char*>(allocInfo.mapPtr) - pageOffset;
          retired.view = basePtr;
          retired.viewBytes = (pageHi-pageLo+1u)*pageSize;
        }
      }

      allocInfo.mapPtr = nullptr;
    }
    return retired;
  }


  void MemoryFilePool::addChunk(size_t minSize) {
    // A freed chunk can still be owned by the retirement worker. Complete it
    // before committing replacement backing storage, including under VA pressure.
    drainRetired();
    auto chunkSize = DefaultChunkSize;

    while (chunkSize < minSize)
      chunkSize <<= 1u;

    // Establish backing storage before publishing the free range. Otherwise a
    // failed CreateFileMapping leaves the next allocation pointing at no file.
    MemoryFile file(chunkSize, m_telemetry);
    auto chunkIndex = m_pageAllocator.addChunk(chunkSize);

    try {
      if (chunkIndex >= m_chunks.size())
        m_chunks.resize(chunkIndex + 1u);
    } catch (...) {
      m_pageAllocator.removeChunk(chunkIndex);
      throw;
    }

    auto& chunk = m_chunks.at(chunkIndex);
    chunk.file = std::move(file);
    chunk.size = chunkSize;

    m_memoryAllocated += chunk.size;
  }


#endif

}
