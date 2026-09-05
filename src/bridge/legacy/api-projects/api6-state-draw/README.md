# API6 StateDrawBatch (C++23)

独立 C++23 SDK consumer：`include/`+`src/` 提供 `StateDrawTransaction`（仅一个紧随的 DP/DIP、`std::expected` 原子校验），`main.cpp` 动态加载 backend 并验证 DP、DIP 与非法输入。

```powershell
cmake -S . -B build; cmake --build build --config Release
build\\Release\\api6_state_draw.exe path\\to\\d3d9.dll
```

代码使用 C++23 编译、RAII COM 持有和先验证后执行的 contract-test。依据 Microsoft D3D9 `CreateVertexBuffer/DrawPrimitive` 约束及 DXVK `ValidateGtaSaStateDrawBatch`/`SubmitGtaSaStateDrawBatch` 源码。它是可重复的接口验证小项目，不是生产 consumer。
