#pragma once
#include <sa_renderstack/backend_api.h>
#include <expected>
#include <string>
namespace api6 { enum class DrawKind:UINT { Primitive=D3D9_GTA_SA_DRAW_PRIMITIVE, Indexed=D3D9_GTA_SA_DRAW_INDEXED_PRIMITIVE }; class StateDrawTransaction { ID3D9GtaSaCompatDevice5* api_{}; D3D9GtaSaStateDrawBatch batch_{}; public: explicit StateDrawTransaction(ID3D9GtaSaCompatDevice5*a):api_(a){if(api_)api_->AddRef();} ~StateDrawTransaction(){if(api_)api_->Release();} StateDrawTransaction(const StateDrawTransaction&)=delete; std::expected<HRESULT,std::string> submit(const D3D9GtaSaStateBatch& state, DrawKind kind, D3DPRIMITIVETYPE type, UINT start, UINT count, INT base=0, UINT min=0, UINT verts=0) noexcept; }; }
