# Windows CI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a hosted Windows GitHub Actions workflow that builds, tests, packages, and uploads SA RenderStack artifacts without depending on the local GTA installation.

**Architecture:** Keep `tools/release-gate.ps1` as the local release authority because it checks protected game-root rollback files. Add a narrowly scoped CI-only MSBuild compatibility switch while preserving strict Visual Studio 18 behavior by default, then have the workflow bootstrap the remaining x86 toolchain and call the existing build/test/package scripts.

**Tech Stack:** GitHub Actions, `windows-2025`, PowerShell 7, Visual Studio MSBuild, Python 3.12, MSYS2, LLVM-MinGW 20260826 msvcrt-i686, Meson, Ninja, glslang, ZIP artifacts.

**Spec:** `docs/superpowers/specs/2026-08-30-windows-ci-design.md`

## Global Constraints

- CI triggers are `pull_request`, push to `main`, and `workflow_dispatch`.
- CI uses `windows-2025`, `permissions: contents: read`, and a 60-minute timeout.
- The pinned compiler archive is `llvm-mingw-20260826-msvcrt-i686.zip` with SHA-256 `8fb74ce85b94f225195113317fe1d6c3da605d6777b39e4ee6461d594fb07160`.
- End users and hosted runners must not require the local GTA San Andreas installation.
- `tools/release-gate.ps1` must remain strict and must not be called by CI.
- No workflow step may create tags, GitHub Releases, or write outside the workspace and runner tool caches.
- Set `$env:GIT_CONFIG_GLOBAL='NUL'` before Git commands.
- Keep source files ASCII/LF and do not track `out/` build artifacts.

---

### Task 1: Workflow Contract Test

**Files:**
- Create: `tests/windows-ci-workflow-test.ps1`
- Modify: `tools/test.ps1`

**Interfaces:**
- Consumes: `.github/workflows/windows-ci.yml` once created.
- Produces: a deterministic static check for workflow triggers, runner policy, pinned toolchain, build commands, artifact policy, and release isolation.

- [ ] **Step 1: Write the failing contract test.**
  Make the test require the workflow path and assert the literal contract: `pull_request`, push branch `main`, `workflow_dispatch`, `windows-2025`, `contents: read`, `timeout-minutes: 60`, `cancel-in-progress: true`, the pinned LLVM-MinGW URL/digest, `AllowNonV18MsBuild`, build/test/package/package-layout commands, `upload-artifact@v4`, `if-no-files-found: error`, and the absence of `release-gate.ps1` and `gh release create`.
- [ ] **Step 2: Run the test before implementation.**
  Run `pwsh -NoProfile -File tests/windows-ci-workflow-test.ps1`. It must fail because `.github/workflows/windows-ci.yml` does not exist.
- [ ] **Step 3: Add the test to normal orchestration.**
  Register it as a required `windows-ci-workflow` layout/automation gate in `tools/test.ps1` beside the repository layout and source hygiene checks.
- [ ] **Step 4: Keep the focused test independently runnable.**
  Run the test directly after workflow implementation and ensure its output reports PASS without reading `out/` or the game directory.

### Task 2: CI-Only MSBuild Discovery

**Files:**
- Modify: `tools/build.ps1`
- Modify: `tools/test.ps1`
- Modify: `tools/lib/toolchain-discovery.ps1`
- Create: `tests/msbuild-compatibility-regression-test.ps1`

**Interfaces:**
- Consumes: existing `Get-RenderStackToolchain`, `Find-RenderStackMSBuild`, and build/test tool parameters.
- Produces: `-AllowNonV18MsBuild` on build/test, forwarded to discovery; default invocations still require Visual Studio major 18, while CI invocations accept canonical major 17 or 18 MSBuild and record the actual version.

- [ ] **Step 1: Write the failing compatibility test.**
  Assert that build/test help advertises `-AllowNonV18MsBuild`, discovery exposes the switch, strict source validation still checks major 18, and the CI path includes the accepted major-17-or-18 branch. Also run local strict discovery and require the current local product major to remain 18.
- [ ] **Step 2: Run the test before implementation.**
  Run `pwsh -NoProfile -File tests/msbuild-compatibility-regression-test.ps1`; it must fail because the new switch and forwarding path are absent.
- [ ] **Step 3: Implement the switch with strict default behavior.**
  Add the switch to build/test parameter blocks and help text, pass it through `Get-RenderStackToolchain`, and update `Find-RenderStackMSBuild` so the default query/range/filter remains `[18.0,19.0)` while the explicit switch uses `[17.0,19.0)` and accepts only product major 17 or 18. Keep `vswhere`, canonical HostX64 MSBuild path, C++ Build Tools requirement, file-version validation, and command version probing in both paths.
- [ ] **Step 4: Pass the switch through test-triggered build refresh.**
  When `tools/test.ps1` invokes `tools/build.ps1` for a stale build, include `-AllowNonV18MsBuild` only when the parent test invocation received it. Do not alter `tools/release-gate.ps1`.
