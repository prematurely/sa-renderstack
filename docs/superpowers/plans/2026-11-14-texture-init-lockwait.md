# Texture Initialization and Lock Wait Implementation Plan

> For agentic workers: use superpowers:executing-plans natively, task by task.
> Preserve uncommitted baseline; no commits, pushes, deployment or game runs.

**Goal:** Remove proven redundant initialization work and evaluate bounded command
pool growth at the observed LockRect synchronization path.

**Architecture:** Fresh-allocation provenance under existing allocator ownership;
small command-buffer batches without changing synchronization or submission order.

**Tech Stack:** Existing x86 C++, Windows D3D9/Vulkan, LLVM-MinGW/MSVC, native probes,
PowerShell/Python orchestration.

**Spec:** docs/superpowers/specs/2026-11-14-texture-init-lockwait-design.md

## Global Constraints

- Preserve staging/copy, rendering quality, public interfaces, configured mapping budget.
- No MOD-name dependence, new runtime dependencies, deployment, game run, commit or push.
- Tests use real device/OS paths; repeated fixed-binary A/B and A/A and full pipeline.
- Preserve previous uncommitted changes and all eight protected game-file hashes.

## Review Focus

- Recycled/shared storage must never be mistaken for fresh zero memory.
- Failed allocation/map must not corrupt allocator state or expose stale bytes.
- Concurrent creation, active locks and GPU upload pointers preserve lifetime.
- Command buffer reserve is bounded; failed batches cannot strand handles.
- Required readback ordering, CPU/GPU synchronization and pixels remain unchanged.

### Task 1: Fresh texture initialization

Files: util_unmap.h/.cpp, d3d9_common_texture.h, d3d9_initializer.cpp,
tests/texture-init-probe.cpp and tests/texture-init-test.ps1.
Private interface: allocation-origin zero proof, consumed only at initialization.

- [ ] Preserve source/runtime hashes; implement real zero-write observation and byte/pixel controls.
- [ ] Run baseline RED for redundant fresh initialization; keep failure log.
- [ ] Implement conservative fresh-range proof and zero-clear elision; preserve map and data paths.
- [ ] Run fresh/reused/shared/explicit-data/mip/face/volume/failure/concurrency/budget controls.
- [ ] Freeze initialization-only binary and focused validation evidence.

### Task 2: CS command pool allocation experiment

Files: dxvk_cmdlist.cpp, tests/lib/vulkan-dispatch-observer.h,
tests/lockrect-pool-probe.cpp, tests/lockrect-pool-test.ps1.
Public interfaces unchanged. Observe existing Vulkan dispatch boundary only in tests.

- [ ] Build real readback/driver allocation observer; preserve baseline measurements and RED.
- [ ] Evaluate two-buffer growth with single-buffer fallback on allocation failure.
- [ ] Verify exact readback pixels, late writes, multiple resources, reset and allocation-failure cleanup.
- [ ] Freeze combined candidate; reject or refine if controls reveal unsupported tradeoffs.

### Task 3: Performance and final validation

Files: narrowly needed tools/test.ps1 wiring, benchmark runner/validator, validation report.

- [ ] Run serial repeated A/B and A/A for initialization, existing mapping workloads and readback controls.
- [ ] Compare initialization-only vs combined to separate contributions; include total cleanup/drain.
- [ ] Request independent whole-change review under executing-plans; resolve important findings with RED/GREEN.
- [ ] Run complete tools/test.ps1 with real hardware and historical Bridge reference; inspect every result.
- [ ] Freeze verified local candidate/symbols/source/import manifests, verify game hashes unchanged.

## Execution ledger

Scope ruling: the latest user directly authorized optimization of the two measured
paths under the established regression/benchmark constraints. Routine implementation
choices remain with the implementer. Work is native and uncommitted; preserve an
immutable initial-source snapshot instead of using commit ranges for review.

Ruling: extend the existing real mapping-budget probe and runner, rather than
duplicating its device/pixel/residency setup in new texture-init files. The same
native paths cover fresh and recycled memory; existing benchmark fixtures stay
fixed during each comparison.

Task 1 evidence: baseline fresh-zero RED (262144 redundant bytes), candidate
GREEN (0 fresh / 262144 recycled bytes, 66 checks). Allocation failure retry
baseline RED (3 failures), candidate GREEN (21 checks). Initializer/concurrent
296449 checks and 1 MiB pressure 284 checks passed.

