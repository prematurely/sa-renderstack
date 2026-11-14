# Texture initialization and LockRect synchronization cost

The user requested optimization of the two measured paths in capture 66984-1.
Implement within the existing authorized scope: generic behavior, unchanged
quality/public interfaces, preserved Vulkan staging and copies, real regression
tests, repeated A/B plus A/A, full local validation. No deployment, game run,
commit or push. Implementation choices below are engineering hypotheses, not
claims that the observed 114.96 ms or 15.54 ms intervals will disappear.

## Evidence and alternatives

The 114.96 ms frame has 12/20 main samples through packImageData and 13.40 ms
OS unmapping. A second 55.08 ms frame contains InitHostVisibleTexture samples.
The next 38.28 ms frame waits 15.54 ms in LockRect/WaitForResource/CS synchronize;
concurrent CS samples include getCommandBuffer/allocateCommandBuffer and driver
calls. See out/reports/20261114-grove-first-exit-f8-66984-1/report.md.

Current initializer eagerly maps and zeros all non-default CPU texture storage.
Fresh paging-file-backed bytes are already zero. Existing reuse and 100 MiB
budget must remain. Skipping first mapping would move mapping failures to Lock;
the proposed path retains first mapping/failure behavior and only skips redundant
zero stores where the allocator proves bytes have never been handed out.
Reused or uncertain storage continues through the original clear path.

Current primary command buffers grow one at a time on CS. Evaluate a small bounded
batch (initially two, optionally four as a separate experiment), retaining pool
reset/reuse and command ordering. A batch allocation failure must retry one buffer
before preserving the original failure. This is a candidate for lowering driver
allocation frequency, not permission to omit resource waits. Reject it if repeated
real-device controls do not justify the added reserved buffers.

Alternatives not selected: larger texture budget (changes configured bounds),
unbounded mapping/buffer cache, background reclamation, skipping resource waits,
rewriting staging/copy, or eagerly flushing every readback (may create more
submissions). No MOD-name detection.

## Initialization proof and lifecycle

On 32-bit Windows, a chunk tracks an exclusive high-water mark of byte ranges
ever allocated since creation. An allocation wholly above this mark is provably
zero; advance the mark on every allocation, never on free. Holes below it are
conservatively treated as reused. Dedicated paging-file mappings are fresh.
Move/free and chunk destruction/recreation preserve/reset the proof. Allocator
metadata is protected by its existing lock. Non-Windows/64-bit paths retain
existing initialization unless a separate equivalent proof exists.

The proof describes initial allocation contents, not live contents after writes.
Only the single resource initialization site consumes it; explicit initial data
is always copied. First mapping still occurs and initialization map failure still
returns creation failure. Existing device locking, LRU admission, budget, padding,
multi-mip/face/volume offsets, upload pinning, and GPU initialization remain.
If touching allocation rollback reveals the known CreateFileMapping failure
recovery issue, fix only the ownership/rollback needed by this path and cover it
with actual fail-once/retry behavior.

## Tests and acceptance

Use the actual x86 DLL and actual D3D9/Vulkan device. Observe OS/CRT/Vulkan call
boundaries in test-only import/dispatch hooks, forwarding to real implementations.
Prove fresh sources avoid redundant zero writes, while dirty reused slices, shared
pages, partial locks, mips/cubes/volumes and explicit data preserve exact bytes and
rendered/readback pixels. Include multiple devices, MULTITHREADED, reset/release,
map/allocation failures and budget pressure.

For command pools, observe driver allocation requests and handle lifecycle through
real workload, verify reserved-buffer reuse, reset only after GPU completion,
pixel correctness, and batch-failure single fallback. Timed runs do not inject
delays or allocation failure. Capture driver-call counts/timing, LockRect and total
work/Present/drain/release time, main cycles, latency quantiles, and memory bounds.
Include cold, warm, create-but-unused, over-budget and readback controls. Compare
separate initialization-only and combined binaries; freeze all binaries before
serial benchmark runs. Retain rejected experiments and all raw observations.

The baseline is the currently installed 1e8120cc DXVK / 11f47624 Bridge, copied to
out/perf/20261114-init-map-lockwait/baseline-runtime. The initial dirty source is
archived separately; git HEAD predates both telemetry and the previous candidate.
Verify all eight installed artifacts unchanged after validation; freeze candidate
DLLs/symbol identities and document imports/dependency changes (none intended).

References: Microsoft CreateFileMappingA documents zero initial paging-file pages:
https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-createfilemappinga
Vulkan documents multi-buffer allocation and atomic failure cleanup:
https://docs.vulkan.org/refpages/latest/refpages/source/vkAllocateCommandBuffers.html
