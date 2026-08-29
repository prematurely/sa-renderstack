# SA RenderStack Alpha Release Preparation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prepare a self-contained, auditable `v0.1.0-alpha.1` split-DLL release for controlled GTA San Andreas testing.

**Architecture:** Keep PowerShell scripts as developer and release automation only. The runtime archive remains independently installable by extracting its files into the game root, with an explicit installation and rollback guide. The release remains the two-DLL profile; single-DLL work and new runtime changes are outside this release.

**Tech Stack:** Windows x86, MSVC Bridge, LLVM-MinGW DXVK backend, Meson/Ninja, PowerShell 7, Git, ZIP archives, SHA-256 manifests.

**Spec:** `docs/releases/0.1.0-alpha.1.md`

## Global Constraints

- Release version is `0.1.0-alpha.1` and must match `VERSION`.
- The supported game is GTA San Andreas 1.0.0.0 US, 32-bit.
- The split package must contain the Bridge proxy, GTA DXVK backend, runtime configuration, installation guide, and required licenses only.
- End users must not need MSBuild, LLVM-MinGW, Meson, Ninja, Git, or the source repository to install the runtime package.
- Build, packaging, and release-gate scripts must never modify the game installation.
- Do not deploy new DLLs to `D:\GTA San Andreas` during repository validation.
- Set `$env:GIT_CONFIG_GLOBAL='NUL'` before every Git command.
- Keep source files ASCII with LF endings and do not track `out/` artifacts.

---

### Task 1: Task 10 Handoff Documentation

**Files:**
- Create: `docs/development/phase-1-handoff.md`
- Create: `docs/architecture/module-map.md`
- Create: `docs/development/known-audit-findings.md`

**Interfaces:**
- Consumes: current `README.md`, release note, `packaging/split/package.toml`, Bridge/DXVK API headers, and the phase-1 release-gate report.
- Produces: maintainer handoff instructions, a module/export/runtime ownership map, and an explicit list of unresolved audit findings and release claims.

- [ ] **Step 1: Record the exact build and validation contract.**
  Document the x86 Release commands, required toolchain paths, generated outputs, test count, skipped live audits, and the rule that the game root is never modified by automation.
- [ ] **Step 2: Record the module map.**
  Identify the root `d3d9.dll` Bridge, the `backend/dxvk-gta/d3d9.dll` DXVK backend, the merged DXGI role, API v7, Bridge plugin API v2, configuration files, and export verification boundaries.
- [ ] **Step 3: Record known findings.**
  Separate verified behavior from unverified behavior. State that performance, texture streaming, ProperShaders/ReShade/ENB coexistence, and arbitrary third-party proxy compatibility have no release guarantee unless covered by the fixed workload.
- [ ] **Step 4: Cross-check the three documents against the release note and package mapping.**
  Ensure filenames, paths, version, architecture, and rollback procedure are consistent.

### Task 2: User Installation Contract

**Files:**
- Create: `docs/installation.md`
- Modify: `packaging/split/package.toml`
- Modify: `tools/package.ps1`
- Modify: `tests/package-layout-test.ps1`

**Interfaces:**
- Consumes: the split package destinations and the rollback language in `docs/releases/0.1.0-alpha.1.md`.
- Produces: a manual-install procedure that works without PowerShell and a package-layout test proving the guide is included.

- [ ] **Step 1: Write the manual installation guide.**
  Explain that the user extracts the split ZIP into the GTA root, preserves folders, closes the game before replacement, backs up an existing `d3d9.dll`, and verifies the expected files. Include uninstall/rollback and the fact that SDK/symbol archives are not runtime dependencies.
- [ ] **Step 2: Add the guide to the split package contract.**
  Add `docs/installation.md` as `docs/INSTALL.md` in `packaging/split/package.toml` and update the package script's exact mapping assertion.
- [ ] **Step 3: Extend the package-layout test.**
  Assert that `docs/INSTALL.md` exists in the staged package and that the runtime package still contains exactly the two-DLL deployment paths and required licenses.
- [ ] **Step 4: Run the focused package-layout test.**
  Run `pwsh -NoProfile -File tests/package-layout-test.ps1` and confirm it fails before the mapping update and passes after it.

### Task 3: Final Candidate Build

**Files:**
- Modify: generated files under `out/` only; do not commit them.
- Verify: `docs/releases/0.1.0-alpha.1.md`, `VERSION`, and the split package mapping.

**Interfaces:**
- Consumes: committed documentation and package contract from Tasks 1-2.
- Produces: final local ZIP archives, manifests, hashes, and release-gate report for the exact release commit.

- [ ] **Step 1: Run a clean x86 Release build.**
  Run `pwsh -NoProfile -File tools/build.ps1 -Configuration Release -Architecture x86 -Component All -Clean`.
- [ ] **Step 2: Run the complete test suite.**
  Run `pwsh -NoProfile -File tools/test.ps1 -Configuration Release -Architecture x86` and record required passes and optional skips.
- [ ] **Step 3: Generate final archives and manifests.**
  Run `pwsh -NoProfile -File tools/package.ps1 -Version 0.1.0-alpha.1 -Configuration Release`.
- [ ] **Step 4: Verify package layout and release gate.**
  Run `pwsh -NoProfile -File tests/package-layout-test.ps1` and `pwsh -NoProfile -File tools/release-gate.ps1 -Version 0.1.0-alpha.1 -Configuration Release`.
- [ ] **Step 5: Record artifact hashes.**
  Capture SHA-256 values for split, SDK, symbols, and source-manifest assets from the final gate report.

### Task 4: Release Readiness Review

**Files:**
- Verify: `out/reports/phase-1-release-gate.md` and `out/packages/*`.
- No source changes unless a validation failure identifies a concrete defect.

**Interfaces:**
- Consumes: final candidate artifacts and the documented known findings.
- Produces: a go/no-go decision for an experimental alpha and the exact GitHub Release asset list.

- [ ] **Step 1: Run the fixed game workload against a copied candidate installation.**
  Test menu-to-save loading, indoor/outdoor transitions, fast driving/streaming, tree/wire/shadow rendering, combat and knockdown animation, F7/F8 diagnostics, 20-minute stability, and clean exit.
- [ ] **Step 2: Do not convert unverified behavior into a performance claim.**
  Any crash, texture-streaming defect, low FPS result, or shader/proxy incompatibility must be listed as a known limitation or block the alpha.
- [ ] **Step 3: Confirm repository state.**
  Require a clean working tree, `main` synchronized with `origin/main`, and no tracked build/runtime artifacts.
- [ ] **Step 4: Prepare the formal tag and release assets.**
  Create `v0.1.0-alpha.1` only after Tasks 1-4 pass, then upload the three ZIP files and source manifest using `docs/releases/0.1.0-alpha.1.md`.

## Self-Review

- Task 1 covers the missing Task 10 handoff artifacts.
- Task 2 prevents a source-tree PowerShell dependency for end users.
- Task 3 regenerates manifests after documentation changes.
- Task 4 keeps actual game behavior separate from source/build validation and prevents unsupported performance claims.
- Single-DLL merging, new renderer optimizations, and ReShade/ENB validation are explicitly outside this alpha scope.
