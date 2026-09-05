# API3 state batch (C++23)

This standalone C++23 project builds `api3-state-batch-lib` (a reusable `StateBatchBuilder`) plus a demo and unit test. The builder accepts `std::span` data, merges adjacent register ranges, validates capacity before constructing the ABI view, and submits through one COM transaction. The demo dynamically loads a supplied D3D9 backend, creates a hidden D3D9 device, and exercises `ID3D9GtaSaCompatDevice2::SubmitStateBatch`.

```text
api3-state-batch.exe path\to\d3d9.dll
```

The probe checks legal multi-register vertex and pixel constant ranges, then submits a batch containing one valid range followed by an out-of-range register. DXVK validates the complete descriptor before taking its device lock, so the failed call must leave the previously committed state unchanged. Null range pointers are also rejected. If the DLL or a GPU device cannot be created the executable exits with a clear `SKIP` result.

The public ABI is included from `sdk/include/sa_renderstack/backend_api.h`; no private DXVK headers or production sources are linked.  RAII-like cleanup is explicit at each early-return boundary so a failed probe cannot leak COM interfaces or the loaded module.

Design references:

- [IDirect3DDevice9::SetVertexShaderConstantF](https://learn.microsoft.com/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-setvertexshaderconstantf) defines register-range semantics.
- [IDirect3DDevice9::GetVertexShaderConstantF](https://learn.microsoft.com/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-getvertexshaderconstantf) is used for the no-partial-state assertion.
- [IDirect3DStateBlock9](https://learn.microsoft.com/windows/win32/direct3d9/state-blocks) motivates all-or-nothing validation before applying a batch.
- DXVK's `ValidateGtaSaStateBatch` and `SubmitGtaSaStateBatch` are the implementation contract this probe audits.
