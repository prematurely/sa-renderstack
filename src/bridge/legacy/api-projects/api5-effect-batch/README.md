# API5 EffectStateBatch (C++23)

独立 C++23 SDK consumer：`include/`+`src/` 提供 `EffectStateBatchBuilder`（`std::span` 分类状态、`std::expected` 先校验后提交），`main.cpp` 是动态加载 backend 的 contract probe。

```powershell
cmake -S . -B build
cmake --build build --config Release
build\\Release\\api5_effect_batch.exe path\\to\\d3d9.dll
```

probe 验证完整 effect state batch、native journal 恢复、非法输入 fail-closed 及无部分状态。仅验证公开 `backend_api.h` 合约，不是 ProperShaders 生产 consumer。依据 Microsoft D3D9 `Direct3DCreate9/CreateDevice` 与 DXVK `SubmitGtaSaEffectStateBatch` 实现。
