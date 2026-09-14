#pragma once

// ProperShaders state ring: fixed-size record ring for the render-state
// transitions observed by the ProperShaders guards. Migrated from FLA++
// (FLACompatBridge/BridgeCompatProperShaders.cpp) together with the guard that
// produces the records.

#include "ProperShadersRenderStateGuard.h"

// ---- record lifecycle (called from the guards) ----
void InitializeProperShadersStateTraceRecord(
    ProperShadersStateTraceRecord* record,
    ProperShadersStateTraceKind kind);
void CaptureProperShadersStateTracePoint(
    ProperShadersStateTraceRecord* record,
    uint32_t snapshotIndex);
void StoreProperShadersStateTraceRecord(ProperShadersStateTraceRecord* record);

// ---- manual dump (hotkey edge) ----
void MaybeDumpProperShadersStateTraceRing();
void DumpProperShadersStateTraceRing();
