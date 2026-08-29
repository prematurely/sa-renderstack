# Windows CI Design

## Goal

Add a custom GitHub Actions workflow that validates the actual SA RenderStack
Windows x86 build, test, and package path on pull requests, pushes to `main`,
and manual runs. The workflow must be independent of the local GTA San
Andreas installation and must never publish a GitHub Release automatically.

## Scope

This change adds hosted Windows CI for the existing developer/release scripts.
It does not change the runtime DLL behavior, create a single-DLL build, run a
game session on a hosted runner, or replace the local release gate.

The local `tools/release-gate.ps1` remains the authority for a release
candidate because it checks protected rollback hashes in the user's GTA game
directory. GitHub CI has no such installation and therefore runs the build,
test, package, and package-layout stages directly, then uploads artifacts.

## Workflow

Create `.github/workflows/windows-ci.yml` with:

- `pull_request` trigger;
- `push` trigger for `main`;
- manual `workflow_dispatch` trigger;
- `windows-2025` runner;
- `contents: read` permission only;
- one active run per ref, with newer runs cancelling older runs;
- a 60-minute job timeout;
- no release, tag, permission, or game-directory write operation.

The job uses the repository's existing scripts in this order:

```text
tools/build.ps1 -Configuration Release -Architecture x86 -Component All -Clean
tools/test.ps1 -Configuration Release -Architecture x86
tools/package.ps1 -Version <VERSION> -Configuration Release
tests/package-layout-test.ps1 -Version <VERSION> -Configuration Release
```

`<VERSION>` is read from `VERSION` and passed to packaging. The workflow must
fail if the file is empty or contains a path separator. The package is an
artifact named with the commit SHA, not a release asset.

## Toolchain Bootstrap

The runner provides Windows PowerShell 7 and a Visual Studio installation with
MSBuild and C++ support. The workflow uses `microsoft/setup-msbuild` to expose MSBuild and passes an
explicit CI-only compatibility switch to the repository scripts. Local
default behavior remains strict about the Visual Studio 18 toolchain used for
the audited release baseline; CI may use the runner's supported Visual Studio
17 or 18 MSBuild after recording the actual version in build metadata.

The workflow installs Python 3.12 with `actions/setup-python`, obtains Meson
and the D3DX headers through the existing preparation scripts, and installs
Ninja and glslang through MSYS2. The workflow uses the action's reported
`msys2-location` output rather than assuming a fixed `C:\\msys64` path. It downloads the official
`mstorsjo/llvm-mingw` `20260602` `msvcrt-i686` archive, verifies its SHA-256
digest, extracts it below the workspace, and passes its `bin` directory to
`build.ps1` and `test.ps1`.

The pinned LLVM-MinGW archive is:

```text
https://github.com/mstorsjo/llvm-mingw/releases/download/20260602/llvm-mingw-20260602-msvcrt-i686.zip
sha256: 2c2ced6587900fd0a4ea27d1215d5ae3176ef136da0287acae7f2881b5da4a3e
```

The workflow must not use an unpinned `latest` URL for this compiler. The
official release source and digest are visible in the build log and in the
artifact's generated build metadata.

The `setup-msbuild` output may be either the MSBuild executable or its
containing directory. The workflow must normalize a directory output by
appending `MSBuild.exe` before passing the canonical file path to the build
scripts.

## CI-Only MSBuild Compatibility

Add an explicit `-AllowNonV18MsBuild` switch to `tools/build.ps1` and
`tools/test.ps1`. Pass it through `Get-RenderStackToolchain` to
`Find-RenderStackMSBuild`. When the switch is absent, existing Visual Studio 18
validation is unchanged. When present, discovery accepts the canonical
HostX64 MSBuild executable from a Visual Studio 17 or 18 installation and
still records its exact product/version information. The switch is for
hosted CI only and is never used by `tools/release-gate.ps1`.

The CI switch must not bypass the requirement for `vswhere`, a canonical
MSBuild path, a Visual Studio C++ installation, or a successful version probe.

Add an explicit `-SkipLocalBridgeEvidence` switch to `tools/test.ps1`. When
used by hosted CI it records the two game-root Bridge evidence checks as
optional skips because the runner has no user's GTA installation. Without the
switch, local test behavior remains unchanged.