- [ ] **Step 5: Run the compatibility test after implementation.**
  Confirm the new test passes and the existing orchestration discovery test still proves strict Visual Studio 18 behavior.

### Task 3: Hosted Windows Workflow

**Files:**
- Create: `.github/workflows/windows-ci.yml`
- Modify: `README.md`

**Interfaces:**
- Consumes: build/test/package scripts, CI-only MSBuild switch, and the pinned toolchain contract.
- Produces: commit-scoped Actions artifacts containing runtime ZIPs, SDK/symbol ZIPs, source manifest, test results, and optional logs.

- [ ] **Step 1: Add workflow metadata and concurrency.**
  Configure the three triggers, `windows-2025`, `permissions: contents: read`, `timeout-minutes: 60`, and a concurrency group keyed by workflow/ref with `cancel-in-progress: true`.
- [ ] **Step 2: Bootstrap tools.**
  Use `actions/checkout@v4`, `actions/setup-python@v5` for Python 3.12, `microsoft/setup-msbuild@v2` with x64 MSBuild, `msys2/setup-msys2@v2` for Ninja and glslang, `actions/cache@v4` for `out/deps`, and a PowerShell step that downloads/verifies/extracts the pinned LLVM-MinGW archive and exports explicit tool paths through `GITHUB_ENV`.
- [ ] **Step 3: Run the repository pipeline.**
  Read and validate `VERSION`, then call `build.ps1 -Configuration Release -Architecture x86 -Component All -Clean -AllowNonV18MsBuild`, `test.ps1 -Configuration Release -Architecture x86 -AllowNonV18MsBuild`, `package.ps1 -Version <VERSION> -Configuration Release`, and `tests/package-layout-test.ps1 -Version <VERSION> -Configuration Release` with explicit discovered tool paths.
- [ ] **Step 4: Upload only CI evidence.**
  Upload required package ZIPs/source manifest and test results with `actions/upload-artifact@v4` and `if-no-files-found: error`; upload `out/logs/**` separately with `warn`. Name artifacts with `${{ github.sha }}`. Do not call `release-gate.ps1`, `gh release create`, or upload the game directory.
- [ ] **Step 5: Document the workflow.**
  Add a README CI section explaining triggers, artifact behavior, and the distinction between hosted build validation and local game-root release validation. Do not claim that CI proves in-game FPS or mod compatibility.
- [ ] **Step 6: Run the workflow contract test.**
  Run `pwsh -NoProfile -File tests/windows-ci-workflow-test.ps1` and verify all required tokens and forbidden-operation checks pass.

### Task 4: Full Local Verification

**Files:**
- Verify: `tools/build.ps1`, `tools/test.ps1`, `tools/lib/toolchain-discovery.ps1`, `.github/workflows/windows-ci.yml`, and `README.md`.
- Generated only: `out/` files; do not commit them.

**Interfaces:**
- Consumes: all implementation changes from Tasks 1-3.
- Produces: fresh local evidence that the normal toolchain and release packaging path remain valid.

- [ ] **Step 1: Run focused tests.**
  Run the workflow contract test and MSBuild compatibility regression test directly.
- [ ] **Step 2: Run the normal source/test orchestration.**
  Run `pwsh -NoProfile -File tools/test.ps1 -Configuration Release -Architecture x86` using the local strict Visual Studio 18 path and confirm no new failures.
- [ ] **Step 3: Run a clean build and package layout check.**
  Run `pwsh -NoProfile -File tools/build.ps1 -Configuration Release -Architecture x86 -Component All -Clean`, `pwsh -NoProfile -File tools/package.ps1 -Version 0.1.0-alpha.1 -Configuration Release`, and `pwsh -NoProfile -File tests/package-layout-test.ps1`.
- [ ] **Step 4: Check repository hygiene and release isolation.**
  Run `git diff --check`, `tests/repository-layout-test.ps1`, `tests/source-hygiene-regression-test.ps1`, and confirm no game-root files changed.

### Task 5: Commit And Push

**Files:**
- Commit all source, test, workflow, README, and plan changes from Tasks 1-4.

**Interfaces:**
- Consumes: passing local verification.
- Produces: a synchronized `main` branch with the Windows CI workflow ready for GitHub Actions.

- [ ] **Step 1: Review staged changes and status.**
  Require only intended files, no `out/` artifacts, no credentials, and no game-root changes.
- [ ] **Step 2: Commit with the repository identity.**
  Use `prematurely <51526890+prematurely@users.noreply.github.com>` and a message describing the Windows CI workflow.
- [ ] **Step 3: Push `main` and verify the remote ref.**
  Set `GIT_CONFIG_GLOBAL=NUL` before Git commands, push `origin main`, and confirm the remote head matches the local commit.
