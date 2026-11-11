# Review fixes: native validation and paired measurements

See [the validation report](../../review-fixes-20261110.md) for the three fixes,
real RED/GREEN regressions, HUD captures, method and interpretation.

- `validation.json`: full local run of `f6876b2f` — 85 passed, zero failed,
  two missing-external-input skips; all 87 per-gate logs have verified hashes.
- `sampler-ab.json`: final frozen candidate vs the previously installed sampler
  candidate, 12 independent processes × four alternating rounds, 1,536 samples.
- `sampler-aa.json`: six processes × four rounds against the same baseline.
- `present-first.json` and `present-final.json`: preserve both passes, including
  the noisy controls. Each mode has six A/B pairs and three A/A pairs; each
  process executes 128 warmup and 2,000 timed Presents on a real multithreaded
  device. Observer mode also verifies every native callback reenters GetAudit.

Binary/probe identities and individual process/round measurements are retained.
Negative percentage changes mean reduced time/cycles. Thread cycles are not
game FPS. Benchmark/runtime scheduling is unchanged; only compiler processes
used the local Clang affinity workaround. Hosted CI remains a separate build
with its existing GPU/environment-sensitive skip policy.
