# State Correctness Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans with test-driven-development. Steps use checkbox tracking; preserve all existing user changes.

**Goal:** Repair the first state-correctness batch and prove it with real behavior before admitting the next batch.

**Architecture:** Keep state authority in DXVK. Capture supported implicit state mutations before changing them, maintain state serials, and repair the ownership/write-order contracts of the SDK wrappers. Tests load an explicitly named test backend and use always-on checks in Release.

**Tech Stack:** Windows x86, C++23, MSVC, LLVM-MinGW, Meson, CMake/CTest, PowerShell.

**Spec:** User instruction of 2026-10-21: fix the first state-correctness batch, add real behavioral regressions, verify before continuing to the next batch, and do not replace game-directory DLLs. The preceding audit defines this batch as journal bool/UP/light-serial correctness, API3 write order, API7 ownership/cleanup, and explicit batch failure semantics.

## Global Constraints

- Never replace, copy over, or deploy any game-directory DLL or active configuration.
- Preserve the existing ProperShadersRenderStateGuard.cpp/.h changes byte for byte during this batch.
- No commits, pushes, merges, release packages, or runtime profile tuning.
- Keep the public backend API7 ABI unchanged.
- Use real D3D9/Vulkan devices for journal, resource binding, Reset, and batch behavior; mocks are only supplementary ownership/failure controls.
- Every behavior fix needs an observed failing regression before its implementation.
- A batch passes only after focused behavior tests, applicable full project tests, independent review, and protected-file hash checks.

## Review Focus

- Bool bitfield updates must capture masked old values without affecting uncaptured registers.
- UP draws must retain their ordinary D3D9 null-binding side effects when capture is disabled and restore prior resources when capture is enabled.
- Journal restore must invalidate fast-skip even when an undefined light disappears.
- SDK wrappers must not duplicate COM ownership or leave an active journal after same-thread destruction/move assignment.
- Overlapping constant updates must preserve the last caller write, including identical starts and reverse-address order.

## Task 1: SDK state builders and selective journal

**Files:** api-projects/api3-state-batch and api7-selective-journal headers/sources/tests; API aggregate test support.

- [x] Replace disabled assert-only checks with always-on behavioral assertions and correct the r255 boundary.
- [x] Write API3 reversed/identical-start overlap, adjacency, invalid-input-preserves-state and reuse tests; observe RED.
- [x] Implement last-write-wins range construction with strong failure safety; observe GREEN.
- [x] Write API7 unique ownership, move/destruction, same-thread scope cleanup and real-device restore/Reset tests; observe RED.
- [x] Implement move-only ownership and deterministic same-thread cleanup without changing the backend ABI; observe GREEN.

## Task 2: DXVK supported implicit mutations

**Files:** backend/dxvk/src/d3d9/d3d9_device.cpp and associated journal/stateblock internals only as needed; a focused runtime regression executable in the public SDK CMake aggregate.

- [x] Add a real-device regression executable covering ordinary stateblock bool apply within full/selective journals, UP stream/index restoration, capture-disabled behavior, undefined-light fast-skip, and RT viewport/scissor effects.
- [x] Run against the unchanged backend and record expected failures.
- [x] Add capture hooks and serial notification before the actual mutations; rebuild the test backend and run both reference and optimized profiles.
- [x] Pin Reset discard, repeated/nested capture, and valid-then-late-failing batch recovery semantics with real device assertions.

## Task 3: Verification wiring and batch gate

**Files:** tools/build.ps1, tools/test.ps1 and focused orchestration tests as needed; API aggregate CMake and runtime tests.

- [x] Register new runtime checks and the API Release behavior suite in normal build/test orchestration, preserving explicit GPU skip policy.
- [x] Run new test executable, seven API unit tests and real API7 integration cases on the newly built backend.
- [x] Run the project's complete tools/test.ps1 command and report every failure/skip.
- [x] Request a fresh-context review of the first-batch patch and resolve important findings with RED/GREEN tests.
- [x] Verify protected-file hashes and diff hygiene; only then admit the second batch.

## Execution Rulings

- Use a dedicated codex branch in the existing source checkout and separate build/test outputs. This preserves the pre-existing dirty guard changes and avoids an unnecessary second source import; all runtime installation files are protected by recorded SHA-256 hashes.
- User has already authorized implementation and sequential verification, so proceed without another plan-approval pause.
- The first-batch gate is mandatory; no second-batch production changes before it passes.