Add an explicit `-AllowMissingBridgeEvidence` switch to `tools/package.ps1`
and `tools/write-manifest.ps1`. In hosted CI this derives current Bridge
toolchain evidence from `out/build-metadata.json` and the selected MSVC
installation, ignoring stale or absent local game-root evidence. The default
packaging path remains strict and still requires the existing Bridge evidence
record.

### Hosted runtime boundaries

Hosted Windows runners are suitable for source, build, static, and packaging
validation but are not a reliable source of the local Vulkan runtime or the
game-root fixture layout used by every Bridge executable. Add two explicit
`tools/test.ps1` switches:

- `-SkipGpuRuntimeProbes` records the DXVK device probes and Bridge tests that
  create a Vulkan-backed D3D9 device as non-required skips;
- `-SkipEnvironmentSensitiveBridgeTests` records Bridge config fixture tests
  that require the local game-root test layout as non-required skips.

The default local invocation does not set either switch. A hosted run must
show each skipped test and reason in `out/test-results.json`; an unavailable
GPU must never be converted into a passing runtime result. The workflow passes
both switches explicitly.

The export verifier accepts explicit `-Ninja` and `-Glslang` paths and the test
orchestrator forwards them. Source-only checks must not require `rg`; the
backend API source test uses `Get-Command rg.exe` when available and a
`Select-String` fallback otherwise.

## Artifacts

On success, upload these paths with `actions/upload-artifact`:

- `out/packages/SA-RenderStack-v<VERSION>-split.zip`;
- `out/packages/SA-RenderStack-v<VERSION>-sdk.zip`;
- `out/packages/SA-RenderStack-v<VERSION>-symbols.zip`;
- `out/packages/SA-RenderStack-v<VERSION>-source-manifest.json`;
- `out/test-results.json`;
- CI-generated `out/logs/**` only; a stale local
  `out/reports/phase-1-release-gate.md` must not be uploaded or treated as CI
  evidence.

The workflow must use `if-no-files-found: error` for required package assets
and `warn` for optional logs. It must not upload credentials, the full runner
environment, or the local game directory.

## Verification Contract

Add `tests/windows-ci-workflow-test.ps1` as a static contract test. It checks:

- the workflow exists at the expected path;
- the three required triggers and `workflow_dispatch` exist;
- the runner, read-only contents permission, timeout, and concurrency policy
  are present;
- the pinned LLVM-MinGW URL and digest are present;
- build, test, package, and package-layout commands are present;
- the CI-only MSBuild switch is passed;
- the workflow does not call `release-gate.ps1`, `gh release create`, or an
  upload path outside `out/`;
- the workflow passes `-SkipLocalBridgeEvidence` to avoid requiring a local
  GTA installation;
- the workflow passes the explicit GPU and environment-sensitive test boundary
  switches and checks out full history with `fetch-depth: 0`;
- required artifact upload entries use `if-no-files-found: error`.

`tests/hosted-ci-boundary-regression-test.ps1` covers the switch forwarding,
export-tool forwarding, full-history checkout, and no-`rg` source-search
fallback contracts.

Run the contract test locally before and after implementation. Add it to the
normal PowerShell test orchestration so future workflow edits are covered by
the repository test suite.

## Security And Maintenance

- Keep `permissions: contents: read` unless a future workflow has a separately
  justified permission requirement.
- Do not place GitHub tokens in shell commands or artifact files.
- Keep third-party action versions explicit and review their upgrades.
- Update the LLVM-MinGW URL and digest together, then rerun the full CI and
  provenance tests.
- Keep CI artifact names tied to the commit SHA so concurrent runs cannot be
  confused with release assets.
- A green hosted workflow proves build/test/package reproducibility only; it
  does not prove GTA in-game streaming, shader, FPS, or third-party mod
  compatibility.

## Acceptance Criteria

The change is accepted when:

1. The workflow contract test passes locally.
2. The normal source/test orchestration passes locally.
3. A GitHub Actions run can bootstrap the pinned x86 toolchain, build Bridge
   and DXVK, run the required tests while explicitly reporting any hosted
   runtime skips, package the three archives and source manifest, and upload
   them.
4. The workflow never runs the local game-root release gate or creates a
   GitHub Release.
5. The local release gate behavior remains unchanged when invoked without the
   CI-only switch.