Task 2 evidence: two-buffer growth passed 4974 checks and injected-failure
fallback 4975 checks, but only reduced 7 driver calls to 6 while allocating 12
buffers instead of 7. Six A/B + three A/A pairs in two readback workloads showed
no repeatable benefit (A/B total +5.06%/+5.24%, broad A/A variation). Reject this
batching experiment; preserve its frozen binary and evidence, restore original
command allocation and retain generic real readback regression in the suite.

Ruling: evaluate a second targeted mapping improvement: prioritize never-used
initialized mappings for eviction, without evicting hot mappings merely to admit
another initialized source. Source inspection found all retained initial maps
currently share the same recency queue as used maps; at the budget ceiling this
forces an immediate unmap/remap even while never-used maps remain resident.
Use the existing LRU with cold insertion and one per-texture use flag, not another
cache or an expanded memory budget. This directly addresses the requested
repeated mapping reclamation; retain only after exact bytes/pixels, pinned-map,
multiple-device, repeated A/B/A/A controls pass.

Mapping admission RED: first-use maps=1 and hot-survivor maps=1; GREEN: both 0,
368 checks. Existing initializer/concurrency (296449) and pinned 1 MiB (284)
checks still passed. Added real volume/mip zero checks (296479 total initializer).

Ruling: fix failed MapView ownership as part of the mapping budget path. The
new fault test proved one failed view permanently consumed 1 MiB + 64 bytes of
budget and evicted an otherwise resident hot source (3 baseline failures).
Commit map/page reference counts only after successful mapping, never offset a
null base pointer. Candidate passes both exact-budget retry and a failed shared
page mapping at a nonzero offset (170 checks).

Benchmark evidence: init-only six A/B and three A/A pairs over six workloads
completed; create total -7.80%, cycles -3.82% (6/6 cycle pairs faster), hot cycles
-3.77% (6/6). Other controls near A/A spread. Final mapping candidate comparison
adds the admission workload and is in progress. Two-buffer experiment is removed
from product code; its source, binary, observer and fallback evidence are retained
under out/perf/20261114-init-map-lockwait/rejected-pool and combined.

Independent whole-change review: no Critical/Important/Minor introduced findings.
All 14 review-file hashes matched, command pool source matched the initial snapshot.
Boundaries: observer verifies real readback/reset/lifecycle, not independent fence
completion proof; dedicated >256 MiB path code-reviewed, not specifically exercised.
Inherited allocation-index-zero bool and later LockImage HRESULT behavior left
outside this patch; no new dependency on either for the fresh-memory proof.

Build audit caught a stale command-pool object after source rollback: Copy-Item
preserved an old source mtime, so the next incremental build retained batch2.
The mapping-candidate/final-ab files are INTERMEDIATE COMBINED experiments,
not final acceptance. Preserve them, force recompilation of the restored source,
verify observed batches=0, freeze a new final binary, then repeat final A/B/A/A
and the complete pipeline. Earlier init-only and pool-ab experiments retain their
original correct scope. This is a build-artifact correction, not a product change.

Final source rebuilt; native observation batches=0, 7 calls/7 primary buffers,
4972 checks passed. Complete pipeline 2026-11-14 00:18:15–00:27:01 UTC completed
in one run: 102 passed, 0 failed, 2 optional external-reference audits skipped.
All required gates passed, including new mapping/failure/readback checks and F8
tail/HUD/runtime checks. Frozen performance backend SHA256:
52e725a9758a976a111c9d4f34caf8af34177cccc91c25153022a414f5304b6b.
accepted-ab repeats all seven workloads (6 A/B + 3 A/A pairs each) against the
installed baseline. Final readback controls use 2048 operations per process.

Integration ruling: the user's existing scope is local candidate, no deployment
or commit/push. Keep this branch and all evidence in place; no integration menu
or extra approval is needed. This supersedes the finishing skill's default menu.

Final measurements completed: 126 mapping runs, 36 LockRect runs, all valid and
fixed-binary hashes preserved. Target admission: total -26.81%, main cycles
-29.18%, maps 2043→1024, operation P95 -25.66%; all 6 target pairs improve.
Creation total -3.97%, hot reuse cycles -4.30%; remaining controls near A/A.
Readback total -0.36% / upload-readback -0.72%, within A/A variation: do not claim
the 15.54 ms game CS wait is fixed.

Explicit open tradeoff: admission end cleanup unmap 0.5866→7.1352 ms, complete
release 7.5314→12.3047 ms; both versions release 98 maps and have identical end
mapped bytes. Full-life unmap 93.8789→57.3136 ms and total cost improve, including
cleanup, but concentrated-release latency regresses. Keep this visible in the
local candidate/report; no deployment or claim that game long frames are solved.
