#pragma once
#include <sa_renderstack/backend_api.h>
#include <expected>
#include <string>
#include <thread>
namespace api7 {
// An active journal must be restored, moved and destroyed on its owner thread.
// Moving transfers ownership; it does not change the backend transaction thread.
class SelectiveJournal {
public:
  explicit SelectiveJournal(ID3D9GtaSaCompatDevice6* api);
  SelectiveJournal(const SelectiveJournal&) = delete;
  SelectiveJournal& operator=(const SelectiveJournal&) = delete;
  SelectiveJournal(SelectiveJournal&& other) noexcept;
  SelectiveJournal& operator=(SelectiveJournal&& other) noexcept;
  ~SelectiveJournal();

  std::expected<void, std::string> begin() noexcept;
  std::expected<void, std::string> capture(bool on) noexcept;
  std::expected<void, std::string> restore() noexcept;

private:
  void close() noexcept;
  ID3D9GtaSaCompatDevice6* api_{};
  std::thread::id owner_;
  bool active_{};
};
}
