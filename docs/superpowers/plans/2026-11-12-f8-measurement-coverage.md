# F8 measurement coverage implementation plan

> For agentic workers: use superpowers:subagent-driven-development for independent backend/runtime work and fresh review. Do not commit, push, or deploy.

**Goal:** Fill the measured F8 coverage gaps without changing rendering, public interfaces or ordinary-frame behavior.

**Architecture:** Device-owned bounded telemetry; optional copied-data COM bridge; lower-rate CS sampling; timestamped context and allocator snapshots; offline frame correlation.

**Tech Stack:** Existing x86 C++/Windows/D3D9/Vulkan, MSVC Bridge and LLVM-MinGW DXVK, PowerShell/Python fixtures.

**Spec:** `docs/superpowers/specs/2026-11-12-f8-measurement-coverage-design.md`.

## Global constraints

No game DLL/config/symbol changes, no public API1–7 or existing vtable changes, no Vulkan staging/copy removal, no bottom-left HUD layout changes, no MOD-name dependency or new external runtime dependencies. Instrumentation is inactive outside F8. Source changes stay on `codex/f8-measurement-coverage-20261112`.

## Review focus

Late writers across capture restart; lock ordering during CS suspension/Reset/destruction; snapshots or events exceeding bounded capacity; events crossing frame/phase boundaries; mistaking present reuse/background for a proven pixel freeze. Tests must cover each.

## Task 1: Backend telemetry and actual-operation regression

- [x] Define and document private structs/interface in `backend/dxvk/src/d3d9/d3d9_bridge_telemetry.h`; agree boundary with runtime implementer.
- [x] Write native behavior tests that fail against immutable main DLL (`out/deploy/20261112-main-0d5680c09/runtime/backend/dxvk-gta/d3d9.dll`). Preserve red output.
- [x] Implement recorder, allocator/file mapping and CS/GPU wait instrumentation in backend files; expose bounded read/snapshot via optional QueryInterface.
- [x] Build and run real device tests including lifetime, overflow/restart, mapping, synchronization and pixels. Review backend changes independently.

## Task 2: F8 runtime, CS stacks, context and HUD

- [x] Add failing real runtime tests for CPU-running HUD, separate CS sample data, captured events/memory and context markers.
- [x] Implement in `src/bridge/legacy/BridgeMeasurementRuntime.cpp/.h` and narrowly related measurement files; preserve old report records, add labelled records.
- [x] Join spans/samples/snapshots to frame intervals, expose partial/unavailable/filtered/dropped data and overhead.
- [x] Run runtime native lifecycle/report regression and HUD pixel checks; independently review.

## Task 3: Integration, measurement overhead and pipeline

- [x] Wire new real-device gates into `tools/test.ps1` and document `docs/unified-measurement.md`.
- [x] Repeat paired A/B and A/A with fixed binaries, randomized/alternating order, independent processes; compare both inactive and active diagnostic cases.
- [x] Resolve meaningful regressions, run complete local validation with durable logs, and validate symbol/export/import/overlay consistency.
- [x] Run whole-change review, fix findings and rerun affected checks only.
- [x] Save evidence and candidate hashes; verify installed files equal `out/perf/20261112-measurement-coverage/game-before.json`. No deployment.

Final verification: 88 passed, 0 failed, 2 optional external-input audits skipped.
Final backend benchmark: 36 serial processes. Final runtime benchmark: 96 captures
across 12 processes, with active A/B and A/A controls. Evidence and the validation
report are retained in `out/perf/20261112-measurement-coverage` and
`out/reports/20261112-f8-measurement-coverage-validation.md`.
Candidate: `out/candidates/20261112-f8-measurement-coverage`; both `.rsym`
identities match the copied DLLs. All eight protected game artifacts are unchanged.
