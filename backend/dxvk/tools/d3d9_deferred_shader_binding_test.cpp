#include "../src/d3d9/d3d9_gta_sa_deferred_binding.h"

#include <cassert>

namespace {

using Decision = dxvk::D3D9GtaSaDeferredBindingDecision;

void testBasicTracking() {
  dxvk::D3D9GtaSaDeferredBindingTracker<int> tracker;

  assert(tracker.resolve(1) == Decision::None);

  tracker.markDirty();
  assert(tracker.resolve(1) == Decision::Bind);

  tracker.markDirty();
  tracker.markDirty();
  assert(tracker.resolve(1) == Decision::Coalesced);

  tracker.markDirty();
  assert(tracker.resolve(2) == Decision::Bind);

  tracker.invalidate();
  assert(tracker.resolve(2) == Decision::Bind);
  assert(tracker.resolve(2) == Decision::None);
}

void testRestoreReapplyWithoutDrawCoalesces() {
  dxvk::D3D9GtaSaDeferredBindingTracker<int> tracker;

  tracker.invalidate();
  assert(tracker.resolve(10) == Decision::Bind);

  // A state block restores another shader and ProperShaders reapplies the
  // original one before any draw. Only the final shader reaches the backend.
  tracker.markDirty();
  tracker.markDirty();
  assert(tracker.resolve(10) == Decision::Coalesced);
}

void testDrawBoundaryPreservesRealShaderChanges() {
  dxvk::D3D9GtaSaDeferredBindingTracker<int> tracker;

  tracker.invalidate();
  assert(tracker.resolve(10) == Decision::Bind);

  tracker.markDirty();
  assert(tracker.resolve(20) == Decision::Bind);

  tracker.markDirty();
  assert(tracker.resolve(10) == Decision::Bind);
}

void testBackendInvalidationForcesRebind() {
  dxvk::D3D9GtaSaDeferredBindingTracker<int> tracker;

  tracker.invalidate();
  assert(tracker.resolve(10) == Decision::Bind);

  // ProcessVertices and device Reset can invalidate the backend binding while
  // leaving the logical D3D9 shader unchanged.
  tracker.invalidate();
  assert(tracker.resolve(10) == Decision::Bind);
  assert(tracker.resolve(10) == Decision::None);
}

}

int main() {
  testBasicTracking();
  testRestoreReapplyWithoutDrawCoalesces();
  testDrawBoundaryPreservesRealShaderChanges();
  testBackendInvalidationForcesRebind();

  return 0;
}
