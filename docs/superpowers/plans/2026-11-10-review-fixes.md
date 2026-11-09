# PR review fixes implementation plan

> **For agentic workers:** Use superpowers:executing-plans task by task, with one fresh whole-branch reviewer before publication.

**Goal:** Fix PR #1 comments 4509025305, 4509025311 and 4509025313, retain sampler optimization, verify full HUD behavior, and publish a validated follow-up PR.

**Architecture:** Keep existing public interfaces and renderer paths. Correct lifecycle serialization, capture-device ownership ordering and Windows module-path encoding at their existing boundaries. Extend native probes that execute the production runtime/backend.

**Tech Stack:** C++ x86, D3D9/Vulkan, Win32, PowerShell, MSBuild, LLVM/MinGW, GitHub Actions.

**Spec:** The three linked comments on https://github.com/prematurely/sa-renderstack/pull/1 and the user's 2026-11-10 instruction in this chat.

## Global constraints

- Preserve existing sampler changes, rendering quality, public interfaces and Vulkan staging/copy behavior.
- Do not modify installed game DLLs, symbols or configuration; compare their hashes at completion.
- Demonstrate each review regression on old production code, then pass on the fix.
- Run full local verification, publish a PR, follow its workflow to completion, and reply to all three original review threads. Do not merge.

## Review focus

- Callback unregister/final release must retain synchronous lifetime guarantees.
- Reset must execute on its device-creation thread while another thread Presents.
- A secondary device must not claim owner-thread identity or advance the selected capture.
- Non-ASCII paths must survive actual module enumeration and load matching RSYM symbols.
- Full DXVK HUD and the eight-line measurement HUD must be checked using actual rendered pixels at practical resolutions.

### Task 1: Selected-device ownership

**Files:** `src/bridge/legacy/BridgeMeasurementRuntime.cpp`, `tests/measurement-runtime-tests.cpp`, `tests/measurement-runtime-test.ps1`.
**Interface:** Existing `RuntimePrepareDevice`, `RuntimePresent`, `RuntimeGetSnapshot` and real D3D9 devices.
- [x] Add a test that prepares A, Presents B on another thread first, and completes an F8 capture on A; B must not consume the key or own the capture.
- [x] Run the focused native runtime case against the unchanged runtime and retain its assertion failure.
- [x] Reject non-selected devices before owner initialization; rerun focused and existing runtime cases.

### Task 2: Unicode module enumeration

**Files:** Same runtime/probe files, with symbol fixture preparation in the runtime test script if required.
**Interface:** Real Toolhelp enumeration, `MeasurementSymbolResolver::LoadModule`, matching `.rsym` files.
- [x] Load a backend from a mixed non-ASCII directory with wide Win32 APIs and verify captured UTF-8 module paths and resolved symbols.
- [x] Record failure against the original enumerator.
- [x] Enumerate `MODULEENTRY32W` with `Module32FirstW`/`Module32NextW`, convert to UTF-8, and pass native runtime and symbol regressions.

### Task 3: Lifecycle lock ordering

**Files:** `backend/dxvk/src/d3d9/d3d9_device.cpp`, `tests/measurement-backend-probe.cpp`, `tests/measurement-backend-test.ps1`.
**Interface:** Actual lifecycle observer, device lock, diagnostics and Reset/Present calls.
- [x] Add coordinated concurrent Reset/Present regression, reentrant diagnostics, unregister and final-release checks with a bounded process watchdog.
- [x] Record the old DLL's deadlock/failure and then enforce a consistent lock order.
- [x] Pass native lifecycle/runtime regressions; compare repeated Present/control measurements against the preserved sampler candidate.

### Task 4: Full HUD and complete delivery

**Files:** Native HUD probe/script, `tools/test.ps1`, validation report; renderer changes only if actual rendering demonstrates overlap/clipping.
- [x] Render full DXVK HUD plus measurement text, capture pixels at 2560x1600 and a smaller practical viewport, and inspect layout.
- [ ] Run the complete local pipeline and sampler regressions, inspect failures and skip reasons, and obtain a fresh whole-branch review.
- [ ] Commit the preserved sampler work and review fixes with clear scope, publish the follow-up PR, follow workflow to completion and reply to each original comment with fix/test evidence.
- [ ] Verify installed file hashes remain unchanged and report HUD findings.
