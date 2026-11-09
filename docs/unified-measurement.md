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

The candidate is validated with native D3D9/Vulkan fixtures, symbol fixtures,
repeated serial measurement-overhead benchmarks, and the repository pipeline.
Native HUD captures verify the renderer with fixture text. Game appearance,
actual Grove Street frame times and indoor/outdoor behavior require a separate
manual game run after deployment is authorized.

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
