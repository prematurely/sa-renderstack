# F8 Tail and Mapping Implementation Plan

> For agentic workers: use superpowers:subagent-driven-development task by task.
> No commits, pushes, deployment, or game runs.

**Goal:** Preserve F8 tail evidence and reduce unnecessary main-thread texture mapping eviction.

**Architecture:** Bounded asynchronous temporary event storage; streaming report reads;
strict-budget LRU reclamation preserving existing upload scopes and Vulkan copies.

**Tech Stack:** Existing x86 C++ Windows/D3D9/Vulkan, MSVC Bridge, LLVM-MinGW DXVK,
native fixtures and PowerShell/Python verification.

**Spec:** `docs/superpowers/specs/2026-11-14-f8-tail-mapping-design.md`.

## Global Constraints

- No game deployment, game run, commit or push.
- Preserve Vulkan staging/copy, rendering quality, public API1–7 and existing COM vtables.
- Respect explicit d3d9.textureMemory budget; no MOD-name dependency or new external runtime dependency.
- Preserve previous uncommitted telemetry work and compare this batch against initial-source.
- Run real behavior regressions, repeated fixed-binary A/B and A/A, and full local validation.
- All eight installed DLL/config/symbol artifacts must remain hash-identical to game-before.json.

## Review Focus

Late writers across capture restart and spool failure must not mix ownership.
No render-thread file I/O/wait or locks held over I/O; all temporary storage bounded.
Pinned/locked or oversized textures must preserve data and respect lifetime rules.
Mapped budget boundaries and failed allocation must not cause an endless retry loop.
Benchmarks must measure complete upload/Present work, not merely move work beyond the timer.

### Task 1: F8 complete event stream

Files: `src/bridge/legacy/BridgeMeasurementRuntime.cpp/.h`, narrowly required new
private storage helper, `tests/measurement-runtime-tests.cpp` and runtime fixture build files.
Interface: consume unchanged copied-data ID3D9BridgeTelemetry; keep existing event
records and frame association; add bounded storage status/counters.

- [x] Preserve RED on old runtime: overflow history and miss a known late event.
- [x] Implement bounded background storage and streaming report iteration per spec.
- [x] Add real native high-volume capture, tail/frame association and count checks.
- [x] Cover open/write/read/queue pressure, cancellation/restart, worker and temp-file cleanup.
- [x] Run normal/old/system backend, fault and native lifecycle regressions; task review.

### Task 2: Budget-respecting mapping reuse and smaller trims

Files: narrowly related `backend/dxvk/src/d3d9/d3d9_device.cpp/.h` or memory helpers;
new real D3D9 mapping-budget probe/script and fixture build wiring as required.
Interface: existing configuration and D3D9 methods remain unchanged; native probes
observe OS operations through the existing private telemetry interface.

- [x] Read mapping design review and test budget/reuse/locked/oversized behavior against baseline.
- [x] Preserve a failing real reuse/eviction regression before changing the policy.
- [x] Implement minimal reclamation without the fixed 75% low-water over-eviction.
- [x] Retain initialized mappings within the same LRU budget and remove allocating
      LRU touches, preserving exception safety and initializer/device lock ordering.
- [x] Verify actual map/unmap counts, bounded mapped bytes, uploaded/readback pixels,
      dirty mips/faces, locked/upload-pinned sources, multi-device and Reset/release.
- [x] Verify initialization/first-lock reuse, create-only pollution and allocation
      failures; freeze reclamation-only and combined builds for attribution.
- [x] Independently review the policy and real behavior tests.

### Task 3: Performance, integration and complete validation

Root owns independent orchestration scripts, pipeline wiring, docs and packaging.
Consume final frozen Task 1/2 binaries and existing baseline; do not rebuild during benchmarks.

- [x] Freeze version comparison fixtures; record both baseline and candidate source/binary hashes.
- [x] Run repeated F8 version A/B and A/A, normal and high-volume; measure inactive controls and completion costs.
- [x] Run repeated mapping workloads, including below-budget, burst reuse, cold and over-budget controls;
      compare total/burst/Present latency and memory/OS map statistics.
- [x] Resolve actual regressions, run full pipeline, task/whole-change review and affected reruns.
- [x] Document observed benefits and limits; freeze local candidate with symbols and source manifest.
- [x] Verify installed game files unchanged and retain complete evidence; no deployment.
