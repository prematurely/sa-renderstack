#include "api7_selective_journal.hpp"
#include "../../tests/test_check.hpp"

#include <thread>
#include <type_traits>
#include <utility>

// Supplementary ownership/failure controls; runtime tests use a real device.
struct JournalControl final : ID3D9GtaSaCompatDevice6 {
  ULONG refs = 1;
  unsigned begins = 0, restores = 0, captures = 0;
  bool active = false;
  HRESULT restoreResult = D3D_OK;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override {
    if (out) *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override { return --refs; }
  HRESULT STDMETHODCALLTYPE GetStatus(D3D9GtaSaCompatStatus*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetVulkanInterop(ID3D9VkInteropDevice**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE RegisterVulkanPass(const D3D9GtaSaVulkanPassDesc*, UINT64*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE UnregisterVulkanPass(UINT64) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SubmitStateBatch(const D3D9GtaSaStateBatch*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE BeginStateJournal() override { return BeginSelectiveStateJournal(); }
  HRESULT STDMETHODCALLTYPE RestoreStateJournal() override {
    ++restores;
    active = false;
    return restoreResult;
  }
  HRESULT STDMETHODCALLTYPE SubmitEffectStateBatch(const D3D9GtaSaEffectStateBatch*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SubmitStateDrawBatch(const D3D9GtaSaStateDrawBatch*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE BeginSelectiveStateJournal() override {
    ++begins;
    if (active) return D3DERR_INVALIDCALL;
    active = true;
    return D3D_OK;
  }
  HRESULT STDMETHODCALLTYPE SetStateJournalCaptureEnabled(BOOL) override {
    ++captures;
    return active ? D3D_OK : D3DERR_INVALIDCALL;
  }
};

int main() {
  CHECK(!std::is_copy_constructible_v<api7::SelectiveJournal>);
  CHECK(!std::is_copy_assignable_v<api7::SelectiveJournal>);
  {
    api7::SelectiveJournal empty(nullptr);
    CHECK(!empty.begin());
    CHECK(!empty.capture(true));
    CHECK(!empty.restore());
  }
  JournalControl control;
  {
    api7::SelectiveJournal journal(&control);
    CHECK(control.refs == 2);
    CHECK(journal.begin().has_value());
    CHECK(journal.capture(true).has_value());
    bool wrongThreadRejected = false;
    std::thread foreign([&] { wrongThreadRejected = !journal.capture(false); });
    foreign.join();
    CHECK(wrongThreadRejected);
    CHECK(control.captures == 1);
  }
  CHECK(control.refs == 1);
  CHECK(control.restores == 1);
  CHECK(!control.active);

  JournalControl source, destination;
  {
    api7::SelectiveJournal original(&source);
    CHECK(original.begin().has_value());
    api7::SelectiveJournal moved(std::move(original));
    CHECK(!original.capture(true));
    CHECK(moved.capture(true).has_value());
    api7::SelectiveJournal target(&destination);
    CHECK(target.begin().has_value());
    target = std::move(moved);
    CHECK(!moved.restore());
    CHECK(destination.restores == 1);
    CHECK(destination.refs == 1);
    CHECK(target.restore().has_value());
  }
  CHECK(source.refs == 1);
  CHECK(source.restores == 1);
  CHECK(destination.refs == 1);

  JournalControl reset;
  {
    api7::SelectiveJournal journal(&reset);
    CHECK(journal.begin().has_value());
    reset.active = false;
    reset.restoreResult = D3DERR_INVALIDCALL;
    CHECK(!journal.restore());
    CHECK(!journal.capture(true));
    reset.restoreResult = D3D_OK;
    CHECK(journal.begin().has_value());
    CHECK(journal.restore().has_value());
  }
  CHECK(reset.refs == 1);
  CHECK(reset.restores == 2);
  return api_test::finish();
}
