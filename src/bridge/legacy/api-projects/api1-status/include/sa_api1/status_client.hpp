#pragma once

#include <sa_renderstack/backend_api.h>

#include <expected>
#include <utility>

namespace sa::api1 {

template <typename T>
using Result = std::expected<T, HRESULT>;

struct InteropHandle {
  ID3D9VkInteropDevice* ptr{};

  InteropHandle() = default;
  explicit InteropHandle(ID3D9VkInteropDevice* value) : ptr(value) {}
  InteropHandle(const InteropHandle&) = delete;
  InteropHandle& operator=(const InteropHandle&) = delete;
  InteropHandle(InteropHandle&& other) noexcept : ptr(std::exchange(other.ptr, nullptr)) {}
  InteropHandle& operator=(InteropHandle&& other) noexcept {
    if (this != &other) { reset(); ptr = std::exchange(other.ptr, nullptr); }
    return *this;
  }
  ~InteropHandle() { reset(); }

  void reset() noexcept;
  explicit operator bool() const noexcept { return ptr != nullptr; }
  ID3D9VkInteropDevice* get() const noexcept { return ptr; }
};

class StatusClient {
public:
  StatusClient() = default;
  StatusClient(const StatusClient&) = delete;
  StatusClient& operator=(const StatusClient&) = delete;
  StatusClient(StatusClient&& other) noexcept : compat_(std::exchange(other.compat_, nullptr)) {}
  StatusClient& operator=(StatusClient&& other) noexcept {
    if (this != &other) { reset(); compat_ = std::exchange(other.compat_, nullptr); }
    return *this;
  }
  ~StatusClient() { reset(); }

  static Result<StatusClient> from_device(IDirect3DDevice9* device) noexcept;
  Result<D3D9GtaSaCompatStatus> status() const noexcept;
  Result<InteropHandle> vulkan_interop() const noexcept;
  static Result<void> validate(const D3D9GtaSaCompatStatus& status) noexcept;
  explicit operator bool() const noexcept { return compat_ != nullptr; }

private:
  explicit StatusClient(ID3D9GtaSaCompatDevice* compat) : compat_(compat) {}
  void reset() noexcept;
  ID3D9GtaSaCompatDevice* compat_{};
};

} // namespace sa::api1
