# StateRing and device API cache Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal:** Remove idle full snapshots and redundant interface validation while preserving rendering and diagnostic availability.

**Architecture:** Existing StateRing owns bounded manual capture admission; guard owns per-thread method caches with explicit lifecycle invalidation. Bridge forwards device boundaries. No backend/API layout changes.

**Tech Stack:** x86 C++/Win32, existing MSVC/CMake, real D3D9/Vulkan fixtures and PowerShell tooling.

**Spec:** docs/superpowers/specs/2026-10-27-statering-cache-design.md

## Global Constraints

- Keep the existing dirty feature checkout and all installed files unchanged.
- No deployment, game launch, commit/push, streaming or picture-quality edits.
- Preserve Fading correction and public API1-7/exports; F7/F8 unchanged.
- Retain cold validation, SEH and COM lifetime handling; cache pointers only.
- Timed A/B runs serially after builds/GPU tests stop.

## Review Focus

- Idle snapshots must stop without suppressing alpha/stencil correction.
- Vtable replacement/in-place changes and invalid getters must not use stale pointers.
- Reset/final release must invalidate raw and wrapped paths, without holding refs.
- Late records, shutdown and held keys must not reopen an expired manual capture.
- Benchmark reports must distinguish native fixture timings from game FPS.

### Task 1: Preserve baseline and add real behavior RED

**Files:** out/perf/20261027-statering-cache/source-before; tests/statering-runtime-tests.cpp; tests/statering-runtime-test.ps1; native test project.

- [x] Preserve status/diff, old sources/DLL, installed identities.
- [x] Add actual native device forwarding-counter tests for idle snapshots, F10 window, warm queries, state/ref correctness and mutation/reset; observe RED.

### Task 2: Implement bounded capture and method caches

**Files:** ProperShadersStateRing.h/.cpp, ProperShadersRenderStateGuard.h/.cpp, BridgeD3D9.cpp.

**Interfaces:** IsProperShadersStateTraceCaptureActive controls guard admission; InvalidateProperShadersDeviceApiCache advances pointer-cache generation. Existing key polling/dump and state snapshot APIs remain.

- [x] Implement capture admission/expiry, late-record exclusion and lifecycle cleanup.
- [x] Implement pointer/vtable caches and wire raw/wrapped creation/reset/release.
- [x] Pass native/system RED cases and existing Fading/state/pixel regressions.

### Task 3: Repeated A/B and required wiring

**Files:** tests/statering-runtime-benchmark.ps1; tools/test.ps1; configuration comments and docs/unified-measurement.md.

- [x] Compare saved/current source with identical actual device fixture, alternating A/B, 12 processes and 4 rounds; include idle and detailed capture.
- [x] Wire required native/system regression gates and document F10 behavior.
- [x] Keep candidates separate from the previous deployment candidate.

### Task 4: Final verification and review

- [x] Build All, run tools/test.ps1 with archived Bridge reference; inspect counts/logs.
- [x] One fresh scoped whole-change review; fix actual findings and verify affected work.
- [x] Check installed identities, retain artifacts, report measurements/limits and paths.

## Execution rulings

Existing feature checkout is retained because it contains the user's validated uncommitted changes. User's direct request and prior delegation resolve method choices; no additional approval or optional integration/cleanup step is needed. Ledger: out/perf/20261027-statering-cache/progress.md.
