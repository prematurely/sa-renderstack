# Repair the ProperShaders Fading entry stack

Scope: restore successful save loading without changing image quality, configuration, rendering algorithms or public interfaces. Existing unrelated working-tree changes must be preserved. No commit or push.

## Evidence and design

The supported local ProperShaders function begins at RVA 0x25590 with push ecx, then push imm32. Its epilogue is pop ecx; ret. The old hook installed at 0x25591 and copied only the latter instruction, so a normal C++ guard calling its gateway consumed the wrong return stack. Both original and candidate Bridge logs show ECX equal to the guard's gateway return address. The user-supplied PID 58628 dump confirms the live bytes 51 E9 and the five-byte gateway.

Move only this hook to the complete entry. Validate 51 68 and the rebased string address, steal six bytes, and resume at 0x25596. Leave state synchronization intact. ECX is volatile at the external void cdecl guard boundary; the original/gateway pair independently preserves its own saved ECX stack slot.

## Tasks

- [x] Preserve original/candidate logs, DLL fingerprints, rollback record and user dump provenance.
- [x] Exercise the production installer, guard, gateway and shutdown in an isolated x86 executable with the real 41-byte function shape.
- [x] Observe executable failure against the old production code before implementing the fix.
- [x] Apply the full-entry/six-byte fix and verify normal returns, stack/nonvolatile registers, call/phase behavior, signatures and restoration.
- [x] Re-run the final fixture against saved old production code, with only include paths rewritten; keep its failing result distinct from fixture setup errors.
- [x] Obtain independent read-only review; add an exact hook target assertion and document test coverage limits.
- [x] Run the full pipeline: 54 passed, 0 failed, two external audit skips. After the test-only assertion, rerun the focused test and all 12 CTest tests.
- [x] Preserve the validated DLLs, symbols and hashes. Back up and replace only the active Bridge/backend after the game exits.
- [x] Verify the same save finishes loading and the player can move; keep startup acceptance separate from FPS improvement.
- [ ] Resume Grove Street transition profiling after successful load verification.

## Verification boundaries

The fixture substitutes only the two external RenderWare API addresses. It verifies patch/control-flow and stack safety, not actual RW-to-D3D alpha values, module discovery or pinning. Existing lifecycle tests disabled this guard and did not cover the failed entry stack.

The rebuilt backend has a different file build identifier, but its .text SHA256 is unchanged from the repeatedly benchmarked constant-control candidate. The Fading fix is a crash correction, and a crashing old path is not a valid FPS A/B baseline.
