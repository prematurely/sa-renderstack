#include "api7_selective_journal.hpp"

#include <utility>

namespace api7 {
SelectiveJournal::SelectiveJournal(ID3D9GtaSaCompatDevice6* api)
  : api_(api), owner_(std::this_thread::get_id()) {
  if (api_) api_->AddRef();
}

SelectiveJournal::SelectiveJournal(SelectiveJournal&& other) noexcept
  : api_(std::exchange(other.api_, nullptr)), owner_(other.owner_),
    active_(std::exchange(other.active_, false)) { }

SelectiveJournal& SelectiveJournal::operator=(SelectiveJournal&& other) noexcept {
  if (this != &other) {
    close();
    api_ = std::exchange(other.api_, nullptr);
    owner_ = other.owner_;
    active_ = std::exchange(other.active_, false);
  }
  return *this;
}

void SelectiveJournal::close() noexcept {
  if (api_) {
    if (active_ && std::this_thread::get_id() == owner_)
      (void)api_->RestoreStateJournal();
    active_ = false;
    api_->Release();
    api_ = nullptr;
  }
}

SelectiveJournal::~SelectiveJournal() { close(); }

std::expected<void, std::string> SelectiveJournal::begin() noexcept {
  if (!api_ || std::this_thread::get_id() != owner_)
    return std::unexpected("same-thread contract");
  if (active_) return std::unexpected("already active");
  if (FAILED(api_->BeginSelectiveStateJournal())) return std::unexpected("begin failed");
  active_ = true;
  return {};
}

std::expected<void, std::string> SelectiveJournal::capture(bool on) noexcept {
  if (!active_ || std::this_thread::get_id() != owner_)
    return std::unexpected("inactive or wrong thread");
  if (FAILED(api_->SetStateJournalCaptureEnabled(on))) return std::unexpected("capture failed");
  return {};
}

std::expected<void, std::string> SelectiveJournal::restore() noexcept {
  if (!active_ || std::this_thread::get_id() != owner_)
    return std::unexpected("inactive or wrong thread");
  const HRESULT hr = api_->RestoreStateJournal();
  active_ = false;
  if (FAILED(hr)) return std::unexpected("restore failed (reset invalidates journal)");
  return {};
}
}
