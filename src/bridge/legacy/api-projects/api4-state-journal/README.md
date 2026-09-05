# API4 state journal (C++23)

This C++23 project builds `api4-state-journal-lib`, whose `JournalSession` wraps a native journal in RAII with same-thread checks and fail-closed cleanup, plus a demo and unit test. `api4-state-journal.exe path\\to\\d3d9.dll` dynamically loads a backend and validates the complete `BeginStateJournal` / `RestoreStateJournal` lifecycle on a real D3D9 device. It covers restore without begin, nested begin, double restore, exact state restoration, and Reset discard.

The executable uses only the stable public header `sdk/include/sa_renderstack/backend_api.h`. It does not link private DXVK code. COM and module cleanup is performed on every early return. A missing DLL, export, window, or GPU device produces `SKIP` so CI can run the same binary on non-GPU hosts without a false failure.

The transaction model follows Microsoft D3D9 state-block guidance: state capture has a clear begin/end boundary, and device reset invalidates default-pool state. DXVK's `BeginGtaSaStateJournal` rejects nested journals and `OnResetBegin` discards a live journal; these are treated as contractual behavior and tested directly.

References: [State Blocks](https://learn.microsoft.com/windows/win32/direct3d9/state-blocks), [IDirect3DDevice9::GetRenderState](https://learn.microsoft.com/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-getrenderstate), [IDirect3DDevice9::Reset](https://learn.microsoft.com/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-reset). RAII/fail-closed rationale follows modern C++ guidance: validate lifecycle transitions before mutating shared device state and make invalid transitions observable as HRESULT failures.
