#include <sa_api1/status_client.hpp>

namespace sa::api1 {

void InteropHandle::reset() noexcept {
  if (ptr) reinterpret_cast<IUnknown*>(ptr)->Release();
  ptr = nullptr;
}

void StatusClient::reset() noexcept {
  if (compat_) compat_->Release();
  compat_ = nullptr;
}

Result<StatusClient> StatusClient::from_device(IDirect3DDevice9* device) noexcept {
  if (!device) return std::unexpected(E_POINTER);
  ID3D9GtaSaCompatDevice* compat = nullptr;
  const HRESULT hr = device->QueryInterface(__uuidof(ID3D9GtaSaCompatDevice),
      reinterpret_cast<void**>(&compat));
  if (FAILED(hr) || !compat) return std::unexpected(hr);
  return StatusClient(compat);
}

Result<D3D9GtaSaCompatStatus> StatusClient::status() const noexcept {
  if (!compat_) return std::unexpected(E_UNEXPECTED);
  D3D9GtaSaCompatStatus value{};
  value.StructSize = sizeof(value);
  const HRESULT hr = compat_->GetStatus(&value);
  if (FAILED(hr)) return std::unexpected(hr);
  return value;
}

Result<InteropHandle> StatusClient::vulkan_interop() const noexcept {
  if (!compat_) return std::unexpected(E_UNEXPECTED);
  ID3D9VkInteropDevice* value = nullptr;
  const HRESULT hr = compat_->GetVulkanInterop(&value);
  if (FAILED(hr) || !value) return std::unexpected(hr);
  return InteropHandle(value);
}

Result<void> StatusClient::validate(const D3D9GtaSaCompatStatus& value) noexcept {
  if (value.StructSize < sizeof(D3D9GtaSaCompatStatus))
    return std::unexpected(D3DERR_INVALIDCALL);
  if (value.ApiVersion < D3D9_GTA_SA_COMPAT_API_VERSION)
    return std::unexpected(E_NOTIMPL);
  constexpr UINT required = D3D9_GTA_SA_COMPAT_ACTIVE |
      D3D9_GTA_SA_COMPAT_VULKAN_BACKEND | D3D9_GTA_SA_COMPAT_VULKAN_INTEROP;
  if ((value.Flags & required) != required)
    return std::unexpected(E_NOINTERFACE);
  return {};
}

} // namespace sa::api1
