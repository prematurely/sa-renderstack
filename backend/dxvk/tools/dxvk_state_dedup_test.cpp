#include "../src/dxvk/dxvk_format.h"
#include "../src/dxvk/dxvk_util.h"
#include "../src/dxvk/dxvk_graphics_state.h"
#include "../src/d3d9/d3d9_state.h"

#include <cassert>

namespace {

void testInputAssemblyEquality() {
  const dxvk::DxvkIaInfo first(
    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE, 0u);
  const dxvk::DxvkIaInfo same(
    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE, 0u);
  const dxvk::DxvkIaInfo different(
    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, VK_FALSE, 0u);

  assert(first.eq(same));
  assert(!first.eq(different));
}

void testMultisampleEquality() {
  const dxvk::DxvkMsInfo first(
    VK_SAMPLE_COUNT_1_BIT, 0xffffu, VK_FALSE);
  const dxvk::DxvkMsInfo same(
    VK_SAMPLE_COUNT_1_BIT, 0xffffu, VK_FALSE);
  const dxvk::DxvkMsInfo different(
    VK_SAMPLE_COUNT_1_BIT, 0xff0fu, VK_FALSE);

  assert(first.eq(same));
  assert(!first.eq(different));
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

  assert(first.eq(same));
  assert(!first.eq(different));
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
  assert(first.eq(dxvk::D3D9SamplerInfo(sameState)));
  assert(!first.eq(dxvk::D3D9SamplerInfo(differentState)));

  auto borderState = firstState;
  borderState[D3DSAMP_ADDRESSU] = D3DTADDRESS_BORDER;
  borderState[D3DSAMP_BORDERCOLOR] = 0x12345678u;
  assert(!first.eq(dxvk::D3D9SamplerInfo(borderState)));
}

}

int main() {
  testInputAssemblyEquality();
  testMultisampleEquality();
  testBlendEquality();
  testSamplerEquality();
  return 0;
}
