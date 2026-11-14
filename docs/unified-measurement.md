# F8 unified measurement

The candidate combines the previous CPU-hotspot and state-attribution captures.
Press F8 once, then keep the scene and camera fixed until the HUD reports a
terminal state. F7 starts the same sequence. Holding a key or pressing either
key during a capture does not restart it.

The default sequence is 5 seconds of baseline, 10 seconds of CPU sampling, then
3 seconds of journal/backend attribution. The CPU duration and interval still
come from `[CPUHotspotProfile]`; the attribution duration comes from
`[PerformanceDiagnostics] StateAttributionDurationMs`.

```ini
[Measurement]
Enable=1
ShowHud=1
CpuHotspots=1
JournalAttribution=1
BaselineMs=5000
SlowFrameMs=50
CsIntervalMs=10
```

`Enable=0` retains the standalone F7/F8 behavior. `ShowHud=0` keeps collection
enabled and hides the display. The Bridge adds the `renderstack` DXVK HUD item
when the combined display is enabled. A backend without the private integration
can still provide CPU/journal reports; the missing backend/HUD is recorded as
unavailable. The public API1-7 interfaces and exports are unchanged.

The HUD updates at most five times per second. It shows stage, progress, live
successful-Present FPS and intervals, each completed stage's FPS, sampling
quality, and the leading function/API/journal result. Files are written after
sampling; the game thread does not parse report files for the display.

The combined report is
`scripts/BridgeD3D9.measurement-<process-id>-<capture-id>.log`. Raw CPU output
continues in `scripts/BridgeD3D9.cpuhotspots.log`, and the original journal
technique/pass/operation report is retained. Preserve the corresponding DLLs
and adjacent `d3d9.dll.rsym` files when sharing a capture.

Interpret the results using their recorded units:

- Baseline, CPU and attribution FPS are separate successful-Present counts over
  elapsed time. The latter two include their diagnostic overhead. These are
  observed presentation intervals, not CPU or GPU execution times.
- CPU rows contain sampled IPs, module path/base/RVA, function, and symbol
  quality. Sidecars must match the exact DLL SHA-256. Missing, inferred and
  ambiguous symbols remain labelled. A sample percentage is not an exact
  function duration or a promise of FPS improvement.
- Captured x86 frame chains receive structural validation; arbitrary executable
  stack addresses remain explicitly heuristic. This is not a full unwinder.
  Suspension time, failed samples, unmatched intervals and dropped bounded
  records are reported separately.
- Slow/ordinary classifications use the observed Present interval containing
  each sample and the configured threshold. They associate evidence with a
  frame interval; they do not prove which sampled function caused the delay.
- Backend timings cover only calls counted as timed. The measured scope starts
  after the device lock is acquired, so lock-acquisition wait is excluded.
  Nested calls and journal transaction/restore spans are inclusive; do not sum
  them as disjoint frame costs. Untimed or unavailable work is not zero cost.

Reset, release, or shutdown interrupts an active capture. Attribution closes
admission immediately and drains previously admitted transactions on a worker.
If draining times out, the report stays interrupted/partial even if a late
transaction eventually finishes. A new capture waits for that transaction to
finish so it cannot overwrite the previous capture's tables.

F8 reports also contain `threadActivity` rows for baseline, CPU and attribution.
These use an optional private backend query independent of GTA compatibility
and third-party MODs. It samples only at phase boundaries, never flushes CS/GPU,
and reports scheduled main/API-thread and CS CPU time, CS condition-wait idle
time (including a wait that has not finished), ordered sequence progress and
pending work including in-flight work, plus existing CS/GPU waits and submission
counters. There is no per-setter clock or additional worker for this feature.

CPU time is Windows kernel+user thread time with finite accounting precision;
very short windows can report zero CPU time or a percentage above 100. Use the
normal multi-second phases. Non-CPU wall time is not proof of idle or a particular
wait. Existing CS/GPU wait totals are device-wide completed spans that can cross
boundaries; GPU idle is estimated and chunk totals are merged on submission.
These counters are not additive frame costs, and pending excludes injected
high-priority commands. Missing support, incomplete windows or changed caller
thread identity produce `available=0`, without interrupting other diagnostics.

