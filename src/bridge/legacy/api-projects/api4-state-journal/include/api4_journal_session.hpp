#pragma once
#include <sa_renderstack/backend_api.h>
#include <expected>
#include <windows.h>

namespace api4 {
class JournalSession {
public:
  static std::expected<JournalSession, HRESULT> begin(ID3D9GtaSaCompatDevice3* device) noexcept;
  JournalSession(JournalSession&& other) noexcept;
  JournalSession& operator=(JournalSession&& other) noexcept;
  JournalSession(const JournalSession&) = delete;
  JournalSession& operator=(const JournalSession&) = delete;
  ~JournalSession() noexcept;
  HRESULT restore() noexcept;
  HRESULT on_reset() noexcept;
  bool active() const noexcept { return m_active; }
private:
  explicit JournalSession(ID3D9GtaSaCompatDevice3* d, DWORD tid) noexcept : m_device(d), m_thread(tid), m_active(true) { m_device->AddRef(); }
  ID3D9GtaSaCompatDevice3* m_device{}; DWORD m_thread{}; bool m_active{};
  void release() noexcept;
};
}
