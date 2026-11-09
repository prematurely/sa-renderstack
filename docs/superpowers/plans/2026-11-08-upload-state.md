# Upload source mapping and repeated state work implementation plan

> Execute this plan inline with superpowers:executing-plans. Use the final fresh review required by that skill.

**Goal:** Reduce source mapping churn during a texture upload and stop repeated deterministic compatibility failures at render safe points.

**Architecture:** Keep active source mappings live throughout each texture upload transaction while allowing other LRU views to be reclaimed after each subresource. A rejected pinned module remembers its first mismatching call site. Stable bytes skip full preflight and logging, and restored bytes permit another full installation attempt. Transient installation errors remain retryable.

**Spec:** The two paths authorized in chat on 2026-11-08 and the local F8 findings in out/perf/20261108-f8-35304-2/findings.md.

**Constraints:** Preserve texture bytes, upload ordering, dirty rectangles, public interfaces, drawing quality, existing staging copies and mapping-budget enforcement. Keep candidates in out/perf/20261108-upload-state. Preserve pre-existing checkout changes. No commit or deployment is part of this batch.

## Source mapping task

- [x] Add a real D3D9/Vulkan fixture that counts the backend's actual Windows map/unmap calls around managed and SYSTEMMEM mip-chain uploads. Verify sampled mip content and repeated write/upload behavior.
- [x] Reproduce multiple mappings of the same source during a pressured mip-chain upload on the baseline.
- [x] Protect source mappings across UpdateTexture and UploadManagedTexture, retain immediate reclamation of unrelated views, and trim the source after the scope ends.
- [x] Verify mapping reuse, final source reclamation, partial updates, sampled pixels for every mip and cube face, release and managed device reset.
- [x] Repeat interleaved baseline/candidate A/B plus baseline A/A for pressured and ordinary controls. Record CPU work reductions and wall-time limitations separately.

## Repeated state work task

- [x] Run a fixture through the actual render-safe-point retry policy. An unchanged rejected signature performs one full preflight, transient PIN failures retry, and restored signatures allow a successful installation.
- [x] Reproduce the unbounded rejection attempts on the baseline.
- [x] Apply a module-instance rejected-site state to the failing optimization-patch path.
- [x] Regress shutdown, successful patch installation after signature restoration, signature refusal, device reset and transient PIN failure.
- [x] Measure repeated rejected-module polling with the actual production entry path. Keep this independent from generic state binding experiments.

## Validation and handoff

- [x] Run focused native tests and regression mutation checks.
- [x] Run tools/test.ps1 with the audited archived Bridge reference. Inspect every failure and optional skip.
- [x] Obtain final read-only review, address substantive findings, verify installed DLL/config hashes and package candidate evidence.

## Progress

- Baseline snapshots and protected-install hashes saved before product edits.
- Implementation choice: first fix directly reproducible source self-eviction across mip levels. Parallel page preparation remains a separate candidate until its integrated lifetime and net benefit are proved.
- Review ruling: protect only active upload sources, including nested sources, while reclaiming unrelated LRU mappings at existing points. The earlier global-deferral candidate failed the new ordering regression and is retained as superseded evidence.
- Review ruling: PIN does not make live bytes immutable. Recheck the rejected six-byte site cheaply and retry when it matches. The earlier permanent-rejection candidate failed the signature-restoration regression.
- The periodic unsupported-CRC path is outside this first retry change because its module lifetime differs. Generic repeated sampler calls remain measured separately from the Present retry fix.
- Pixel checks sample two positions per 2D mip and one position per cube face/mip. The new cold-view test verifies reclamation order and does not establish process-wide VA high water.
- Final native gates: upload pressure 1,569 checks; ordinary 1,520; single mip 819; retry reject/transient/restore 38/38/143; all zero failures. The same final pressure fixture fails the original backend on six map/unmap assertions, with pixel checks passing.
- Full validation: first run 81 passed, 1 compile failure, 2 optional skips. The unchanged thread-journal-no-compat compile could not be reproduced in isolation. A complete rerun passed 82 gates with zero failures and the same two missing external-audit inputs skipped. The first failure remains archived.
- Two final production A/B passes each use six A/B and three A/A pairs per case, with AB and BA order. Pressured upload main-thread cycles fall about 7%; mapping calls fall 5,632 to 512 over 512 uploads. Ordinary and single-mip controls show timing noise, including one single-mip wall-time regression that is not treated as an accepted performance result.
- Retry CPU cycles decrease in all six comparisons in both final passes, but 2,000 empty Present calls do not show stable end-to-end wall-time benefit. One final pass regressed 42.53%; follow-up instrumentation with normal affinity measured -1.44% against 6.59% A/A median variation, while CPU cycles fell 40.51%. CS queue idle dominates this fixture, and existing completed CS/GPU synchronization counters are zero. The specific presentation wait is not established. Do not claim throughput/FPS acceptance.
- Candidate DLLs and matching RSYM fingerprints are under out/perf/20261108-upload-state/candidate-package. No runtime DLL dependency was added. Installed DLL/config hashes are unchanged. Game-route validation remains unperformed and deployment is not part of this batch.
