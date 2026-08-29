# Known Audit Findings

This file separates evidence from assumptions for the `v0.1.0-alpha.1`
release. It is intentionally conservative: a passing source or probe test is
not evidence that every GTA mod combination is stable.

## Verified by the Local Release Gate

- Clean x86 Release builds complete for the Bridge and DXVK backend.
- The Bridge and merged DXVK PE images are `IMAGE_FILE_MACHINE_I386` / PE32.
- Required Bridge and DXVK export sets pass verification.
- DXVK upstream provenance resolves to official v3.0.1 and the recorded
  dependency commits.
- API/ABI layout, runtime compatibility, state-journal, batching, and export
  probes pass.
- The staged split package contains the two runtime DLL paths, configuration,
  documentation, manifest, and required license files.
- The package ZIP contents match their staging directories byte-for-byte.
- The source manifest matches the current Git file set and records SHA-256
  hashes and toolchain metadata.
- The release gate does not modify the protected game-root Bridge/backend
  rollback files.

## Tests That Need External Inputs

The most recent automated run records these optional entries as skipped:

- `live-dxvk-audit`: no external DXVK source path was supplied.
- `live-bridge-config-audit`: no external Bridge and active-config paths were
  supplied.

The `build-refresh` entry is an intentional no-op when the existing Task 6
build metadata and hashes are already valid. It is not a failed test.

These skips do not invalidate the source release gate, but they mean that the
final candidate still needs the fixed game workload and manual observation.

## Runtime Behavior Without a Release Guarantee

The following areas are outside what the automated suite can prove and must
not be advertised as fixed by this alpha:

- FPS or frame-time improvement in every outdoor scene, mod loadout, or
  resolution.
- Absence of texture LOD changes, streaming stalls, black materials, tree
  alpha artifacts, wire/shadow flashes, or world-loading gaps.
- Full stability during combat, knockdown animations, save loading, and long
  sessions with every loader and limit-adjuster combination.
- Correct behavior of every ProperShaders preset and extension resource.
- Coexistence with an installed ReShade, ENB, or unrelated D3D9 proxy.
- Compatibility with arbitrary ASI/CLEO modules that intercept the same D3D9
  lifecycle or modify GTA's memory directly.

If any of these fail in the fixed workload, keep the exact logs and either
block the alpha or add the reproducible case to a later issue. Do not solve a
runtime failure by silently changing the release profile after the manifest
has been generated.

## Known Scope Limits

- The release is a two-DLL split profile, not the planned single-DLL runtime.
- ReShade and ENB are optional external components and are not redistributed.
- FLA++ and OLA are external limit-adjuster components; this repository does
  not replace their configuration or claim ownership of their pools.
- SDK and symbol archives are development aids, not runtime dependencies.
- The package is an experimental alpha for controlled testing, not a stable
  universal mod-loader replacement.

## Required Manual Workload

Before publishing the formal tag, test the exact split ZIP in a copied game
installation and record the result for:

1. Menu to save-game loading.
2. Indoor/outdoor transitions and fast driving through streamed areas.
3. Tree leaves, wires, shadows, distant LODs, water, smoke, and HUD.
4. Combat, knockdown animations, and repeated effects.
5. F7/F8 diagnostics and clean diagnostic shutdown.
6. At least 20 minutes of continuous play and a normal exit.

The release note must retain the alpha wording and the no-performance-claim
qualification until this workload has an actual record.
