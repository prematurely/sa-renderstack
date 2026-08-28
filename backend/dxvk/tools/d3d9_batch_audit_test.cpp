#include "../src/d3d9/d3d9_gta_sa_compat.h"

#include <cassert>

int main() {
  dxvk::D3D9GtaSaBatchAudit audit = { };

  assert(audit.GetP95BatchSize() == 0u);

  audit.BatchCount = 20u;
  audit.BatchSizeHistogram[1u] = 18u;
  audit.BatchSizeHistogram[10u] = 2u;
  assert(audit.GetP95BatchSize() == 10u);

  audit = { };
  audit.BatchCount = 1u;
  audit.BatchSizeHistogram[dxvk::D3D9GtaSaBatchAudit::BatchHistogramMax] = 1u;
  assert(audit.GetP95BatchSize() == dxvk::D3D9GtaSaBatchAudit::BatchHistogramMax);

  return 0;
}
