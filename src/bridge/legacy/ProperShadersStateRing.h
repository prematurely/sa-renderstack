#pragma once

// ProperShaders state ring: fixed-size record ring for the render-state
// transitions observed by the ProperShaders guards. Migrated from FLA++
// (FLACompatBridge/BridgeCompatProperShaders.cpp) together with the guard that
// produces the records.

#include "ProperShadersRenderStateGuard.h"

// ---- record lifecycle (called from the guards) ----
bool IsProperShadersStateTraceCaptureActive() noexcept;
bool BeginProperShadersStateTraceCapture();
void InitializeProperShadersStateTraceRecord(
    ProperShadersStateTraceRecord* record,
    ProperShadersStateTraceKind kind);
void CaptureProperShadersStateTracePoint(
    ProperShadersStateTraceRecord* record,
    uint32_t snapshotIndex);
void StoreProperShadersStateTraceRecord(ProperShadersStateTraceRecord* record);

// F10 opens a one-second capture. Expiry/dump is polled at owner safe points.
void MaybeDumpProperShadersStateTraceRing();
void DumpProperShadersStateTraceRing();
