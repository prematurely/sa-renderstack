# F8 tail retention and texture mapping reclamation

User-authorized local work: fix lost F8 tail events, then optimize generic texture
mapping budget usage/reuse/reclamation to reduce main-thread long frames. Preserve
Vulkan staging/copy, rendering quality and public interfaces. Real behavior
regressions, repeated paired A/B with A/A, and full local validation are required.
No game deployment, game run, commit or push.

## Evidence and baseline

Capture 19168-1 retained 65536 events then discarded 21624 host-side events after
14.833 seconds. Its 114.83 ms peak at 10.61 seconds contained 299 OS unmaps costing
11.87 ms, 8/20 main samples through packImageData, and 9/10 CS samples waiting on
the command queue. The configured CPU mapping budget is 100 MiB; current eviction
continues to 75 MiB. Native allocation/copy/flush behavior must remain unchanged.

The deployed baseline is the immutable candidate in
`out/perf/20261114-f8-tail-mapping/baseline-candidate`. Initial uncommitted source
is preserved in `initial-source` with hashes. Compare changes against that source,
not just git HEAD, which predates the telemetry implementation.

## F8 retention decision

Retain exact completed events across the normal capture, using a bounded producer
queue and a capture-owned temporary sequential binary stream written by a managed
background worker. This replaces a lifetime prefix-only vector. The render thread
does not perform event file writes or wait for disk progress. All symbol lookup,
frame association and text formatting remain in the report worker.

Bound in-memory event staging to at most 2 MiB, allocated at capture startup;
bound temporary event storage to 128 MiB. Stream events back in bounded blocks for
the final report rather than loading the whole file into the 32-bit process.
The exact sizing and block packing are implementer choices within these bounds.
Use existing host worker ownership and stop/join conventions; no device reference
or COM access from the spool worker. No event-storage work outside active F8.

Queue pressure, allocation/open/write/read failure, or the file cap must be
explicitly reported as incomplete telemetry, with accepted/written/omitted counts
and an unavailable/error indication. Never emit an apparently complete zero-loss
stream after such failures. Reset/release/shutdown and restart must not lose already
accepted data silently, mix capture IDs, leak worker handles or leave temporary
files after their owner is destroyed. Keep the private backend interface and
existing report event/frameOverlap records compatible; append storage metadata.

At least one regression must exceed the former 65536 limit through the production
runtime and actual D3D9/Vulkan operations, verify an event at the late end of the
capture, verify frame association, and verify count conservation. Deterministic
failure injection belongs in fixtures at the existing dependency boundary.

## Mapping decision

Respect the explicit `d3d9.textureMemory` budget. The first candidate removes
unnecessary 25% over-eviction: when over budget, reclaim only enough LRU mappings
to return within the configured budget. Keep existing locks, active-upload scope,
subresource lifetime, and destruction/Reset ordering intact. This retains mappings
that fit within the budget and spreads small incoming demand across smaller trims.
Do not introduce an unbounded mapping cache, eviction worker or new loader hook.

Budget zero, exact boundary, oversized texture, all-candidates-pinned, allocation
failure, multiple devices and Reset/release need explicit treatment consistent
with existing contracts. Do not claim a time cap on reclaiming an oversized source.
Assess a larger bounded budget as a separate experiment only; do not silently
override the configured budget or change the game configuration.

An improvement must be visible in real OS map/unmap behavior and end-to-end
upload/Present latency, with identical uploaded/readback pixels. Controls include
already-below-budget reuse, cold sequential uploads and working sets larger than
the budget. If repeated A/B shows no useful improvement or unacceptable control
regression, reject/adjust the optimization rather than retaining a code-only win.

### Evidence-led refinement: initialization and LRU reuse

The retained real prefix also contains 879 cookie-matched remaps after unmap, 632
within 16 ms. This does not identify every caller. A concrete source path exists in
InitTexture: initialization unconditionally unmaps before the first application
Lock. Evaluate retention of the already initialized unmappable source within the
same LRU/budget, after SyncSharedTexture succeeds. Register only after all pointer
uses finish; an internal device method takes the existing device lock. Initialized
objects are complete and held by the caller's Com. No new COM/API surface.

The evaluated always-admit MRU variant polluted the `unused` control: a single
512-operation smoke observed501 additional OS remaps versus reclaim-only. Preserve
that discarded variant as evidence. The final policy admits an initialized mapping
only if total logical mapped bytes already fit the same explicit budget; otherwise
it unmaps that new source and applies ordinary reclamation. First actual Lock still
uses normal demand-driven LRU admission. Verify old hot data stays reusable across
create-but-never-use pressure, including existing pinned/locked mappings.

Existing LRU touches allocate a fresh list node. Use list splice for existing
entries and strong exception safety for first insertion (rollback list node if
index allocation fails). Initializer admission allocation failure falls back to
unmapping the new source and ordinary reclamation, preserving creation behavior.
No new allocation failure may leave a dangling LRU pointer.

Real tests must distinguish Create/initialization from first Lock, preserve initial
zero/provided-data pixels, bound create-only memory, protect existing locks, and
exercise MULTITHREADED creation/reclamation and allocation-failure cleanup. Include
create-and-never-use pollution control. Freeze reclamation-only and combined builds
to separate their contributions; keep any initializer-retention change only after
same-budget paired data supports it. Do not label all observed remaps initializer-caused.

## Validation and delivery

Run focused red/green native runtime and texture lifecycle/pixel tests, then task
review. Run fixed-binary version A/B and A/A serially without parallel builds or GPU
tests; include P95/P99/max/burst latency, unmap counts/timing and mapped-memory peak.
Measure F8 inactive, normal active and high-volume active cases, including report
completion latency and failure counters. Run the complete repository pipeline with
the verified historical Bridge reference and installed toolchain. Freeze the local
candidate and matching symbols; verify all eight protected installed artifacts
remain unchanged. Fixture results do not establish game FPS or exit-spike gains.

Mapping lifetime totals include release of measured resources and the final GPU
drain. Report release time separately, within that total, so retaining an initializer
mapping cannot appear beneficial only by deferring its unmap beyond the timer.
