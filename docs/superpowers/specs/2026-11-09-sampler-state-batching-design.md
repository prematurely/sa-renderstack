# Generic sampler submission cost

The user authorizes local implementation, native behavior regressions, repeated A/B and complete validation. Do not deploy, push, merge or change the game configuration. Preserve image quality, public interfaces and Vulkan staging/copy/upload behavior. No MOD names or module signatures participate in this optimization.

The latest capture has about 44,698 sampler calls per attributed outdoor frame, 98.20% redundant, while the main thread is busy and the CS queue is mostly idle. This establishes a producer-cost target, not a promise of game FPS improvement. The capture includes focus loss and cannot serve as a controlled A/B.

## Approaches

1. **Selected:** split the sampler setter's ordinary unchanged path from recording/auditing/change handling; reuse its locked internal entry from an already validated effect batch and StateBlock application. The authoritative device state remains the only comparison source. All reads stay under the existing device lock when multithread protection is enabled.
2. A Bridge-side sampler mirror could avoid crossing the backend boundary, but raw-device calls, StateBlocks, Reset and external clients would require new invalidation rules. It is unnecessary for this bounded iteration.
3. Delayed command queues or last-write-wins collapsing across setters could affect observation and FETCH4 state transitions. Existing deferred GPU binding remains unchanged.

## Behavior

- Keep public sampler-index remapping and return values. The existing effect-batch prevalidation must still reject the complete batch before mutation.
- BeginStateBlock must record even a value identical to live state. During recording, setters must not mutate live state.
- Journal captures the original value on its first actual change, including overlapping individual/batched writes and StateBlock application. Restore and Reset retain their existing behavior.
- Active F8 state attribution counts and times redundant calls as before; do not hide traffic to improve the diagnostic numbers.
- Preserve every changed sampler entry in input order, especially MIPMAPLODBIAS/FETCH4 and MAGFILTER interactions. No new queue, resource lifetime or memory-budget behavior.
- Keep the lock-owning entry distinct from the internal entry whose callers already own that lock. No public COM method or SDK structure is added or changed.

## Evidence

Build and freeze the baseline from merged main with the same local toolchain as the candidate. Test through real x86 D3D9/Vulkan devices without game MODs: all 21 sampler slots, raw getter values, same-value recording, original-value journal capture, batch rejection, diagnostic counts, protected-device batch atomicity, Reset and pixel readback. The original implementation should pass behavioral characterization because this is a behavior-preserving optimization. An intentionally unsafe shortcut must fail the recording regression; do not fabricate an old-version correctness bug or add timing thresholds to CI.

Compare ordinary/redundant and changed setters, sampler batches, StateBlock restore, and draw controls. Use repeated alternating A/B plus A/A calibration, retain raw wall-clock and main-thread-cycle measurements and final binary hashes, and do not accept a repeatable control regression without resolving it. Run the full local pipeline including GPU probes. Save candidate artifacts in out, verify game hashes unchanged, and report remaining uncertainty separately from measured fixture gains.
