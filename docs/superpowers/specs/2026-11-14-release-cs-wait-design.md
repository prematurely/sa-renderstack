# Concentrated mapping release and CS wait attribution

The previous candidate keeps the hot mappings resident. The final 98 views have
25,088 resident pages, versus 863 with the installed baseline. An untimed
QueryWorkingSetEx pass measured 4.7785 ms versus 0.4950 ms in UnmapViewOfFile.
The previously accepted repeated benchmark reported 12.3047 versus 7.5314 ms
for the complete release phase. Retain the measured main-thread improvements.

First experiment: one bounded worker for final-release OS unmapping. Budget
evictions stay synchronous. Prototype at the actual OS import in the real-device
probe before changing product code. Include the worker drain inside release and
total timers. Reject a change which only moves cleanup beyond measurement.
Keep the resident-page query in a separate untimed observation.

If the experiment passes, integrate only unreferenced final-release views. No
more than 128 jobs, 128 MiB of views or 128 MiB of backing files may be pending.
These are already-owned resources, and the two limits apply separately rather
than double-charging the same pages. Only mapped working sets of at least eight
MiB, or the remainder of an existing pending burst, use the worker: the initial
unconditional prototype regressed the real create/release control by 13%.
Pending bytes remain accounted
until OS unmapping completes; allocation/budget pressure drains pending cleanup
before evicting useful hot mappings. Shared-page references, recycled ranges,
thread-start failure, device reset/destruction and telemetry lifetime must remain
correct. New backing storage drains pending files first; failed maps with pending
views may drain and retry once. No additional DLL dependency. The worker must not
run on the CS queue. Report submit latency and full release completion separately;
the Windows teardown work is still present and included in the lifetime timer.

Second, resolve the diagnostic gap around the 15.538 ms LockRect CS wait. The
existing capture proves a wait on a sequence and contains one command-buffer
allocation sample, but cannot attribute the whole wait to that sample. Add
capture-only CS chunk/sequence and Vulkan command-buffer boundary spans as needed
and verify their correlation against a real-device controlled wait. Never remove
a resource dependency based only on the sampled stack.

Preserve Vulkan staging/copy, image quality, public interfaces, configured memory
budget and existing HUD placement. Run real pixels/lifetime/failure regressions,
fixed-binary alternating A/B and A/A, complete local validation and final review.
No game deployment, game capture, commit or push in this batch.
