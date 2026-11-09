# Thread attribution and generic journal implementation plan

> Execute inline with superpowers:executing-plans; use a fresh final reviewer. The user authorized the instrumentation-first sequence and complete automatic validation. Do not commit or deploy.

**Goal:** distinguish API-thread CPU work, CS starvation/backlog and synchronization, then reduce measured generic journal overhead.

**Architecture:** private read-only boundary snapshots, F8 report integration, independent native D3D9 fixtures. Keep public SDK/vtables/exports and quality settings unchanged.

**Tech Stack:** C++17, DXVK Vulkan/D3D9, Win32 thread CPU queries, PowerShell test orchestration.

**Spec:** ../specs/2026-10-31-thread-journal-design.md

## Global constraints
- Preserve all pre-existing dirty work and installed game files; source-before snapshots in out/perf/20261031-thread-journal.
- No additional libraries, services, MOD-name recognition or game-specific addresses in new functionality.
- All performance conclusions identify fixture scope; game FPS remains unmeasured.

## Review focus
- Ongoing CS idle intervals must be accounted for without waiting for a wake-up.
- Caller thread identity and counter availability must prevent misleading CPU percentages.
- Reset/release/aborted capture must not retain an interface/device self-reference.
- Sparse journal cleanup must preserve COM ownership and normal StateBlock capture semantics.
- Diagnostic support must work with gtaSaCompat disabled and no game/MOD modules loaded.

## Tasks
- [ ] 1. Save status, source and installed hashes; add actual-device tests that fail for missing private thread query. Build/run RED against baseline and preserve baseline DLL.
- [ ] 2. Implement CS queue-idle snapshots/private query; integrate per-phase snapshots in F8 reports; verify CPU busy/idle, nonblocking reads, sequence progress, waits and unavailable behavior.
- [ ] 3. Run independent sparse/dense state/journal/control workloads and identify cost; add regression for the chosen optimization and retain existing pixel/lifecycle tests.
- [ ] 4. Implement targeted generic improvement, verify behavior then repeat matched A/B including instrumentation overhead and controls.
- [ ] 5. Wire required gates, build all, run full tools/test.ps1, fresh review and resolve material findings; inspect imports/exports/modules, package candidate and verify protected hashes.

## Ledger
2026-10-31: User-approved research sequence is the selected approach; routine instrumentation mechanics are implementation decisions. Continue in the established dirty checkout because earlier uncommitted validated changes are required; no worktree or commit requested.

Instrumentation RED: private QI absent on saved backend, real device probe 810 checks/1 expected failure. F8 report RED using saved production runtime and new backend: 67 checks/1 expected missing-thread-row failure. GREEN: current runtime 67/0. Extended native probe 1264/0; gtaSaCompat disabled 235/0, same private activity query and actual pixel readback work without any MOD module.

Diagnosis: pure sparse/tail/dense journal workloads have zero CS dispatch, zero CS CPU and queue-idle approximately wall time. Actual draw+readback creates CS work and nonzero completed CS/GPU waits. Targeted source changes: captured-set journal cleanup and word-wise constant-run iteration; no MOD-name lookup or game addresses added.

Fresh reviewer found two evidence issues, no core-code correctness issue: completed timing included printing; external AddRef counts did not test private retained resources. Both addressed: timestamps taken before printing, completion wait HRESULTs/deadline checked; non-null texture/VB/IB destruction sentinels cover restore and Reset/discard. Deliberate texture-release omission mutant fails sentinel test (1264 checks/1 failure), then correct source restored and full build succeeds. CPU phase and wait-counter assertions added.

First A/B is a pilot only: tiny control windows showed excessive scheduling variance; reviewer also identified completed-time logging contamination. Final protocol uses longer loops, pins only the fixture main thread to an allowed CPU, leaves worker affinity unchanged, and separates logging from all timing windows. Preserve pilot logs; use paired-final for acceptance.