The extended F8 capture records allocator and synchronization evidence through
an optional, separate private telemetry interface. Collection is off outside
F8; compatibility mode and third-party MODs are not prerequisites. The existing
Vulkan staging allocations, texture copies and rendering order remain intact.

`telemetryEvent` records contain QPC-derived nanosecond begin/end times, calling
thread, operation kind, identity, bytes, result and scope flags. The scopes
distinguish resource suballocations, Vulkan backing allocations, file-pool
operations and actual OS map/unmap calls. A logical map can reuse an existing
mapping, and a resource free can return space to a pool without freeing its
backing allocation. Reusing an existing mapped pointer does not produce an OS
map call; these events do not time every D3D9 Lock/Unlock API invocation.
Allocation bytes can be requested size while free bytes include alignment, so
their difference is not a physical-memory balance. Submission-status waits
are distinguished from GPU-completion waits, and a device-idle span can include
queue draining and driver work. These inclusive spans may nest and must not be added as
independent frame costs. Successful operations below both 50 microseconds and
64 KiB are filtered; failures remain eligible. The event queue and retained
history are bounded, and the report includes filtered, dropped, late and
in-flight counts. Completed events do not describe an operation still waiting
when collection stops.

Completed backend events are drained in bounded batches on active Presents,
independently of the 500 ms memory-snapshot cadence. Capture history uses a
32,768-event staging ring and a managed background writer, with less than 2 MiB
of simultaneous event staging and a 128 MiB temporary binary file limit. The
render callback neither writes event files nor waits for the writer. The report
worker streams the file in 256-event blocks, preserving late events beyond the
former 65,536-event lifetime limit. Temporary files are deleted when the capture's
writer/reader ownership ends; idle F8 does not start an event-storage worker.
After observing seal or abort, the writer acquires the queue head again before
deciding that the stream is drained. This preserves events published between an
earlier empty-queue observation and the final stop publication.

`telemetryStorage` is the final storage-completeness record. It separates received,
accepted, written, reported, omitted and unread events, and records queue pressure,
file open/write/read errors, file-cap exhaustion, worker-start failure and shutdown
cancellation. `complete=0` and the telemetry unavailable bit make partial evidence
explicit. The report header is finalized after streaming so a read failure is also
visible there. Backend drops and operations still open or unread after stop also set
the telemetry unavailable bit; their individual counters remain in the report.
An operation in flight while capture is active is normal and does not alone mark
the capture unavailable. Do not infer zero lost events merely from a report file
existing or the phase controller reaching its end.
Terminal accounting errors use bit 256: accepted/written/received ordering,
written plus omissions, drained head/tail, and reported bounds must agree. The
check flags a discrepancy without inventing omissions to repair the totals.
A complete stream additionally requires received=accepted=written=reported and
zero omissions and errors.

The final `reportEnd captureId=... completed=1` row confirms that report generation
reached its terminal record; storage completeness remains a separate check. Active
runtime snapshots count fully spooled events. After reporting finishes they count
only events in the successfully finalized report, and include omitted or unread
events in the loss count. Report-open/write failure leaves zero finalized events
and an explicit report error rather than treating worker exit as successful output.

`memorySnapshot`, `memoryChunk` and `memoryMask` persist the pooled allocation
data used by the bottom-right HUD. Each chunk has a stable cookie, heap/type,
capacity, used bytes, mapped/active flags and occupancy masks. Snapshots are
taken at capture boundaries and at the first Present at least 500 ms after the
previous one. Their actual spacing, scan time, missing observations and
truncation are recorded. No Present means no periodic allocator snapshot;
there is no additional flush or synchronous GPU readback to obtain one. These
pooled totals exclude dedicated allocations and are not whole-process RAM or
total GPU memory. The bottom-left frame-time HUD layout is unchanged.
The runtime retains at most 128 snapshots, 256 chunks and 4096 mask words per
snapshot. Adjacent snapshots also produce chunk changes; missing chunks in a
truncated snapshot remain uncertain instead of being reported as freed.
Frame-latency waits and sampler-pool fence waits are separate kinds. Their
pending check means the signal was incomplete when observed; it can become
ready before the thread actually sleeps. Swapchain image acquisition is an
inclusive CPU/driver/acquire span, not a pure GPU timer.

