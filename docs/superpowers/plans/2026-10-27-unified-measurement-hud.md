# Unified Measurement HUD Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans for integrated changes; independently scoped symbol and journal work may use superpowers:dispatching-parallel-agents. Steps use checkbox syntax for tracking.

**Goal:** One F8 press collects baseline, CPU hotspots, and former F7 journal/backend audit information with a visible HUD and precise, qualified attribution.

**Architecture:** Bridge owns the sequential controller and capture identity. A new private, versioned COM interface transfers bounded HUD snapshots and controls the existing backend state audit without altering public API1-7 or existing private vtables. Symbol lookup and journal aggregation are separate components; expensive work stays outside suspended-thread and per-frame rendering paths.

**Tech Stack:** C++23/MSVC x86 Bridge, C++17/LLVM-MinGW DXVK, existing Vulkan HUD renderer, Python/PowerShell build and validation tools.

**Spec:** docs/superpowers/specs/2026-10-27-unified-measurement-hud-design.md (user approved revised design and delegated technical details).

## Global Constraints

- Preserve quality, public API1-7 and DLL exports, existing Fading/state fixes, and all pre-existing dirty work.
- No game launch, game-directory deployment, commit, push, driver change, new streaming hook, or budget adjustment.
- F7/F8 are aliases in combined mode; held/repeated keys never restart an active capture.
- Defaults: 5000 ms baseline, configured CPU duration/interval (currently 10000/5 ms), configured attribution duration (currently 3000 ms); text refresh at most 5 Hz.
- Preserve standalone behavior with combined mode disabled. Missing optional backend support produces an explicit unavailable status.
- CPU samples, heuristic callers, inferred symbol boundaries, and inclusive QPC timings must retain their evidence labels.
- All timed A/B processes run serially after builds and other GPU tests stop.

## Review Focus

- Two same-basename d3d9 modules or stale symbols must never be merged or silently named from the wrong build.
- Reset/release/shutdown during CPU sampling must terminate the capture without accessing retired device data or reporting success.
- Backend F7 polling must not independently start an audit during baseline/CPU phases in combined mode.
- An attribution transaction crossing the end boundary must finish recording before summary publication, without deadlocking the owner thread.
- HUD disabled/unsupported and missing journal records must remain distinguishable from measured zero cost.

## Execution rulings

Work in the existing feature checkout because the approved state includes extensive uncommitted fixes; a fresh checkout at HEAD would omit them. Capture a baseline patch and file identities first. The user's explicit 'implement directly, details are yours' replaces further plan/method confirmation. No optional skill commit or cleanup step overrides the no-commit requirement or prior cleanup-policy rejection.

### Task 1: Pure controller and measurement algorithms

**Files:** create `src/bridge/legacy/BridgeMeasurement.h`, `src/bridge/legacy/api-projects/tests/measurement_tests.cpp`; modify API-project CMake configuration.

**Interfaces:** `BridgeMeasurement::Controller` consumes key edges, monotonic Present observations and sampler completion; produces phase/action snapshots. `FrameStats`, bounded frame/sample records, jitter and captured-frame-chain helpers operate on supplied data without Win32/global game state.

- [x] Write behavioral tests first: complete ordering, failures/interruption, held/both keys, clock/arithmetic boundaries, failed Presents, explicit missing stages, slow/ordinary intervals, sample capacity/jitter, validated-versus-heuristic frames.
- [x] Run and save RED output; implement the minimal production helpers and run GREEN.
- [x] Confirm bounded storage, no allocating operations in the captured-stack helper, and stable Present-based units.

### Task 2: Exact-build symbol preparation and resolution

**Files:** create `src/bridge/legacy/MeasurementSymbols.h/.cpp`, `tools/prepare-measurement-symbols.py`, relevant native/Python tests; build/package wiring owned by the parent.

**Interfaces:** immutable per-module lookup accepts a real binary path/base and a prepared identity-bound sidecar; result includes module path/base/RVA, symbol/offset, and resolution quality. Runtime interface details documented by implementer before parent integration.

- [x] RED tests for wrong identities, same basenames/different bases, alias/ambiguous ranges, section boundaries, no symbols, and malformed/truncated input.
- [x] Generate sidecars from actual linker/COFF metadata; preserve explicit size versus inferred-next-symbol distinctions.
- [x] GREEN tests on fixtures and the actual baseline DLLs; no symbol download or invented third-party function names.

### Task 3: Journal precision and inactive admission cost

**Files:** modify `EffectInspector.cpp`, `ProperShadersStateJournal.h`; add scoped tests/runner without editing shared build orchestration concurrently.

**Interfaces:** bounded `JournalAttributionSummary` exposes capture identity, frame/Present counts, technique/pass counts, inclusive transaction and restore timing, and dropped/unavailable indicators. Existing output remains parseable.

- [x] RED for absent summary/per-frame attribution and inactive lock-free admission; include StopCapture admission ordering.
- [x] Add inactive fast check retaining the locked active recheck, stable identity, bounded frame aggregation and named summary.
- [x] GREEN on real journal transactions and duplicate/changed state; keep transaction span and restore cost separate.

### Task 4: Private backend audit/HUD and unified Bridge integration

**Files:** create `backend/dxvk/src/d3d9/d3d9_bridge_diagnostics.h`; modify D3D9 device, compat audit, HUD, swapchain; modify BridgeD3D9.cpp and project files; add focused integration source if needed.

**Interfaces:** new IID `ID3D9BridgeDiagnostics` accepts combined-mode ownership, bounded HUD snapshots, and start/stop/read audit commands. Backend snapshots use copied data and independent synchronization; existing lifecycle IID/vtable is unchanged.

- [x] Add real-device RED tests for the missing QI interface, audit identity/counters, HUD snapshot validation, raw/wrapped lifecycle and reset/release.
- [x] Implement QI integration and reused backend state audit; suppress standalone F7 polling only while combined ownership is active.
- [x] Add throttled HUD item and test its actual renderer path, preserving normal draw pixels and D3D9 state.
- [x] Integrate the sequential controller at owner-thread Present boundaries, bounded CPU sample collection, jitter, phase/frame correlation, symbol lookup after resume, journal/backend control, and one combined summary identity.
- [x] Preserve old standalone logs and wrapped/raw fallback behavior; add explicit failure/interrupted outcomes.
- [x] Run focused GREEN tests and inspect generated reports/HUD artifacts.

### Task 5: Full verification and candidate delivery

**Files:** modify tools/build.ps1, tools/package.ps1, tools/test.ps1, candidate configuration/docs and new measurement regression/benchmark runners.

- [x] Wire all new required gates and sidecar production without changing installed files.
- [x] Repeated serial A/B: combined disabled versus ready/idle HUD; distinguish active CPU/journal measurement from baseline and report units/observer overhead honestly.
- [x] Run `pwsh -NoProfile -File tools/test.ps1 -BridgeReference 'D:/GTA San Andreas/Temp/SA-RenderStack-backups/pre-alpha.1-20260829-230407-3d998b6e/d3d9.dll'` and required native/Python tests; read every result.
- [x] Obtain a fresh scoped code review, resolve actionable issues, and rerun affected verification plus full pipeline after changes.
- [x] Verify installed file hashes/size/timestamps still match the input record; report actual test counts, benchmark limits, candidate paths, and remaining game visual acceptance.

## Progress

Execution ledger: `out/perf/20261027-unified-measurement/progress.md`.
