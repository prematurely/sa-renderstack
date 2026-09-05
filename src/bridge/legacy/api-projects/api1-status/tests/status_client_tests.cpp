#include <sa_api1/status_client.hpp>
#include <cassert>

int main() {
  D3D9GtaSaCompatStatus good{};
  good.StructSize = sizeof(good);
  good.ApiVersion = D3D9_GTA_SA_COMPAT_API_VERSION;
  good.Flags = D3D9_GTA_SA_COMPAT_ACTIVE | D3D9_GTA_SA_COMPAT_VULKAN_BACKEND |
               D3D9_GTA_SA_COMPAT_VULKAN_INTEROP;
  assert(sa::api1::StatusClient::validate(good).has_value());
  good.ApiVersion = 1;
  assert(!sa::api1::StatusClient::validate(good).has_value());
  return 0;
}
