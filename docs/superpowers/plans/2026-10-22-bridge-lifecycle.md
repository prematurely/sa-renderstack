# Bridge Lifecycle Implementation Plan

> **For agentic workers:** Use systematic-debugging and test-driven-development. Parent integrates lifecycle; independent IAT/config tasks use dispatching-parallel-agents, then one fresh whole-patch review.

**Goal:** Complete the audited second batch with behavior regressions before changing installed runtime files.

**Architecture:** Keep raw DXVK devices unwrapped through a private COM lifecycle observer. Bridge receives safe owner-thread boundaries, releases borrowed host bindings before final device release, and exposes explicit shutdown on its factory through private COM. Prepare and publish patches at these boundaries instead of suspending threads.

**Tech Stack:** Windows x86, MSVC/LLVM-MinGW, CMake/CTest, D3D9/Vulkan, PowerShell.

**Spec:** User request 2026-10-22 and original audit second batch: IAT publication, patch safety/rollback, unwrapped lifetime/reclaim, stoppable workers/shutdown, configuration ownership. First-batch evidence remains a prerequisite and regression gate.

## Global Constraints

- Do not replace installed game DLLs or current configs; record hashes before/after.
- Preserve first-batch and original guard changes. No commits, pushes or deployment.
- Keep existing API1-7 ABI and exact DLL export set unchanged; new lifecycle COM interfaces are private integration contracts.
- Each fix needs expected RED behavior before GREEN. Real devices/process calls are required; fault injection may supplement otherwise inaccessible OS failure paths.
- No next-batch production work before focused tests, full tools/test.ps1, independent review and hash checks pass.

## Review Focus

- A caller entering immediately after IAT CAS always has a valid original; failed restoration preserves retryable ownership.
- Device/compat references do not keep an unwrapped device alive; a second device can take over after final release.
- Failed Reset leaves passes suspended; successful Reset resumes them, with paired plugin events.
- Shutdown rejects live devices, joins workers outside loader lock, disables callbacks and reports failed patch restoration.
- Disabled process policy preserves actual affinity/priority; DirectConstants production quarantine cannot be bypassed by INI precedence.

## Task 1: IAT publication and rollback

- [x] Add real allocated-IAT call/concurrency and protection-failure regressions; observe RED.
- [x] Publish callable original before slot CAS, serialize ownership, retain original and retryable failed restoration state; observe GREEN.

## Task 2: Device ownership, safe points and shutdown

**Files:** backend d3d9_device.cpp/.h and new private lifecycle header; BridgeD3D9.cpp; guard .cpp/.h; new runtime regression; CMake/test orchestration.

- [x] Add private COM contract and real-device observer/Bridge lifecycle tests; run against first-batch DLLs and record RED.
- [x] Implement observer notifications for Present, Reset/ResetEx and final public Release, with serialized unregister.
- [x] Attach borrowed unwrapped hosts, pair plugin callbacks, release claims and allow subsequent devices; fallback to existing wrapper for backends lacking the observer.
- [x] Move runtime initialization out of DllMain; pin initialized Bridge and add explicit shutdown through private factory COM, refusing live devices.
- [x] Replace guard watcher and suspended-thread patch installation with owner-thread create/Present safe points. Record original/installed bytes and restore conditionally; keep failure state.
- [x] Make support/affinity/diagnostic workers stoppable and join before plugin shutdown; run real repeated create/release/reset/shutdown and executable-patch tests.

## Task 3: Configuration ownership

- [x] Add actual factory-loading child-process affinity/priority regressions and effective config tests; observe RED.
- [x] Remove backend-wide process policy overrides, enforce production DirectConstants gate and update repository config documentation; observe GREEN.

## Task 4: Batch gate

- [x] Integrate focused tests into normal orchestration, keeping explicit hosted GPU skips.
- [x] Rebuild All and run full tools/test.ps1 with the verified archived BridgeReference; report all failures/skips.
- [x] Fresh-context review; address blocking findings and rerun relevant checks.
- [x] Verify installed file hashes and diff hygiene; save evidence/report before ending this batch.
