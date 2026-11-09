#pragma once

#include <d3d9.h>

// Private, copied-data diagnostics. Public API1-7 and lifecycle vtables do not
// change. Start/Stop/GetAudit run on the device's calling thread; Publish/GetHud
// additionally support concurrent copying from the HUD thread. No pointer in a
// snapshot refers to caller memory. Initialize StructSize and Version on reads.
inline constexpr GUID IID_D3D9BridgeDiagnostics =
  {0x36f6d3a1,0xc28c,0x4dd0,{0x91,0xaf,0x23,0x1e,0x8b,0xa0,0xb2,0x03}};
inline constexpr UINT D3D9_BRIDGE_DIAGNOSTICS_VERSION = 1;
inline constexpr UINT D3D9_BRIDGE_AUDIT_COUNTER_COUNT = 17;
inline constexpr UINT D3D9_BRIDGE_PREPARE_COUNTER_COUNT = 28;
inline constexpr UINT D3D9_BRIDGE_HUD_LINE_COUNT = 8;
inline constexpr UINT D3D9_BRIDGE_HUD_LINE_CAPACITY = 128;

enum D3D9BridgeAuditStatus : UINT {
  D3D9_BRIDGE_AUDIT_IDLE, D3D9_BRIDGE_AUDIT_ACTIVE,
  D3D9_BRIDGE_AUDIT_COMPLETE, D3D9_BRIDGE_AUDIT_INTERRUPTED,
  D3D9_BRIDGE_AUDIT_UNAVAILABLE,
};

struct D3D9BridgeAuditCounter {
  UINT64 Calls, RedundantCalls, Units, RedundantUnits, QpcTicks, MaxQpcTicks;
  // TimedCalls == 0 means no elapsed measurement was taken. A timed zero
  // remains distinguishable from an unsupported/untimed category. Spans are
  // device-locked API-body elapsed: lock acquisition wait is excluded. They
  // include waits inside that body and may nest (e.g. state-block restore).
  UINT64 TimedCalls;
};
struct D3D9BridgePrepareCounter { UINT64 Draws, Units; };

struct D3D9BridgeAuditSnapshot {
  UINT StructSize, Version;
  UINT64 CaptureId;
  UINT Status, Available, Active, DurationMs;
  UINT64 ElapsedMs, ObservedPresentBoundaries, QpcFrequency;
  D3D9BridgeAuditCounter Total;
  // Order matches D3D9GtaSaStateAuditKind; timings are inclusive measured
  // API spans, not additive frame costs. Present boundaries are attempts,
  // never a successful-Present or FPS numerator.
  D3D9BridgeAuditCounter Counters[D3D9_BRIDGE_AUDIT_COUNTER_COUNT];
  UINT64 SamplerBindingCacheHits, TextureBindingCacheHits;
  UINT64 ResourceBindingCacheInvalidations, StateBlockFastSkips;
  UINT64 PrepareDraws, PrepareCleanDraws, PrepareCategoryEvents;
  UINT64 PrepareMaxCategoriesPerDraw;
  D3D9BridgePrepareCounter Prepare[D3D9_BRIDGE_PREPARE_COUNTER_COUNT];
};

struct D3D9BridgeHudSnapshot {
  UINT StructSize, Version;
  UINT64 CaptureId;
  UINT Phase, Status, Visible, ProgressPermille;
  UINT64 ElapsedMs, DurationMs;
  UINT LineCount;
  char Lines[D3D9_BRIDGE_HUD_LINE_COUNT][D3D9_BRIDGE_HUD_LINE_CAPACITY];
};

struct ID3D9BridgeDiagnostics : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE SetCaptureOwner(BOOL) = 0;
  virtual HRESULT STDMETHODCALLTYPE StartAudit(UINT64 captureId, UINT durationMs) = 0;
  virtual HRESULT STDMETHODCALLTYPE StopAudit(UINT64 captureId) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetAudit(D3D9BridgeAuditSnapshot*) = 0;
  virtual HRESULT STDMETHODCALLTYPE PublishHud(const D3D9BridgeHudSnapshot*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetHud(D3D9BridgeHudSnapshot*) = 0;
};