CPU sampling uses one worker that suspends the main thread and CS separately,
never together. Main sampling keeps its configured interval; CS defaults to
10 ms. All symbol resolution, stack interpretation and report writing happen
after resuming the target. `cpuStream kind=main` and `kind=cs` report separate
sample counts, errors, missed/dropped records, structural-chain quality and
suspension time. Failure to sample CS does not discard the main stream. The
HUD says when CPU sampling is running instead of retaining the baseline
waiting message.
Actual sample count and elapsed sampling time establish the achieved rate.
Scheduling misses count late wake-ups, not every interval extended by sample
processing. A zero miss count does not prove the nominal rate was achieved.

`frameOverlap` associates event spans with each intersecting half-open Present
interval. Samples and memory snapshots also carry frame references. A phase
boundary whose start is not a successful Present is marked truncated; trace
overflow, phase gaps and out-of-window evidence stay explicitly unmatched.
This is temporal association, not proof that every overlapping operation
caused a long frame.

A separate 25 ms observer reads the device window's foreground/minimized state,
the Present heartbeat and the host's safe game-frame counter. It does not call
the device or hold its reference. Its bounded history remains useful when
Presents pause. Reports preserve raw observations, background exclusions and
candidate freezes; rendering work and pixel equality remain unknown.
Freeze candidates require at least 250 ms without game-frame progress and
distinguish a continuing Present heartbeat from a pause in Presents.
All instrumented phases retain their measured overhead, including the memory and
context collection during the baseline phase. Baseline still excludes CPU
thread suspension; it is not a completely uninstrumented run.

The candidate is validated with native D3D9/Vulkan fixtures, symbol fixtures,
repeated serial measurement-overhead benchmarks, and the repository pipeline.
Native HUD captures verify the renderer with fixture text. Game appearance,
actual Grove Street frame times and indoor/outdoor behavior require a separate
manual game run after deployment is authorized.

The measurement-coverage benchmark runner uses already-built binaries and
records their hashes before and after each independent process:

```powershell
pwsh -NoProfile -File tests/measurement-coverage-benchmark.ps1 -Mode Backend `
  -Baseline <immutable-main-d3d9.dll> -Candidate <candidate-d3d9.dll> `
  -Probe <measurement-telemetry-probe.exe> -OutputDirectory <new-output-directory>
pwsh -NoProfile -File tests/measurement-coverage-benchmark.ps1 -Mode Runtime `
  -Candidate <candidate-d3d9.dll> -Probe <MeasurementRuntimeTests.exe> `
  -OutputDirectory <new-output-directory>
```

Backend mode compares inactive old/new DLLs, inactive/active collection on the
same candidate, and inactive/inactive A/A. Runtime mode compares the existing
main sampling/audit flow with the full new telemetry, CS and context coverage,
plus an A/A control. Timed phase rows and whole-capture sample/event counters
have separate scopes. Inspect each process and A/A dispersion before claiming
a measurable change; report rendering/symbol resolution is outside the timed
frame phases. Whole-capture work/Present means and maxima, the completion
callback, the observed CPU-finishing gap and post-completion report wait are
recorded separately; these overlapping wall-time measures are not additive.
Cycle normalization reads `sampledPresents` from the same source report, and
zero thread-CPU accounting values remain in the paired data. Run these serially
without builds or other GPU tests in parallel.

For a version comparison of bounded event storage, build the same native fixture
source against old and new `BridgeMeasurementRuntime.cpp`, freeze both executables,
and use `tests/tail-mapping-benchmark.py --kind runtime` with the same backend DLL.
It runs fresh-process AB/BA and A/A, records file hashes and rejects failed work or
incomplete candidate captures. The isolated storage cases disable CPU sampling and
journal attribution intentionally; full F8 phase overhead is measured separately
with both versions' coverage enabled. Report completion time is separate from the
resource-work/Present/GPU-drain timing. Start-Present and completion-interrupt costs
are also recorded; `totalMainNs` and `totalMainThreadCycles` include both callbacks
and the resource-work window. The high-volume old control is expected to
truncate; its omissions are retained in the output rather than hidden.
The default native runtime test build includes a compile-only synchronization
seam to force the empty-queue/stop interleaving. Shipping DLLs do not compile it.
Freeze timing executables with `tests/measurement-runtime-test.ps1
-StorageTestSync:$false` for both runtime versions, using matching fixture and
shared header inputs; do not benchmark the synchronized correctness build.

