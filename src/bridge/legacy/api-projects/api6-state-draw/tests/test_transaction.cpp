#include "api6_state_draw.hpp"
#include "../../tests/test_check.hpp"

int main() {
  api6::StateDrawTransaction transaction(nullptr);
  D3D9GtaSaStateBatch state{};
  auto result = transaction.submit(
    state, api6::DrawKind::Primitive, D3DPT_TRIANGLELIST, 0u, 1u);
  CHECK(!result.has_value());
  return api_test::finish();
}
