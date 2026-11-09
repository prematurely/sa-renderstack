#include "../src/dxvk/dxvk_format.h"
#include "../src/dxvk/dxvk_util.h"
#include "../src/dxvk/dxvk_graphics_state.h"
#include "../src/d3d9/d3d9_state.h"

#include "../src/d3d9/d3d9_binding_fast_path.h"
#include "../src/d3d9/d3d9_state_capture.h"
#include <cstdio>
#include <cstdlib>

// Keep calls and checks active in Release builds.
#define CHECK(...) do { if (!(__VA_ARGS__)) { \
  std::fprintf(stderr, "FAIL %s:%u: %s\n", __FILE__, __LINE__, #__VA_ARGS__); \
  std::exit(1); } } while (false)

namespace {

void testInputAssemblyEquality() {
  const dxvk::DxvkIaInfo first(
    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE, 0u);
  const dxvk::DxvkIaInfo same(
    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE, 0u);
  const dxvk::DxvkIaInfo different(
    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, VK_FALSE, 0u);

  CHECK(first.eq(same));
  CHECK(!first.eq(different));
}

void testMultisampleEquality() {
  const dxvk::DxvkMsInfo first(
    VK_SAMPLE_COUNT_1_BIT, 0xffffu, VK_FALSE);
  const dxvk::DxvkMsInfo same(
    VK_SAMPLE_COUNT_1_BIT, 0xffffu, VK_FALSE);
  const dxvk::DxvkMsInfo different(
    VK_SAMPLE_COUNT_1_BIT, 0xff0fu, VK_FALSE);

  CHECK(first.eq(same));
  CHECK(!first.eq(different));
}

void testBlendEquality() {
  const dxvk::DxvkOmAttachmentBlend first(
    VK_FALSE,
    VK_BLEND_FACTOR_ONE,
    VK_BLEND_FACTOR_ZERO,
    VK_BLEND_OP_ADD,
    VK_BLEND_FACTOR_ONE,
    VK_BLEND_FACTOR_ZERO,
    VK_BLEND_OP_ADD,
    VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
      | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);
  const dxvk::DxvkOmAttachmentBlend same(
    VK_FALSE,
    VK_BLEND_FACTOR_ONE,
    VK_BLEND_FACTOR_ZERO,
    VK_BLEND_OP_ADD,
    VK_BLEND_FACTOR_ONE,
    VK_BLEND_FACTOR_ZERO,
    VK_BLEND_OP_ADD,
    VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
      | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);
  const dxvk::DxvkOmAttachmentBlend different(
    VK_TRUE,
    VK_BLEND_FACTOR_ONE,
    VK_BLEND_FACTOR_ZERO,
    VK_BLEND_OP_ADD,
    VK_BLEND_FACTOR_ONE,
    VK_BLEND_FACTOR_ZERO,
    VK_BLEND_OP_ADD,
    VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
      | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);

  CHECK(first.eq(same));
  CHECK(!first.eq(different));
}

void testSamplerEquality() {
  std::array<DWORD, dxvk::SamplerStateCount> firstState = {};
  firstState[D3DSAMP_ADDRESSU] = D3DTADDRESS_WRAP;
  firstState[D3DSAMP_ADDRESSV] = D3DTADDRESS_WRAP;
  firstState[D3DSAMP_ADDRESSW] = D3DTADDRESS_WRAP;
  firstState[D3DSAMP_MAGFILTER] = D3DTEXF_LINEAR;
  firstState[D3DSAMP_MINFILTER] = D3DTEXF_LINEAR;
  firstState[D3DSAMP_MIPFILTER] = D3DTEXF_LINEAR;
  firstState[D3DSAMP_MAXANISOTROPY] = 16u;

  auto sameState = firstState;
  sameState[D3DSAMP_BORDERCOLOR] = 0x12345678u;
  sameState[D3DSAMP_ELEMENTINDEX] = 7u;
  sameState[D3DSAMP_DMAPOFFSET] = 11u;

  auto differentState = firstState;
  differentState[D3DSAMP_MINFILTER] = D3DTEXF_POINT;

  const dxvk::D3D9SamplerInfo first(firstState);
  CHECK(first.eq(dxvk::D3D9SamplerInfo(sameState)));
  CHECK(!first.eq(dxvk::D3D9SamplerInfo(differentState)));

  auto borderState = firstState;
  borderState[D3DSAMP_ADDRESSU] = D3DTADDRESS_BORDER;
  borderState[D3DSAMP_BORDERCOLOR] = 0x12345678u;
  CHECK(!first.eq(dxvk::D3D9SamplerInfo(borderState)));
}

void testSingleBindingDispatch() {
  UINT calls = 0, slot = 99;
  const auto bind = [&](uint32_t i) { ++calls; slot = i; };
  CHECK(!dxvk::TryD3D9SingleBinding(0, bind));
  CHECK(calls == 0);
  for (UINT i = 0; i < dxvk::SamplerCount; ++i) {
    CHECK(dxvk::TryD3D9SingleBinding(1u << i, bind));
    CHECK(calls == i + 1);
    CHECK(slot == i);
  }
  calls = 0;
  for (uint32_t mask : {3u, 0x10001u, (1u << dxvk::SamplerCount) - 1}) {
    CHECK(!dxvk::TryD3D9SingleBinding(mask, bind));
    CHECK(calls == 0);
  }
}

void testFirstWriteRanges() {
  // Independent per-register oracle, including mutations between captures.
  dxvk::bit::bitset<8192> captured;
  dxvk::ForEachD3D9UncapturedRange(captured, 8192u, 0u,
    [](uint32_t, uint32_t) { CHECK(false); });
  dxvk::ForEachD3D9UncapturedRange(captured, 8191u, 1u,
    [&](uint32_t start, uint32_t count) {
      CHECK(start == 8191 && count == 1); captured.set(start, true);
    });
  dxvk::ForEachD3D9UncapturedRange(captured, 8191u, 1u,
    [](uint32_t, uint32_t) { CHECK(false); });
  captured.clearAll();
  std::array<uint32_t, 8192> current = {}, saved = {}, expected = {};
  std::array<bool, 8192> seen = {};
  uint32_t random = 0x71e45u;
  for (uint32_t pass = 0; pass < 2000; pass++) {
    random = random * 1664525u + 1013904223u;
    const uint32_t start = pass < 5 ? 31u * pass : random % 8192;
    const uint32_t count = std::min(8192u - start, pass < 5 ? 8192u - start : (random >> 16) % 513u);
    for (uint32_t i = 0; i < 8192; i++) current[i] = pass * 8192 + i;
    for (uint32_t i = start; i < start + count; i++) {
      if (!seen[i]) { expected[i] = current[i]; seen[i] = true; }
    }
    dxvk::ForEachD3D9UncapturedRange(captured, start, count,
      [&](uint32_t first, uint32_t length) {
        CHECK(length && first >= start && first + length <= start + count);
        for (uint32_t i = first; i < first + length; i++) {
          CHECK(!captured.get(i));
          saved[i] = current[i]; captured.set(i, true);
        }
      });
    CHECK(saved == expected);
    for (uint32_t i = 0; i < 8192; i++) CHECK(captured.get(i) == seen[i]);
    if (pass % 11 == 0) { captured.clearAll(); seen.fill(false); saved.fill(0); expected.fill(0); }
  }
}

}

int main() {
  testInputAssemblyEquality();
  testMultisampleEquality();
  testBlendEquality();
  testSamplerEquality();
  testSingleBindingDispatch();
  testFirstWriteRanges();
  std::puts("single binding dispatch and first-write ranges: passed (Release checks enabled)");
  return 0;
}
