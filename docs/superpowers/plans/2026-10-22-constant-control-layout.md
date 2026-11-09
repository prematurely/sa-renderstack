# Constant control layout implementation plan

> **For agentic workers:** Use superpowers:executing-plans inline. No commit or push is authorized.

**Goal:** Repair the three confirmed constant control regressions without changing rendering, public interfaces, or state semantics.

**Architecture:** Controlled identical-machine-code relocation on one real device establishes instruction placement as a cause. Isolated loop results do not establish a single microarchitectural mechanism. Keep constant algorithms intact; constrain the measured early comparison and VS-int update path and reject any candidate that moves regressions elsewhere.

**Tech Stack:** x86 LLVM-MinGW 22.1.7, MSVC real-device probes, D3D9/Vulkan, PowerShell paired measurements.

**Spec:** User request in this chat, plus docs/development/performance-validation.md.

## Global constraints

- Preserve prior dirty changes, image quality, COM/SDK interfaces and runtime configuration.
- Root cause before tuning. No raw assembly patches or machine-wide scheduling changes.
- Real public COM behavior, repeated paired A/B and full validation must pass before installation.
- User authorized later backup, replacement and testing; announce that step and record backup hashes.

## Review focus

- Dedup off must retain short and long unchanged performance as well as changed writes.
- Every 32-bit lane, special bit pattern, unaligned input and bank boundary must remain bitwise correct.
- Journal and state-block recording must preserve their existing dedup-dependent behavior.
- Validate actual linked code; a source attribute alone is not performance evidence.
- Separate synthetic setter improvement from an unmeasured game FPS claim.

## Task 1: Diagnose and constrain layout

- [x] Preserve source and installed file snapshots in out/perf/20261022-controls-rootcause.
- [x] Reproduce the layout sensitivity with identical full functions on the same real device and distinguish isolated-loop evidence.
- [x] Add linked-code guard and observe baseline red; add real COM all-lane/boundary regression and run on baseline.
- [x] Apply the smallest layout-only candidate in d3d9_state.h. If the VS-int while alignment hint is lost, inspect generated IR before choosing another formulation.
- [x] Check linked layout, real COM and pixel behavior, then screen all 78 cases on/off.
- [x] Reject every candidate with repeatable material control regression; retain full rejected evidence.

## Task 2: Verify and deliver

- [x] Run repeated 12-process x 4-round paired A/B, on/off all cases, plus CPU0 control diagnostics and A/A calibration.
- [x] Run tools/test.ps1 with the archived Bridge reference and inspect every gate.
- [x] Obtain an independent read-only review; address actionable issues and reverify impacted checks.
- [ ] Publish report, exact DLL/probe hashes and performance limits. Only then back up and replace the active validated DLL and run feasible game/runtime checks under user authorization.

## Investigation rulings

- Branch-weight hints were rejected: the first improved the targets but slowed changed VS-int writes; two others did not retain the needed update-loop alignment.
- Moving the first store outside the scan did not preserve the metadata and is reverted.
- Ruling: allow an assembler alignment directive in the source, guarded to Clang i386 device-state VS int only. This is a toolchain layout workaround, not a binary/runtime patch; generated NOPs and implicit compiler asm constraints must be reviewed and measured. It replaces reliance on discarded while metadata, preserves scan/copy code, and excludes stateblock recording state.
- The preheader 64-byte candidate passed real behavior and improved the three controls, but its 4-register all-change screen warrants reducing preheader padding before formal repeats. Retain all diagnostic and rejected artifacts.

- Ruling: reject the helper-preheader candidate after formal +4.681% VS-int 4/all regression; do not hide it behind unrelated AA noise. Move the directive to the device caller before pointer setup. This removes the added EDX spill and makes all six normalized setter instruction streams/CFG equal to baseline. Remove the helper directive and type-trait dependency.
- Final caller pipeline: 54 passed, 0 failed, 2 external audit skips. Normal 12x4 all-case A/B, historical CPU0 controls, full CPU0 A/B and AA are recorded. The previous short-write residual no longer repeats; an unstable tail point estimate is explicitly reported, not rounded to zero.
- Game backup: 10 verified files. Installed DLLs remain unchanged while the elevated Fastman startup dialog requires manual user handling. No claim of game FPS or transition-bottleneck remediation.
