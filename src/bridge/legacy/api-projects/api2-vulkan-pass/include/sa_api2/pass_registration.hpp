#pragma once

#include <sa_renderstack/backend_api.h>

#include <atomic>
#include <expected>
#include <memory>
#include <span>

namespace sa::api2 {
template <typename T> using Result = std::expected<T, HRESULT>;
using Callback = D3D9GtaSaRecordVulkanPass;

class PassRegistration {
public:
  PassRegistration() = default;
  PassRegistration(const PassRegistration&) = delete;
  PassRegistration& operator=(const PassRegistration&) = delete;
  PassRegistration(PassRegistration&&) noexcept;
  PassRegistration& operator=(PassRegistration&&) noexcept;
  ~PassRegistration();

  static Result<PassRegistration> create(ID3D9GtaSaCompatDevice1* device,
      std::span<const char> name, int priority, Callback callback, void* userData);
  HRESULT close() noexcept;
  bool closed() const noexcept { return token_ == 0; }
  bool callback_failed() const noexcept;
  UINT64 token() const noexcept { return token_; }

private:
  struct State { Callback callback{}; void* userData{}; std::atomic_bool failed{false}; };
  PassRegistration(ID3D9GtaSaCompatDevice1* device, UINT64 token, std::shared_ptr<State> state)
      : device_(device), token_(token), state_(std::move(state)) {}
  ID3D9GtaSaCompatDevice1* device_{};
  UINT64 token_{};
  std::shared_ptr<State> state_{};
  static HRESULT STDMETHODCALLTYPE trampoline(void*, const D3D9GtaSaVulkanFrameContext*);
};
} // namespace sa::api2
