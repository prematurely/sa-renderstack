# Unified measurement capture and HUD

Date: 2026-10-27 (Asia/Shanghai)

## Intent and agreed approach

The user requested a visible measurement HUD and a merged F7/F8 workflow, then
selected sequential collection to distinguish diagnostic overhead. Their latest
clarification makes diagnostic precision the priority: one F8 press should
collect the useful information from F7 and F8, and reports must explain concrete
functions and measured work rather than repeatedly stopping at module totals.
Keep picture quality, rendering correctness, and public API1-7/exports unchanged.

One press of F8 starts a capture; F7 is an alias for the same workflow. Both keys
use edge detection. Holding either key or pressing the other during a capture
must not start a second capture or restart the timer.

Alternatives considered: simultaneous CPU/state attribution would observe the
same interval but mix their measurement costs. Separate keys would retain the
current fragmented workflow. The selected approach runs the phases in order.

## Capture behavior

The proposed defaults are 5 seconds of baseline, the existing CPU-hotspot
duration (currently 10 seconds), then the existing state-attribution duration
(currently 3 seconds). Preserve the configured CPU sampling interval. Start
and transition phases at the owning device's Present boundaries.

The baseline enables neither CPU suspension sampling nor journal attribution.
It measures the normal configuration with the same HUD enabled. All reported
frame rates use clearly identified successful Present counts and elapsed time;
they are not CPU/GPU execution times. Keep separate summaries for each phase.

Use both parts of the current F7 behavior: Bridge state/journal attribution and
DXVK's existing D3D9 state audit. Today the backend separately polls F7 and dumps
its own three-second counters; the combined controller must own their timing
and associate both results with the same capture. Do not revive the legacy
effect-patching profiler or enable experimental state-save bypasses.
Retain standalone legacy behavior when the new combined mode is disabled.

On Reset, final device release, shutdown, or sampler failure, end the current
capture with an explicit interrupted/error result. Preserve completed phases;
never display a failed or interrupted phase as a successful measurement.

## HUD and data flow

Reuse the DXVK HUD renderer and current FPS/frame-time display. Add a compact
RenderStack item with ASCII labels supported by the existing font:

- Capture ID, current phase, elapsed/remaining time and progress.
- Live FPS and average frame interval, plus the maximum observed interval.
- Separate baseline, CPU-sampled and state-attribution summaries.
- CPU sample/missed counts, named function hotspots where verified symbols
  exist, and completed journal/API attribution counts.
- Ready, collecting, complete, interrupted, or error status; log location.

Refresh displayed text at a bounded rate, approximately 5 Hz. Reuse fixed-size
capture statistics; do not read log files or scan virtual address space in the
HUD's per-frame path. Do not add GPU queries or a per-call D3D9 wrapper merely
to display diagnostics. F8 CPU results remain statistical samples, not a
computed percentage of recoverable frame time.

Bridge publishes a bounded snapshot through an additional private COM interface
with a distinct IID, independent of existing lifecycle and public SDK vtables.
The backend HUD renders a copied snapshot. No new DLL exports or public API1-7
changes are required. A backend without this optional interface keeps logging
and explicitly reports unavailable HUD integration.

## Diagnostic precision and measurement algorithm

Current limitations were checked in source: CPU stacks are formed by scanning
64 stack words for executable-looking values, brief caller strings omit the
module base, only the top 256 IP rows are serialized, and the separate F7
reports have no common capture identity. These directly limit attribution.

The proposed revision addresses these gaps without promising unsupported full
unwinding of arbitrary optimized x86 third-party code:

- Preserve full module identity (path, loaded base, image identity and RVA) for
  sampled IPs and candidate callers, so the two d3d9.dll modules cannot be mixed.
- Generate function ranges for the actual Bridge/DXVK builds from existing
  linker-map/COFF symbols, with strict binary identity matching. Resolve known
  system exports where possible. Label missing, ambiguous, or mismatched
  symbols explicitly; do not guess names from another build.
- Keep executable-address stack scanning explicitly marked heuristic. Where a
  bounded captured x86 frame chain can be validated, report that separately;
  count unresolved/truncated chains rather than manufacturing callers.
- Correlate samples with monotonic timestamps and observed Present intervals.
  Summarize slow intervals separately from ordinary ones, with sample coverage
  and truncation counters. A sampled function percentage is never rendered as
  exact milliseconds saved or an exact duration.
- Use bounded sampling jitter around the configured average interval to reduce
  fixed-interval phase bias. Copy only bounded raw state while suspended, then
  resume before allocation, symbol lookup, aggregation, or file output. Record
  sampling failures and directly measured suspension time. Suspension time is
  one component of observer overhead, not a complete estimate of its FPS cost.
- Collect the existing backend state-audit counters during the attribution
  phase: calls/frame, redundant calls/units, measured total/maximum elapsed
  time, binding-cache hits/invalidations, and prepare-draw categories. Expose
  summaries through the private integration rather than scraping logs.
- Associate Bridge journal transactions with technique/pass and present frame;
  display transaction-span and restore timings with explicit inclusive labels.
  Do not sum nested timings into a fictitious total frame cost. Show missing or
  unavailable attribution instead of interpreting no records as zero cost.
- Reduce demonstrated measurement-path overhead: the current journal admission
  function takes an exclusive lock even when attribution is disabled. Add a
  cheap inactive check while retaining the locked admission check that pairs
  transactions with StopCapture. Avoid repeated hot-path symbol resolution or
  string formatting; verify active counts and shutdown behavior are preserved.

The HUD presents the top verified results and measurement quality. Detailed
per-function/per-frame and technique/API tables remain in the combined report.
No new game streaming hooks are introduced; a resource-loading bottleneck is
not considered proved merely because one CPU sample lies in a loading module.

## Reporting and verification

Link the phase records with one session/capture identity, preserving the raw
CPU and state-attribution outputs and recording phase durations and failure
states. Produce a combined summary with observer-stage labels. Existing tools
must retain valid standalone log behavior.

Before implementation, add behavior tests that fail for the missing workflow:
phase ordering, key edges/repeated keys, FPS/time arithmetic, disabled/missing
attribution, sampler failure, Reset, release, and shutdown. Add fixtures with two
same-basename modules, wrong symbol identities, ambiguous function ranges,
heuristic-versus-validated callers, bounded buffers, sampling jitter, and known
slow/ordinary Present intervals. Exercise actual journal/state-audit counters
with real API calls and true duplicate/changed state, and verify inclusive
timings are not double-counted. Exercise the private
snapshot interface and both wrapped/raw device lifecycle paths on real D3D9/
Vulkan devices. Verify HUD rendering through the actual render path where the
test harness permits, and report any remaining visual acceptance separately.

Run repeated paired measurement-overhead benchmarks with HUD/capture idle and
active states kept distinct. Run the full repository pipeline and existing
state, pixel, lifetime, layout, and protocol checks. Preserve all pre-existing
dirty work and the installed Fading fix. No commit, game launch, or deployment
is part of this implementation batch; validated candidates remain in out/.

## Scope

This batch improves measurement and its feedback. It does not change streaming
budgets, StateRing policy, game threading, texture-binding optimization, or
rendering quality. The original streaming configuration restored for comparison
remains installed until separately changed.

## Design review

A fresh read-only builder check of this revised design reported "nothing
important left". This checks that the design can be planned; it is not runtime
verification or approval of an implementation.
