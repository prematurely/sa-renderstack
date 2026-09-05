#include <sa_api2/pass_registration.hpp>
#include <algorithm>
#include <cstring>
#include <utility>

namespace sa::api2 {
HRESULT STDMETHODCALLTYPE PassRegistration::trampoline(void* data, const D3D9GtaSaVulkanFrameContext* frame) {
  auto* state = static_cast<State*>(data);
  if (!state || !state->callback || state->failed.load()) return E_FAIL;
  const HRESULT hr = state->callback(state->userData, frame);
  if (FAILED(hr)) state->failed.store(true);
  return hr;
}
Result<PassRegistration> PassRegistration::create(ID3D9GtaSaCompatDevice1* device,
    std::span<const char> name, int priority, Callback callback, void* userData) {
  if (!device || !callback || name.size() >= 64) return std::unexpected(E_INVALIDARG);
  auto state = std::make_shared<State>(); state->callback = callback; state->userData = userData;
  D3D9GtaSaVulkanPassDesc desc{}; desc.StructSize=sizeof(desc); desc.ApiVersion=2;
  desc.Priority=priority; desc.Stage=D3D9_GTA_SA_VULKAN_PASS_AFTER_BLIT;
  desc.Flags=D3D9_GTA_SA_VULKAN_PASS_RESTORES_LAYOUTS;
  std::copy(name.begin(), name.end(), desc.Name); desc.Name[name.size()]='\0';
  desc.Record=&trampoline; desc.UserData=state.get();
  UINT64 token=0; const HRESULT hr=device->RegisterVulkanPass(&desc,&token);
  if (FAILED(hr) || token==0) return std::unexpected(hr);
  device->AddRef(); return PassRegistration(device,token,std::move(state));
}
HRESULT PassRegistration::close() noexcept {
  if (!device_ || token_==0) return S_FALSE;
  const HRESULT hr=device_->UnregisterVulkanPass(token_);
  if (SUCCEEDED(hr)) { token_=0; device_->Release(); device_=nullptr; state_.reset(); }
  return hr;
}
PassRegistration::~PassRegistration(){ (void)close(); }
PassRegistration::PassRegistration(PassRegistration&& o) noexcept : device_(std::exchange(o.device_,nullptr)), token_(std::exchange(o.token_,0)), state_(std::move(o.state_)) {}
PassRegistration& PassRegistration::operator=(PassRegistration&& o) noexcept { if(this!=&o){ close(); device_=std::exchange(o.device_,nullptr); token_=std::exchange(o.token_,0); state_=std::move(o.state_);} return *this; }
bool PassRegistration::callback_failed() const noexcept { return state_ && state_->failed.load(); }
} // namespace sa::api2