Texture mapping policy comparisons use `tests/tail-mapping-benchmark.py --kind
mapping` with one frozen `mapping-budget-probe.exe` and two frozen backend DLLs.
The default six workloads cover below-budget reuse, pressured hot reuse, cold
sequential input, reuse beyond the budget, create-to-first-use, and unused new
resources competing with a hot set. Each uses the same explicit 100 MiB budget
and 512 operations, with six alternating A/B pairs and three A/A pairs.

`totalNs` and `mainThreadCycles` include measured resource release and the final
GPU drain. `releaseNs` is a part of that total. Loop quantiles and
`operationMaxUnmapBurst` describe the individual update/draw/Present operations;
`maxUnmapBurst` includes final cleanup as well. `cleanupUnmapCalls` and
`cleanupUnmapNs` identify the final release/drain cost, so deferred cleanup cannot
be mistaken for a saving. Actual main-thread OS map/unmap calls are observed at
the backend's existing import boundary, without active telemetry in the timed pass.

A separate untimed pass repeats the same resource lifecycle and GPU pixel check
to measure logical allocator residency through existing telemetry records. These
are logical source-allocation bytes, not total GPU memory or summed OS view sizes.
`mappedEndBytes` is before final release; `mappedAfterReleaseBytes` must be zero.
The timing pass pins only its main thread to the lowest permitted CPU, which is
reported in `affinityScope`; correctness tests use normal affinity. Run all timing
processes serially, and assess the measured A/A spread with each control workload.

StateRing diagnostics use a separate on-demand F10 capture. With
`[ProperShadersStateRing] Enable=1`, normal play no longer takes full StateRing
device snapshots. Press its configured key once to collect about one second of
detailed transitions; the next owner polling point after expiry writes
`scripts/BridgeD3D9.propershaders-state-ring.log`. Holding or pressing again
during collection does not extend it. This replaces the former immediate dump
of continuously collected history. Device reset/release or reconfiguration
cancels collection. The Fading guard still runs its required state correction
while diagnostics are idle, and F7/F8 collection is unchanged.
Admission stops at the deadline even if another Present is delayed. A guard
record completed after that deadline is discarded; an empty dump can therefore
mean that no complete guard record fit inside the window, rather than no state
changes occurring.
# Mapping retirement and CS sequence correlation

The optional telemetry stream also records `mapping-retire` (12), `cs-chunk`
(13), `command-allocate` (14), `command-begin` (15), and `command-reset` (16).
No interface layout or vtable changes. Command spans time the actual Vulkan
entry point, including its return status. CS chunks identify their queue
sequence; flag `0x800` means high priority, whose unsynchronized sequence can be
zero. Flag `0x1000` identifies secondary command buffers. CS chunk completion
includes reference cleanup after sequence publication; clip intervals to each
wait before attributing time. Filtered, unfinished or dropped chunks cannot be
inferred from a CPU sample.

`mapping-retire` is enqueue-to-completion latency, not time blocked on the main
thread. Its `0x400` flag also marks logical ownership removal in `unmap` records;
the separate OS unmap span runs on the worker that actually makes the call.
Queued retirements own in-flight recorder tokens so the capture tail cannot
appear empty while cleanup is still queued. Pending mapped bytes remain charged
to the texture budget. Pending backing files are drained before committing a
replacement file. Live texture eviction, Vulkan staging and copies are unchanged.

Final-release work uses one device-owned worker only when the mapped working set
is at least 8 MiB or a burst is already queued. The queue has at most 128 entries,
128 MiB of mapped views and 128 MiB of backing files; these are separate limits
on already-owned resources. Worker startup failure and oversized retirements
fall back to synchronous cleanup. Small create/release loops remain synchronous.

Run `python tools/analyze-cs-waits.py REPORT --output RESULT.json` to join waits
to CS chunks and nested command-buffer spans. It uses clipped interval unions,
preserves unexplained time, and outputs the source hash and capture metadata.
The duration fields are wall time, not CPU cycle measurements or pure GPU time.
For mapping A/B, `releaseSubmitNs` measures the calling thread and `releaseNs`
waits for the last real backing-file close. This completion barrier and the GPU
drain remain inside `totalNs`; the small barrier resource is created after the
timed work loop, so it cannot change allocation reuse during that loop.
