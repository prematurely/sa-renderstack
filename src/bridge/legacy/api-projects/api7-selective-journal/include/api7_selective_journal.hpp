#pragma once
#include <sa_renderstack/backend_api.h>
#include <expected>
#include <string>
#include <thread>
namespace api7 { class SelectiveJournal { ID3D9GtaSaCompatDevice6* api_{}; std::thread::id owner_; bool active_{}; public: explicit SelectiveJournal(ID3D9GtaSaCompatDevice6*a):api_(a),owner_(std::this_thread::get_id()){if(api_)api_->AddRef();} ~SelectiveJournal(){if(api_)api_->Release();} std::expected<void,std::string> begin() noexcept; std::expected<void,std::string> capture(bool on) noexcept; std::expected<void,std::string> restore() noexcept; }; }
