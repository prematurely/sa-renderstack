# M1 Thread Scheduling Implementation Plan

> **For agentic workers:** Use subagent-driven development with bounded file ownership. Preserve existing worktree modifications.

**Goal:** Implement opt-in topology-aware scheduling for existing device-owner, DXVK CS, and background threads, without changing render semantics.

**Architecture:** A shared, header-only Windows scheduling helper with a pure selection policy. Consumers create thread-lifetime scopes; the CS scope alone may join MMCSS. Root INI selection is shared conceptually across Bridge/backend.

**Tech Stack:** C++23, MSVC Win32, LLVM-MinGW x86, Meson/Ninja, PowerShell.

**Spec:** `docs/superpowers/specs/2026-09-05-thread-scheduling-design.md`

## Global Constraints

- Both new switches default to 0; failed tuning must not fail rendering.
- No changes to public COM API v1–v7 or draw order.
- No game-root deployment or claimed performance gain without runtime validation.
- Existing dirty baseline saved separately; only feature-related edits are made.

## Tasks

- [x] Locate current repository, preserve baseline diff, verify official CPU Sets/MMCSS documentation.
- [ ] Add pure CPU selection and tests, observe RED, implement GREEN.
- [ ] Implement Windows apply/restore scope and fallback tests.
- [ ] Integrate Bridge owner/background scopes and root/legacy configuration selection.
- [ ] Integrate DXVK CS/compiler scopes; gate legacy process hard pin when new mode is on.
- [ ] Register test target with build/test orchestration and default-off config.
- [ ] Correct chapter 17 with current-source/API/mathematical evidence and milestone status.
- [ ] Build both DLLs and run focused + existing regression tests; review diff and preserve runtime files.
- [ ] Record actual validation and remaining M2–M5/visual/FPS work.
