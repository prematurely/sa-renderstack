# Upload/retry candidate measurements

These CSV/JSON files preserve the paired measurements described in
[the report](../../upload-state-20261108.md). Negative percentage changes mean
less work/time. CPU cycles are not milliseconds or game FPS.

- `cycles-*`: first cycle-instrumented pass of the final implementation.
- `validated-*`: binaries rebuilt by the complete local validation pipeline.
- `waits-pinned-*` / `waits-normal-*`: follow-up empty-Present diagnosis with
  fixed or ordinary calling-thread affinity; no game affinity was changed.
- Each profile has six A/B and three A/A pairs; AB and BA order alternate.
- Upload timing uses 512 iterations after 64 warmups. Retry timing uses 2,000
  Present calls after 16 warmups. Raw stdout/configuration copies remain in the
  local `out/perf/20261108-upload-state` evidence directory.
- `package-manifest.json` records the measured local candidate binaries and
  matching symbol files. CI rebuild hashes may differ.
- `import-dependencies.json` compares static runtime DLL imports only.

The earlier superseded implementation and its timings are excluded here.
The final single-mip and empty-Present regressions are included. These records
do not constitute game-route or whole-Present performance acceptance.
