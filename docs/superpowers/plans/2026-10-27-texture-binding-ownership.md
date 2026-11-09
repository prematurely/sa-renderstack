# Texture Binding Ownership Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove unnecessary ownership changes before texture-cache decisions, retaining a candidate only after behavioral and repeated performance validation.

**Architecture:** Borrow the existing sample-view Rc during the cache decision in BindTexture and BindTextureBatch; take ownership only for a real CS command. Preserve cache ownership and all invalidation logic. Keep changes in the existing working tree, which contains the earlier required state-correctness fixes; do not reset or commit those changes.

**Tech Stack:** C++ / D3D9 / Vulkan, x86 LLVM-MinGW Release, MSVC native runtime probes, Python linked-code inspection, PowerShell orchestration.

**Spec:** `out/reports/nvidia-dll-assessment-20261027.md`, approved by the user on 2026-10-27: "按短设计，先优化纹理绑定（推荐）".

## Global Constraints

- Preserve image quality, public ABI and queued-resource ownership.
- Do not modify the game installation, launch GTA, tune driver settings, change streaming budgets, commit or push.
- Limit production changes to BindTexture and BindTextureBatch in d3d9_device.cpp.
- Keep baseline/candidate DLLs and benchmark evidence under out; serialize timed GPU benchmarks and compilation.
- Use the existing inline execution workflow; no further approval is needed for the already approved implementation and validation steps.

## Review Focus

- Cache-hit and cache-miss paths must preserve actual sampled pixels.
- Single-slot and two-slot deferred commands must own views after application textures are released.
- sRGB view changes must not reuse stale linear views.
- Stateblock/journal restoration and Reset must preserve required invalidation.
- Constant controls and miss-heavy draw workloads must not gain a reproducible regression through changed x86 code layout.

## Task 1: Texture ownership timing and acceptance

**Files:**
- Modify: `backend/dxvk/src/d3d9/d3d9_device.cpp`, BindTexture and BindTextureBatch only.
- Modify: `src/bridge/legacy/api-projects/tests/constant_benchmark.cpp`, add isolated `lifetime`/binding measurement modes without changing the existing pair protocol.
- Create: `tests/texture-binding-ownership-test.py`, inspect actual linked x86 cache-hit control flow, not source text.
- Modify: `tools/test.ps1`, required linked-code and real-device lifecycle gates.
- Update: `docs/development/performance-validation.md`, final result and limits.

**Interfaces:**
- Consume existing `const Rc<DxvkImageView>& D3D9CommonTexture::GetSampleView(bool)`.
- Preserve `PrepareGtaSaTextureBinding(DWORD, const Rc<DxvkImageView>&)` and all public interfaces.
- Add a probe command `<dll> on|off lifetime`, separate from existing `check`/`bench` and `pair` modes.

**Steps:**

- [x] Archive same-toolchain baseline and calibrate existing paired A/A.
- [x] Add linked-code regression that fails on the existing cache-hit retain/release sequence; save RED evidence. Its counter anchor is derived from PrepareGtaSaTextureBinding, not a fixed source offset.
- [x] Add real-device regressions for restore-to-same-view, single/multi-slot view switches, linear/sRGB, queued resource release and Reset; run on baseline for characterization. Behavioral tests preserve correctness; the linked-code RED pins the performance defect.
- [x] Borrow sample-view Rc before cache lookup; copy it only into a real command/pending batch. Build candidate and rerun focused checks.
- [x] Run true cache-hit/miss draw measurements and the existing 78-case control matrix in alternating A/B, 12 processes x 4 rounds, on/off. Compare to A/A; do not time during compilation.
- [x] Reject a candidate with no supported benefit or a reproducible control regression, restoring only this batch's production delta.
- [x] Run complete `tools/test.ps1 -BridgeReference <archived-audited-reference>` and inspect every result, including skips.
- [x] Obtain scoped independent read-only code review, address actionable findings and report retained/rejected candidate honestly.

## Completion evidence

Use fresh command exit codes, DLL/probe hashes, raw A/B samples, behavior check counts and the complete test report. Verify protected installed-file hashes against the batch input record. Distinguish native microbenchmark benefit from unmeasured GTA FPS.

**Final outcome:** all runtime optimization variants rejected. Production source restored to the exact input SHA256; retained native423-check lifetime/mixed-hit regressions, texture workload/protocol and six required GPU gates. The unretained optimization's linked-code cost guard is archived with the experimental evidence, not required of stable source. Final restored state:61passed/0failed/2optional external skips. See `out/reports/nvidia-texture-validation-20261027.md`.
