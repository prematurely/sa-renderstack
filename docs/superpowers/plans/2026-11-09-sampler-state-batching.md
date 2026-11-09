# Sampler State Batching Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan in this session. Preserve the evidence directory across compaction.

**Goal:** Reduce generic sampler submission CPU work with unchanged observable D3D9 behavior.

**Architecture:** A small equality path reads authoritative state under the existing device lock. Recording, attribution and actual changes keep their existing semantics. Prevalidated sampler batches and StateBlock application enter the same implementation under their existing outer lock.

**Tech Stack:** C++ DXVK x86, native D3D9/Vulkan probe, PowerShell validation and Python evidence analysis.

**Spec:** `docs/superpowers/specs/2026-11-09-sampler-state-batching-design.md`

## Global Constraints

- No deployment, push or merge in this iteration; leave the final patch reviewable locally.
- Preserve public interfaces, quality, Vulkan staging/copy/upload and existing configuration.
- No MOD-specific predicates or new runtime dependencies.
- Evidence: `out/perf/20261109-sampler-batch`; baseline source is main `659f9ed60084f16dba98bb4cd396edeca2ab17cc`.

## Review Focus

- Same-value setters while recording still record and do not mutate live state.
- Batch validation is complete before any mutation; duplicate entries retain order.
- Journal first-write capture and Reset cannot reuse stale comparison state.
- Audit-on calls retain counts; ordinary fast paths never race on protected devices.
- Pixel filtering, border color and sRGB remain correct after setters, batches and restores.

### Task 1: Native characterization and benchmark

Files: create `tests/sampler-batch-probe.cpp`, `tests/sampler-batch-test.ps1`, `tests/sampler-batch-benchmark.ps1`.

- [x] Freeze a same-toolchain baseline and installed-game hashes.
- [x] Exercise real public setters, existing API5 effect batches, StateBlocks, Journal, diagnostic snapshots, Reset, pixels and concurrent protected-device snapshots.
- [x] Run baseline behavior checks: expect zero failures. Add no implementation-mirroring timing assertions.
- [x] Record interleaved repeated setter/batch/draw timings, wall time and thread cycles, with validated output and A/A support.

### Task 2: Sampler fast path and locked batches

Files: `backend/dxvk/src/d3d9/d3d9_device.cpp`, `d3d9_device.h`, `d3d9_stateblock.h`.

- [x] Split the unchanged non-recording/non-audited route from the existing setter body.
- [x] Reuse the internal locked route from validated effect batches and StateBlock Apply; retain outer locks, validation and order.
- [x] Build an unsafe same-value recording shortcut as a temporary negative control; expect the new recording test to fail, then restore the correct implementation.
- [x] Run the final candidate's native regressions in protected/unprotected and compatibility-off modes: expect zero failures.
- [x] Run repeated A/B and A/A with changed-state and draw controls; resolve supported regressions before accepting the optimization.

### Task 3: Integration and final evidence

Files: `tools/test.ps1`, `docs/development/performance-validation.md`, a local validation report.

- [x] Add native gates with explicit existing GPU-skip semantics; run full `tools/test.ps1` with GPU probes and the established Bridge reference.
- [x] Review the full patch with a fresh reviewer, address material findings and rerun affected/required checks.
- [x] Stage candidate DLL/symbols and a local dirty-tree manifest only under out; verify exports/dependencies and game-directory hashes. Formal release packaging remains gated on a clean committed tree.
- [x] Save actual results, benchmark controls and limitations; leave changes uncommitted for review.
