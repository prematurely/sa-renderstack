# 2026-11-09 source candidate

This snapshot collects the D3D9 correctness, resource lifetime, measurement and
CPU submission work developed since the previous published main branch. It is
a source/build candidate, not a claim of improved GTA frame rate.

The changes cover state-block and journal restoration, shader-constant capture
and submission, texture/sampler binding and ownership, the x86 Fading entry
repair, Bridge lifecycle and patch transactions, staged F7/F8 diagnostics with
HUD/thread attribution and symbol sidecars, and streaming configuration
preparation. The latest batch protects active texture upload source mappings
and avoids repeating full preflight/logging for an unchanged rejected patch.

The public D3D9/API interfaces and Vulkan staging/copy path are retained. Test
fixtures and benchmark tools are source-only; no game installation, dump,
private shader package, local binary or Python cache is included.

## Validation status

- The 2026-11-08 full local pipeline finished with 82 passed, 0 failed and two
  optional external-audit skips. The latest native upload/retry fixtures ran
  4,127 checks with zero failures, including pixel sampling and lifecycle checks.
- The pre-publication 2026-11-09 local run finished with 77 passed, 5 failed and
  two optional skips. All five failures occurred while LLVM-MinGW compiled
  fixtures: thread-journal-native, upload-mapping-pressure, patch-retry-native,
  measurement-backend-unavailable and measurement-hud-render. Standard Windows
  and libc++ headers reported inconsistent preprocessing errors; two invocations
  also reported Clang frontend access violations. These failures are retained
  and are not counted as successful runtime tests. The local cause is under
  investigation; no test retry or skip was added to hide it.
- GitHub Windows CI builds from a clean checkout, runs its existing hosted-runner
  checks, verifies exports and package layout, and publishes split/SDK/symbol
  artifacts. Its existing GPU/environment-sensitive skips do not replace local
  GPU behavior tests. The PR check status identifies the actual cloud result.

See [the upload/retry report](upload-state-20261108.md) and
[raw paired measurements](evidence/2026-11-08-upload-state/) for the latest
performance evidence. Pressured mip-chain uploads reduced main-thread cycles
about 7%; rejected-patch polling also reduced CPU cycles. Single-mip controls
and empty-Present wall time include regressions. Whole-Present throughput and
same-route game frame-time acceptance remain open.

No game-directory deployment or new batching/multicore implementation is part
of this publication. Future batching work must first account for the existing
DXVK command-stream worker and preserve state observation, lifetime, ordering
and memory-budget behavior.
