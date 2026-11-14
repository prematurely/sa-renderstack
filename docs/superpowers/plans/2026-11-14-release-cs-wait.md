# Mapping release and CS wait implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Remove the concentrated release regression while retaining mapping wins, and make the observed CS wait precisely attributable.

**Architecture:** Prototype bounded final-view retirement on the real OS boundary, then integrate only if full-lifetime timing improves. Add capture-only sequence and command-buffer spans without changing rendering order.

**Tech Stack:** C++/Win32, D3D9/DXVK/Vulkan, Python, PowerShell.

**Spec:** `docs/superpowers/specs/2026-11-14-release-cs-wait-design.md`

## Global Constraints

- Preserve Vulkan staging/copy, quality, public interfaces and configured memory budget.
- Keep all cleanup inside measured lifetime; inactive telemetry stays low overhead.
- No game deployment, game capture, commit or push.

## Review Focus

- Shared mapped page still referenced by another live texture.
- Recycled file range or chunk while an old view is pending.
- Cleanup worker startup failure and shutdown with work queued.
- Pending mapped bytes under budget pressure and capture termination.
- CS high-priority work, ordered sequences, driver spans and truncated captures.

### Task 1: Explain and prototype final release

**Files:** `tests/mapping-budget-probe.cpp`; task evidence under `out/perf/20261114-release-cs-wait`.
**Interfaces:** real `UnmapViewOfFile` import; separate untimed residency pass; `releaseNs` includes drain.

- [x] Record old/current resident-page counts and existing release timing.
- [x] Add opt-in test-only bounded worker and compare same DLL with hook off/on.
- [x] Record full release, total, main cycles and true OS completion; reject if no repeatable gain.

### Task 2: Implement retained mechanism

**Files:** `backend/dxvk/src/util/util_unmap.{h,cpp}`, D3D9 mapping budget callers and focused tests as needed.
**Interfaces:** final region release distinct from synchronous eviction; pending bytes remain counted; pool destruction drains.

- [x] Add real behavior checks for retained views, budget, shared pages, reuse, failure and teardown; run against baseline.
- [x] Implement only the mechanism supported by Task 1, recording any evidence-based plan revision.
- [x] Run focused pixels, concurrency, reset, failure and lifetime regressions.

### Task 3: CS wait attribution

**Files:** telemetry protocol/recorder, `dxvk_cs.cpp`, `dxvk_cmdlist.cpp`, bridge report decoder and real telemetry probe.
**Interfaces:** existing CS_WAIT identity is target sequence; capture-only chunk and driver spans preserve temporal nesting.

- [x] Add real-device wait correlation test; demonstrate missing old evidence.
- [x] Instrument precise chunk/driver boundaries and extend report/test decoder.
- [x] Verify actual sequence correlation, tail handling and inactive/active overhead.

### Task 4: Acceptance and packaging

- [x] Freeze baseline/current/final binaries; alternating A/B and A/A across mapping and LockRect workloads.
- [x] Run complete local pipeline once focused gates pass; repair any introduced failure.
- [x] One independent whole-change review; resolve findings and rerun affected gates.
- [x] Write evidence report, package local candidate, verify game files remain unchanged.
