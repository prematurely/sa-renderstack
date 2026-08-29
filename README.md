# SA RenderStack

SA RenderStack is a GTA San Andreas rendering runtime built around a modular
D3D9 Bridge and a GTA-compatible DXVK Vulkan backend. It provides a single
root D3D9 entry point for the current profile, ordered proxy/plugin metadata,
native compatibility APIs, and diagnostics for investigating rendering and
frame-time behavior.

The current release is an **x86 split-DLL alpha**. It is a real, installable
runtime, but it is not a universal replacement for every GTA mod combination.

## Current Release

- Release: [v0.1.0-alpha.1](https://github.com/prematurely/sa-renderstack/releases/tag/v0.1.0-alpha.1)
- Game: GTA San Andreas 1.0.0.0 US, 32-bit
- Bridge: Win32 x86, MSVC Release
- Backend: DXVK v3.0.1, LLVM-MinGW x86
- Renderer: Vulkan through DXVK D3D9
- Deployment: two DLLs
- DXGI: merged into the D3D9 backend for this profile

The release is marked pre-release because third-party proxy combinations,
long-session stability, and scene-independent performance still require
workload-specific validation.

## Quick Start

The runtime package does not depend on PowerShell or the source repository.

1. Download `SA-RenderStack-v0.1.0-alpha.1-split.zip` from the release page.
2. Close GTA San Andreas and all loader or injector processes.
3. Back up any existing `d3d9.dll`, `dxvk.conf`,
   `scripts\BridgeD3D9.ini`, and `backend\dxvk-gta\d3d9.dll`.
4. Extract the ZIP directly into the GTA game root and preserve its folders.
5. Start the game and first verify menu-to-save loading before testing a
   larger mod workload.

The complete manual procedure, expected paths, hash verification, and
rollback steps are in [docs/installation.md](docs/installation.md). The SDK
and symbols archives are for development and diagnosis; they are not runtime
dependencies.

## Runtime Layout

```text
<GTA root>\
  gta_sa.exe
  d3d9.dll                         # SA RenderStack Bridge proxy
  SA.RenderStack.ini               # Bridge and module registry
  dxvk.conf                        # DXVK and GTA compatibility options
  backend\dxvk-gta\d3d9.dll       # x86 DXVK D3D9/Vulkan backend
  scripts\BridgeD3D9.ini          # legacy configuration alias
```

The archive also includes `manifest.json`, `docs\INSTALL.md`,
`docs\README.md`, `docs\LICENSE-SA-RENDERSTACK`, `docs\LICENSE-DXVK`, and
`docs\THIRD_PARTY_NOTICES.md`. These files do not participate in the D3D9 load
chain.

## Architecture

```text
gta_sa.exe
    |
    v
root d3d9.dll (Bridge)
    |  ordered registry, diagnostics, adapters, compatibility API
    v
backend\dxvk-gta\d3d9.dll (DXVK)
    |  D3D9 implementation, GTA overlay, Vulkan device and queue
    v
Vulkan driver and GPU
```

The Bridge selects the backend through `[Backend]` in
`SA.RenderStack.ini`:

```ini
[Backend]
Backend=DXVK
DxvkBackendDir=backend\dxvk-gta
```

The backend owns the Vulkan device and the existing DXVK command submission.
Bridge-native Vulkan passes are recorded into that submission and must not
flush, submit, end, reset, or lock the backend command buffer themselves.
Passes are ordered by priority and then registration order.

More detail is available in
[docs/architecture/module-map.md](docs/architecture/module-map.md).

## Proxy Registry

The Bridge uses an ordered registry rather than implicitly loading every
possible D3D9 wrapper:

```ini
[ProxyChain]
Enable=1

[register]
1=ProperShaders
2=ReShade
3=ENB
```

Each registered component has its own section with a type, `dll=` or `asi=`
path, lifecycle stage, claimed hooks, and conflict policy. Relative paths are
resolved below the GTA root. Missing optional components are skipped. A later
component that claims an already-owned hook is skipped when its policy is
`SkipIfClaimed`.

The shipped profile observes the installed ProperShaders ASI and leaves
ReShade and ENB disabled unless they are installed and explicitly enabled.
The registry does not download or manufacture absent third-party modules.

## Configuration

`SA.RenderStack.ini` controls the Bridge and its diagnostic/adaptation layers.
`dxvk.conf` controls DXVK and the GTA compatibility overlay. The current
profile includes these categories:

- DXVK backend selection and frame pacing
- x86 affinity and startup priority handling
- ordered proxy and ASI metadata
- ProperShaders incremental state journal integration
- D3D9 state-block prefiltering and fast-skip experiments
- shader, scalar, input-layout, push-data, sampler, and texture coalescing
- resource binding cache
- DXVK HUD and per-session diagnostic output

Experimental options that can change rendering semantics are kept disabled in
the release profile unless their tests and workload evidence justify enabling
them. Do not copy benchmark settings into a release profile without
regenerating the package manifest and rerunning the release gate.

## Diagnostics

The normal profile keeps diagnostics lightweight:

- DXVK sessions: `Diagnostics\DXVK\<timestamp>-pid<id>\`
- Bridge aggregate log: `scripts\BridgeD3D9.log`
- F7 state attribution: `scripts\BridgeD3D9.state-attribution.log`
- F8 CPU hotspot capture: `scripts\BridgeD3D9.cpuhotspots.log` and
  `Diagnostics\CPU\`
- F9 callsite profile: `scripts\BridgeD3D9.callsites.log` when enabled
- F10 draw trace: `scripts\BridgeD3D9.drawtrace.log` when enabled

F7 is a read-only but high-overhead capture and is not a valid normal FPS
benchmark. F8 samples the GTA render/main thread for the configured window;
if its log timestamp does not change, the trigger was not received and the
capture did not run.

The release DXVK HUD is `fps,frametimes`. Shader dumping, full trace output,
and detailed statistics should be enabled only for a deliberate diagnostic
run because they alter the workload being measured.

## Build From Source

The source build is Windows-only for this release and produces x86 binaries.
Required tools are:

- PowerShell 7 (`pwsh`)
- Visual Studio MSBuild with C++ support
- Python 3
- Meson and Ninja
- LLVM-MinGW with the i686 compiler and binutils
- `glslangValidator`

From the repository root:

```powershell
pwsh -NoProfile -File tools/build.ps1 `
  -Configuration Release -Architecture x86 -Component All -Clean

pwsh -NoProfile -File tools/test.ps1 `
  -Configuration Release -Architecture x86
```

The build writes only to `out/`. It does not deploy files to a game
installation. Toolchain paths can be supplied explicitly; see `-Help` on the
build and test scripts.

## Test And Package

The complete release candidate sequence is:

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

The gate checks source hygiene, provenance, clean build output, required
tests, ABI and export sets, package layout, artifact hashes, and protected
game-root rollback hashes. A release candidate is valid only when the report
ends with `Verdict: PASS`.

## GitHub Actions

The repository includes a custom Windows workflow at
`.github/workflows/windows-ci.yml`. It runs on pull requests, pushes to
`main`, and manual dispatches. The workflow bootstraps the pinned x86
LLVM-MinGW toolchain, uses the hosted Visual Studio MSBuild installation, and
the action-provided MSYS2 path, and executes the same build, test, package,
and package-layout stages used by the local workflow.

Hosted CI passes the explicit `-SkipLocalBridgeEvidence` option because a
runner does not contain the user's pre-installation GTA Bridge reference. The
two affected evidence checks are recorded as non-required skips; local runs
and the release gate keep the original game-root checks.

Hosted runners also do not provide the Vulkan device and local fixture paths
needed by the runtime-only probes. CI therefore passes
`-SkipGpuRuntimeProbes` and `-SkipEnvironmentSensitiveBridgeTests`; every
skipped check is written to `out/test-results.json` with `required=false` and a
reason. The pure Meson, ABI, source, adapter-policy, packaging, and export
checks remain required. The workflow checks out full Git history because the
historical provenance tests compare against pinned commits. It passes the
action-resolved Ninja and `glslangValidator` paths through every tool-discovery
layer, and the backend API source test has a PowerShell fallback when
`ripgrep` is unavailable.

Each run uploads commit-scoped artifacts containing the split, SDK, and
symbols archives, the source manifest, build metadata, test results, and
diagnostic logs. CI does not install the runtime into GTA San Andreas, does
not inspect the local game directory, and does not create tags or GitHub
Releases. The local release gate remains necessary for protected game-root
rollback checks and final in-game workload validation.

## Why The DXVK DLL Is Not 20 MiB

A full upstream DXVK distribution is a bundle, not one universal DLL. It can
contain multiple API targets (`d3d8`, `d3d9`, `d3d10`, `d3d11`, and `dxgi`),
both x86 and x64 builds, debug symbols, tools, and supporting files.

This profile intentionally builds only:

```text
enable_d3d8=false
enable_d3d9=true
enable_d3d10=false
enable_d3d11=false
enable_dxgi=false
merge_dxgi_into_d3d9=true
```

Therefore the release backend is approximately 7.7 MiB uncompressed, while
the Bridge is approximately 1.0 MiB. The split ZIP is smaller because it is
compressed. This is expected for the selected x86 D3D9-only target and does
not mean the Vulkan backend was omitted.

## Compatibility And Scope

Supported baseline:

- GTA San Andreas 1.0.0.0 US, 32-bit
- x86 D3D9 entry through the root Bridge
- DXVK v3.0.1 with the audited SA RenderStack compatibility overlay
- ProperShaders observation and API v7 native compatibility path

Not bundled or universally validated:

- ReShade and ENB
- FLA++, OLA, Project2DFX, Urbanize, and other third-party mods
- arbitrary D3D9 proxy chains
- the planned single-DLL runtime

This alpha makes no universal claim about FPS, frame pacing, texture
streaming, distant LODs, shader appearance, or long-session stability. Those
properties must be recorded against a fixed workload and exact mod set.

Known findings and release test requirements are maintained in
[docs/development/known-audit-findings.md](docs/development/known-audit-findings.md).

## Repository Map

```text
backend/dxvk/                 DXVK source and GTA compatibility overlay
config/                       Release runtime configuration
docs/architecture/            Module and ownership documentation
docs/development/             Maintainer handoff and audit records
docs/releases/                Release notes
packaging/split/              Runtime package contract
sdk/                          Public backend API headers
src/bridge/legacy/            Bridge, adapters, registry, and probes
tests/                         Source, package, ABI, and regression tests
tools/                         Build, test, package, and release automation
```

## Provenance And Licenses

The backend is based on official [DXVK v3.0.1](https://github.com/doitsujin/dxvk/tree/v3.0.1).
Pinned upstream and dependency commits are recorded in
`backend/dxvk/SA_RENDERSTACK_UPSTREAM.toml` and
`backend/dxvk/SA_RENDERSTACK_DEPENDENCIES.toml`.

The root `LICENSE` covers only SA RenderStack-specific code. Vendored DXVK and
its dependencies retain their own license files, indexed by
`THIRD_PARTY_NOTICES.md` and copied into the binary package's `docs/` directory.
The source manifest records the exact source file hashes, dependency
provenance, build options, and toolchain metadata for each generated release
candidate.

## Further Reading

- [Installation and rollback](docs/installation.md)
- [Architecture and module ownership](docs/architecture/module-map.md)
- [Maintainer handoff](docs/development/phase-1-handoff.md)
- [Known audit findings](docs/development/known-audit-findings.md)
- [Alpha release notes](docs/releases/0.1.0-alpha.1.md)
