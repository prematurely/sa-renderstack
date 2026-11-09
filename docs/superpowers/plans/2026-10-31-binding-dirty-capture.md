# Generic binding, dirty propagation and first-write capture

Execute inline with one fresh final review. User selected these three measured hotspots. No commit, deployment, game launch, quality change or public interface change.

Baseline: backend D57B5D8BE5144B17CE71493EA8D924C46109827303CD1A79396EBFCF1475F710. Preserve existing dirty checkout; status/diff/source/installed files saved under out/perf/20261031-binding-dirty-capture.

## Design and acceptance

- Borrow image-view references until an actual binding command/cache entry needs ownership. Keep command and cache ownership, Reset and resource replacement intact.
- Propagate sampler dirtiness from the state actually consumed by sampler creation. sRGB selects a texture view; only its low bit matters and it affects the sampler only when a border view is consumed. Ignored sampler fields and unused border colors remain stored and captured. Preserve Fetch4 and upload/mipmap/hazard paths.
- Capture only previously untouched constant registers using bounded word scans; preserve first-write values across overlapping writes, holes, stages, types and word boundaries.
- Also read old texture format before releasing the old private reference; replacement can destroy an externally released bound texture.
- Do not add a common-texture shadow cache: current GetCommonTexture is already an inline offset access, not a virtual dispatch.
- Native tests must exercise actual DLL behavior, pixels, overlapping constant writes, resource destruction and Reset, with compatibility enabled and disabled. Repeated matched A/B includes controls and completed-work timings. No game FPS claim from fixtures.
- Check exports/imports, no-MOD isolated execution and installed-file hashes. Run the complete pinned-reference pipeline.

## Tasks

- [x] Add characterization and new helper tests; retain baseline results and observed RED.
- [x] Implement bounded first-write scanning and precise binding/dirty changes; run focused native tests.
- [x] Repeat A/B on sealed bytes, investigate any control regression, retain supported changes only.
- [x] Full validation, fresh final review, independent execution, candidate package and report.

## Ledger

2026-10-31: User explicitly requested all three selected targets. Routine implementation decisions are authorized. No repeated design approval needed. Candidate only. A/B baseline is the currently deployed validated backend, not the previous batch's invalidated mutant. Existing working tree includes required prior changes and is preserved without a commit.

Characterization: baseline 1640/0 before pixel extensions; new first-write helper test observed missing-header RED, then GREEN with independent randomized 8192-register oracle. Mixed-device test initially compared unread outputs beyond the existing public getters' hardware clamp: both baseline and candidate failed identically. Ruling: retain getter behavior and compare the readable window; extended writes still exercise capture/restore, while the full-range helper oracle covers exact high-register masks. Do not claim native high-register readback coverage.

Real-device dirty propagation RED: saved baseline fails exactly the sampler-unit and texture-unit expectations (1848 checks / 2 failures); refined candidate passes 1848/0. Linked scalar and batch cache-hit guards confirm no atomic ownership work; the constant-loop layout guard also passes.

Pilot rejected as-is: per-register-sized capture paths and dense controls regressed. Added Count==1 shortcut and word-wise captured-mask marking. Refined 12-pair x 5-round native A/B: capture overlap -53.29%, capture repeat -56.19%, dense -4.17%, tail -4.12%, sparse -4.27%; control -0.86% (7/12 faster), empty -6.08% (9/12). Completed timing agrees; no native game FPS conclusion.

Previous October 27 borrowing experiment was rejected for control regressions. This batch must re-pass the full 78-case on/off control matrix and texture subset, not infer acceptance from fewer atomics. Fresh reviewer found no production defect; identified sRGB-only border-sampler branch coverage gap. Added actual draw + sampler/texture unit assertions without a concurrent border-color change; pending final execution.

Matched timing complete: 9 draw/setter groups, 22176 timing records including A/A; separate journal group 840 records. Two on-profile texture windows show scalar hits -7.27/-7.89%, double hits -13.13/-12.78%, mixed hits -7.51/-9.39%; corresponding A/A +0.36/-0.01/+0.99%. On/all miss-heavy texture-draw initially +4.32% (1/12 faster), texture subset +0.41%, repeated controls +2.41%, repeated texture subset +1.52%; A/A controls +2.59%. Completed timings also retained. Ruling: the miss/control results do not establish a stable improvement or a repeatable regression beyond fixture variability; retain this combined candidate on the consistent hit/constant improvements, report all positive estimates and do not claim zero regression or game FPS. Full on/off 78-case matrices and texture controls preserved.

Fresh reviewer confirmed added sRGB-only border draw/counter assertion resolves the Minor gap at source level; no remaining review findings. Await final execution. No code or timed binary changes after sealing A5015E14E46CCCC60D0C31170BF327D0F5813B888FAC7EE5EDFD38E98AE8C275; only regression coverage and gate wiring changed.

First complete run: 75 passed, 3 failed, 2 optional skips. Two hygiene gates found a manual probe log in the repository root; preserve/move it under this batch's out directory. measurement-backend's old cache-hit fixture deliberately changed only ignored sRGB high bits; the new optimization correctly prevents that dirty path. Preserve failed report/logs. Update the fixture to change/restore consumed ADDRESSU and texture identity before draw, then verify the final high-bit-only change adds no prepare event. New counters 7 sampler calls / 5 texture calls / 2 prepare draws, focused 3430/0. Reviewer confirmed the test-only correction, no open findings. Re-run full pipeline; production code unchanged.

Final acceptance: complete pinned-reference pipeline exit 0, 78 passed / 0 failed / 2 optional external-source skips. Native on 1867/0, mixed 1883/0, off 446/0; measurement-backend 3430/0. Independent directory with only backend + probe, own performance flags enabled, repeats on/mixed/off with the same counts and no game-directory modules. Exports/import contracts unchanged; final backend 943827FE52992B02973BDECFCF51E2AB168D06176D4D0F73217F5305D143A50D has identical sections to timed A501 except build identity. Candidate packaged with matching symbols and unchanged config. All protected installed hashes/sizes/mtime unchanged. No deployment, game test, commit or push. Report out/reports/20261031-binding-dirty-capture-validation.md.
