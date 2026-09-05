#include "api4_journal_session.hpp"
namespace api4 {
std::expected<JournalSession, HRESULT> JournalSession::begin(ID3D9GtaSaCompatDevice3* d) noexcept {
  if (!d) return std::unexpected(E_POINTER);
  const HRESULT hr = d->BeginStateJournal();
  if (FAILED(hr)) return std::unexpected(hr);
  return JournalSession(d, GetCurrentThreadId());
}
HRESULT JournalSession::restore() noexcept { if (!m_active) return D3DERR_INVALIDCALL; if (GetCurrentThreadId()!=m_thread) return D3DERR_INVALIDCALL; HRESULT hr=m_device->RestoreStateJournal(); if (SUCCEEDED(hr)||FAILED(hr)) m_active=false; return hr; }
HRESULT JournalSession::on_reset() noexcept { if (!m_active) return D3DERR_INVALIDCALL; if (GetCurrentThreadId()!=m_thread) return D3DERR_INVALIDCALL; m_active=false; return S_OK; }
void JournalSession::release() noexcept { if(m_device){ m_device->Release(); m_device=nullptr; } m_active=false; }
JournalSession::JournalSession(JournalSession&& o) noexcept : m_device(o.m_device),m_thread(o.m_thread),m_active(o.m_active){o.m_device=nullptr;o.m_active=false;}
JournalSession& JournalSession::operator=(JournalSession&& o) noexcept { if(this!=&o){ if(m_active&&GetCurrentThreadId()==m_thread) (void)restore(); release(); m_device=o.m_device;m_thread=o.m_thread;m_active=o.m_active;o.m_device=nullptr;o.m_active=false;} return *this; }
JournalSession::~JournalSession() noexcept { if(m_active&&GetCurrentThreadId()==m_thread) (void)restore(); release(); }
}
