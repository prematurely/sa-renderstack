#include "api3_state_batch.hpp"
#include "../../tests/test_check.hpp"

#include <array>

namespace {
void check_values(const D3D9GtaSaFloatConstantRange& range,
    UINT start, std::span<const float> expected) {
  CHECK(range.StartRegister == start);
  CHECK(range.RegisterCount * 4 == expected.size());
  if (range.RegisterCount * 4 == expected.size()) {
    for (std::size_t i = 0; i < expected.size(); ++i)
      CHECK(range.Data[i] == expected[i]);
  }
}

void last_write_wins(bool pixel) {
  api3::StateBatchBuilder builder;
  auto add = [&](UINT start, std::span<const float> value) {
    return pixel ? builder.add_pixel_float(start, value)
                 : builder.add_vertex_float(start, value);
  };
  const std::array<float, 4> old{1, 2, 3, 4};
  const std::array<float, 8> replacement{5, 6, 7, 8, 9, 10, 11, 12};
  CHECK(add(1, old).has_value());
  CHECK(add(0, replacement).has_value());
  auto view = builder.view();
  CHECK(view.has_value());
  if (!view) return;
  const auto count = pixel ? view->PixelFloatRangeCount : view->VertexFloatRangeCount;
  CHECK(count == 1);
  if (count != 1) return;
  check_values(pixel ? view->PixelFloatRanges[0] : view->VertexFloatRanges[0], 0, replacement);

  CHECK(add(0, old).has_value());
  const std::array<float, 8> partial{1, 2, 3, 4, 9, 10, 11, 12};
  view = builder.view();
  CHECK(view.has_value());
  if (view) check_values(pixel ? view->PixelFloatRanges[0] : view->VertexFloatRanges[0], 0, partial);

  CHECK(add(0, replacement).has_value());
  view = builder.view();
  CHECK(view.has_value());
  if (view) check_values(pixel ? view->PixelFloatRanges[0] : view->VertexFloatRanges[0], 0, replacement);
}

void adjacency_and_boundaries() {
  api3::StateBatchBuilder builder;
  const std::array<float, 4> a{1, 2, 3, 4}, b{5, 6, 7, 8};
  CHECK(builder.add_vertex_float(0, a).has_value());
  CHECK(builder.add_vertex_float(1, b).has_value());
  auto view = builder.view();
  CHECK(view && view->VertexFloatRangeCount == 1);
  const std::array<float, 8> expected{1, 2, 3, 4, 5, 6, 7, 8};
  if (view && view->VertexFloatRangeCount == 1) check_values(view->VertexFloatRanges[0], 0, expected);
  CHECK(builder.add_vertex_float(255, a).has_value());
  CHECK(builder.add_pixel_float(223, b).has_value());
  CHECK(!builder.add_vertex_float(256, a).has_value());
  CHECK(!builder.add_pixel_float(224, a).has_value());
  CHECK(!builder.add_vertex_float(0, std::span(a).first(3)).has_value());
  CHECK(builder.add_vertex_float(256, {}).has_value());
  view = builder.view();
  CHECK(view && view->VertexFloatRangeCount == 2 && view->PixelFloatRangeCount == 1);
  if (view && view->VertexFloatRangeCount == 2) {
    check_values(view->VertexFloatRanges[0], 0, expected);
    check_values(view->VertexFloatRanges[1], 255, a);
    check_values(view->PixelFloatRanges[0], 223, b);
  }
}
}

int main() {
  last_write_wins(false);
  last_write_wins(true);
  adjacency_and_boundaries();
  return api_test::finish();
}
