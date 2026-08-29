# Phase 1 Maintainer Handoff

## Scope

This document describes the `v0.1.0-alpha.1` release candidate for the
`SA RenderStack` monorepo. The release target is the controlled x86 split
profile for GTA San Andreas 1.0.0.0 US. It contains two runtime DLLs:

1. The Bridge proxy installed as `d3d9.dll` in the game root.
2. The GTA-compatible DXVK backend installed as
   `backend/dxvk-gta/d3d9.dll`.

The single-DLL design remains a later experiment. ReShade, ENB, FLA++, OLA,
Project2DFX, Urbanize, and other third-party mods are not bundled by this
repository and are not part of the release compatibility guarantee.

## Repository Contract

- Repository: `prematurely/sa-renderstack`
- Release version: `0.1.0-alpha.1`
- Game architecture: Win32/x86
- Bridge toolchain: MSVC Release/Win32
- DXVK toolchain: LLVM-MinGW x86
- DXVK upstream: official `v3.0.1`
- DXGI: included in the merged D3D9 backend for this split profile
- Runtime package: `out/packages/SA-RenderStack-v0.1.0-alpha.1-split.zip`

The repository commit is intentionally read at build time and recorded in the
source manifest. Do not hard-code a commit in this document as proof of a
future release. The authoritative commit is the one printed by the release
gate and stored in the generated source manifest.

## Required Tooling

The build orchestrator discovers or accepts paths for:

- PowerShell 7 (`pwsh`)
- Visual Studio MSBuild with the C++ workload
- Python 3
- Meson and Ninja
- LLVM-MinGW with the i686 compiler and binutils
- `glslangValidator`

The local workspace may use the bundled dependency runtime and the tool paths
reported in `out/build-metadata.json`. A clean machine must provide equivalent
tools through the parameters documented by `tools/build.ps1` and
`tools/test.ps1`.

## Reproducible Release Sequence

Run all commands from the repository root:

```powershell
pwsh -NoProfile -File tools/build.ps1 `
  -Configuration Release -Architecture x86 -Component All -Clean

pwsh -NoProfile -File tools/test.ps1 `
  -Configuration Release -Architecture x86

pwsh -NoProfile -File tools/package.ps1 `
  -Version 0.1.0-alpha.1 -Configuration Release

pwsh -NoProfile -File tests/package-layout-test.ps1

pwsh -NoProfile -File tools/release-gate.ps1 `
  -Version 0.1.0-alpha.1 -Configuration Release
```

The release gate is the final local authority. It checks the clean build,
required tests, package layout, source hygiene, export sets, artifact hashes,
and the protected game-root rollback hashes. It must finish with
`Verdict: PASS`.

## Generated Artifacts

The release process writes only below `out/`:

- `out/packages/*-split.zip`: runtime files for manual installation
- `out/packages/*-sdk.zip`: public SDK headers and API material
- `out/packages/*-symbols.zip`: Bridge symbols and map files
- `out/packages/*-source-manifest.json`: source file hashes, provenance, and
  toolchain metadata
- `out/reports/phase-1-release-gate.md`: gate result and artifact hashes

The `out/` directory is ignored and must not be committed. The package and
release gate never write to the game directory. The runtime ZIP is installed
separately by the user; see `docs/installation.md`.

## Test Interpretation

The automated suite covers source layout, provenance, historical overlays,
Bridge and DXVK exports, API/ABI behavior, state-journal behavior, package
layout, and runtime probes. It does not prove that every third-party mod chain
or every GTA gameplay workload is stable.

The live DXVK audit and live Bridge/config audit are optional test entries. If
their external source/config paths are not supplied, they are recorded as
skipped rather than silently treated as passes. A release must report these
skips and complete the fixed game workload manually before claiming a tested
alpha candidate.

## Runtime Ownership

The root Bridge owns proxy selection, ordered registry metadata, adapter
observation, diagnostic triggers, and the Bridge-side compatibility API. The
DXVK backend owns the D3D9 implementation and Vulkan submission. The Bridge
must not create a second Vulkan submission for a registered native pass. See
`docs/architecture/module-map.md` for the full boundary map.

## Release and Rollback

After the final candidate has passed the fixed workload:

1. Confirm a clean tree and record the release-gate commit and hashes.
2. Create the annotated `v0.1.0-alpha.1` tag on that exact commit.
3. Push the tag to `origin`.
4. Create the GitHub Release from `docs/releases/0.1.0-alpha.1.md`.
5. Upload the split, SDK, symbols, and source-manifest assets.

Never replace the existing audited game installation during repository builds.
For a failed runtime test, stop the game and loader processes, restore the
backed-up root Bridge/backend/configuration files, and preserve the generated
diagnostics for analysis. Do not use `git reset --hard` or overwrite unrelated
working-tree changes as a rollback method.

## Maintenance Rules

- Keep the DXVK upstream and dependency commits in the provenance TOML files.
- Update export expectations and API layout tests with any public ABI change.
- Keep runtime configuration changes single-purpose and documented in the
  release notes.
- Rebuild the source manifest after every committed source or documentation
  change that affects a release.
- Do not claim an FPS, streaming, shader, or third-party compatibility result
  without a repeatable fixed-scene measurement or workload record.
