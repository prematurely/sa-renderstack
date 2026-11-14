# F8 measurement coverage design

The user authorized implementation after the Grove Street capture exposed missing memory history, CS stacks and per-frame wait attribution. This document records the implementation choices within that scope; there is no deployment, commit or publication step.

## Requirements

- Preserve rendering, public API1–7 and existing diagnostic/lifecycle vtables; retain Vulkan staging and copies and the bottom-left HUD layout.
- Capture memory chunk changes (the data behind the bottom-right HUD), allocation/free and map/unmap duration, CS call stacks, resource/CS/GPU waits, and correlate timestamps with successful-Present intervals and slow frames.
- Record foreground/minimized transitions and render-reuse/freeze evidence. Foreground loss or low draw counts must not be presented as proof of pixel-identical output. Distinguish observed signals from inferred freeze candidates.
- Correct the CPU-running HUD text; main and CS sampling must be separately labelled with missed/dropped/chain quality and direct suspension overhead.
- Remain self-contained, without MOD-name checks, external collectors, extra runtime DLLs or symbol servers.
- Regress real device behavior, lifecycle/concurrency and pixels; repeat paired A/B plus A/A controls; run the full local validation pipeline. Do not deploy.

## Approach and alternatives

Use bounded in-process events plus low-frequency snapshots and post-capture correlation. This fits the existing F8 runtime and can work without administrator tracing. Always-on detailed tracing and per-frame full allocator scans are rejected because they add overhead outside the requested diagnostic window. GPU readback to detect frozen pixels is rejected because it adds synchronization and perturbs the workload; the report will identify render-reuse candidates explicitly.

## Boundary and data

Add one optional private COM interface, `ID3D9BridgeTelemetry`, with a distinct GUID, fixed-width copied-data structs, validated StructSize/Version and bounded caller capacities. Existing interfaces retain their ABI. Methods: StartCapture, StopCapture, ReadEvents and GetMemorySnapshot. Start/Stop identify the capture; old or mismatched capture IDs must not consume new data. Unsupported backends remain usable and report unavailable telemetry.

Backend events carry capture ID, event kind, begin/end QPC nanoseconds, producer thread ID, opaque resource/allocation identity, byte count, outcome and flags. Timing scopes are inclusive and may nest. Buffers are bounded; dropped and filtered records are explicit. Disabled fast paths perform no clock query, allocation, file I/O or symbol work. A recorder lock must never be held over a Vulkan/OS resource operation or wait, or while acquiring a device/allocator lock. Concurrent late operations cannot write into a new capture generation.

Record actual allocator operations, file-backed memory alloc/free/map/unmap and actual CS/GPU wait spans. Resource details below a documented duration/size threshold may be aggregated or filtered, with counts; failure and large/slow events remain eligible. Never silently claim every allocation was traced.

Snapshots use the same allocator state as the memory HUD, recording heap/type, chunk cookie, capacity, used bytes, mapped/active state and bounded occupancy masks where supported. They include timestamps, scan overhead, totals versus copied counts and truncation flags. Default cadence is 500 ms during F8, plus capture boundaries. No snapshots outside an active capture. Bounds and cadence are explicit report metadata.

## Runtime

The main thread remains sampled at the configured cadence (default 5 ms); CS uses the measured CS thread ID and a lower default cadence (10 ms). Never suspend both at once. Copy preallocated context/stack data while suspended, resume before walking, allocating, resolving symbols or touching runtime/backend locks. Preserve worker cancellation, capture identity and Reset/final-release guarantees. CS failure is separately reported and must not discard valid main samples.

Telemetry spans and both sample streams join to the same frame QPC intervals after collection; cross-frame events list overlap rather than being charged entirely to one frame. Do not add inclusive timing categories as if they were exclusive frame components.

Foreground/minimized state is observed from the actual device window/process; game-frame progress can use the existing safe readFrame callback. Mark unchanged game-frame or present-without-render work as explicit freeze/reuse evidence with confidence/source. No dependence on Proper Shaders internals or log parsing. Preserve raw frames and provide counts/ranges excluded from steady-state interpretation.

## Validation

Real GPU tests create/use/release buffers/textures, force real host-visible/file mappings, issue resource and CS synchronization, compare copied/rendered pixels, and verify event timestamps/identities and chunk changes. Include capture restart, overflow, missing interface, invalid arguments, worker shutdown, secondary devices, Reset/final release and slow-frame cross-boundary correlation.

Runtime tests verify the actual report and actual HUD published during CPU sampling, separate CS samples, foreground/freeze evidence and clean teardown. Benchmark the identical workload with collection off/on and A/A, alternate order across independent processes, and retain per-stage elapsed/cycle, recorded/filtered/dropped events and suspension overhead. Capture-off throughput must stay within measured noise; active costs are reported, not converted to a GTA FPS claim. Full validation includes exports/imports, source provenance/overlay, real device regressions and HUD pixel layout. Installed eight-file hashes remain unchanged.
