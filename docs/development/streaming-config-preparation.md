# Offline Improved Streaming interval preparation

The first streaming batch corrects a configuration consumer mismatch. It does
not change either RenderStack DLL, create streaming threads/hooks, alter memory
budgets, or apply files to the game installation.

The audited `ImprovedStreaming.SA.asi` v1.1 (SHA256
`D7BD005EDE20F00B358B75C4D4844618024F2A74AAF60957553EBD964BEE527C`)
reads `Settings.RemoveUnusedInterval`, default `60`, at RVA `0x73E9`.
`RemoveUnusedIntervalMs=750` therefore leaves the default in effect. This is
evidence for that binary, not a compatibility promise for every plugin release.

Run from the checkout using PowerShell 7:

```powershell
pwsh -NoProfile -File tools/prepare-streaming-config.ps1 `
  -Source '<read-only-path-to-ImprovedStreaming.ini>' `
  -OutputDirectory '<absolute-checkout-path>/out/candidates/streaming-interval'
```

The destination must be a new directory under this checkout's `out/`. The tool
emits a full `ImprovedStreaming.ini` and `preparation.json` with input/output
hashes, the preserved interval, and `applied=false`. It changes only the exact
key in `[Settings]`; comments, other sections/keys, the value, UTF-8 BOM, and LF
or CRLF bytes are preserved. A canonical key is copied unchanged. Missing,
duplicate, conflicting, malformed, or unsupported-encoding inputs are rejected.
No apply/deployment option exists. Keep both the source and preparation record
for review; this command does not authorize game installation.

The value is preserved because this batch repairs the requested policy rather
than selecting a new policy. A longer reclaim interval can retain resources
longer during streaming pressure. Actual churn, loading waits and resource
residency still require controlled game measurements before rollout; offline
acceptance alone does not establish a net FPS gain.

## Regression and evidence limits

```powershell
pwsh -NoProfile -File tests/streaming-config-regression-test.ps1
pwsh -NoProfile -File tools/test.ps1 -BridgeReference '<audited-reference-d3d9.dll>'
```

The first command compiles a native x86 test executable. It reads real fixture
files with the unmodified upstream INI reader, and executes the installed v1.1
interval-decision instruction fragment relocated into test-owned memory.
The ASI is never loaded, its DllMain never runs, and GTA is never started.
Input preservation, failure behavior, interval boundaries, timer wrap and RAM
bypass are checked. The full test orchestrator runs this gate as required.
Upstream hashes, license and the exact fragment/relocations are documented in
`tests/vendor/loadwholemap-ini/README.md`.

The repeated A/B uses 12 alternating AB/BA rounds, a simulated 10-second clock,
16-ms gate visits and sustained outer pressure. The native decision returns
156 permits for 60 ms and 13 for 750 ms. With RAM above the supplied cap, both
return 625 permits. These counts are conditional decision-fragment results;
they do not model the outer callback cadence, eviction success, I/O,
decompression, RenderWare conversion, GPU upload or game frame time. The public
PC plugin source identifies itself as v1.0, so it is not used as a full v1.1
policy replacement.
