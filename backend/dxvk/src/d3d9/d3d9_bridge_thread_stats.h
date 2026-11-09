#pragma once
#include <d3d9.h>

// Private optional boundary query. Does not flush, synchronize, enable an
// audit, require GTA compatibility, or alter any public D3D9/API1-7 vtable.
inline constexpr GUID IID_D3D9BridgeThreadStats =
  {0xd0967703,0x3642,0x49d2,{0x93,0xa8,0x91,0xa2,0xd1,0xe0,0x31,0x50}};
struct D3D9BridgeThreadSnapshot {
  UINT StructSize,Version,MainThreadId,CsThreadId,AvailableMask,Reserved;
  // CPU is scheduled kernel+user execution, in 100ns units. Queue idle is
  // wall time spent inside the CS condition wait, including an open wait.
  // OrderedDispatched-OrderedCompleted includes in-flight work and excludes
  // the high-priority/injected stream. Counters are lifetime totals.
  UINT64 WallUs,MainCpu100ns,CsCpu100ns,CsQueueIdleUs,OrderedDispatched,OrderedCompleted;
  // Existing CS/GPU wait counters count COMPLETED spans (possibly crossing
  // a sampling boundary). Device-wide GPU waits are not necessarily main-
  // thread waits. GPU idle is an estimate, not a timestamp-query GPU time.
  UINT64 CsSyncCount,CsSyncUs,GpuSyncCount,GpuSyncUs,GpuIdleUs,Chunks,Submits;
};
struct ID3D9BridgeThreadStats : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetThreadStats(D3D9BridgeThreadSnapshot*) = 0;
};
