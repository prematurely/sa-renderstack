# API1 状态库与示例（C++23）

`include/sa_api1/status_client.hpp`/`src/status_client.cpp` 提供可复用的
`StatusClient`。它只依赖 SDK 中的 COM 接口声明，不复制或猜测
`ID3D9VkInteropDevice` 的 ABI；Interop 句柄通过 COM 的 `IUnknown::Release`
按 RAII 释放。`std::expected` 用于携带 HRESULT，`validate` 检查结构大小、
API 版本和 Vulkan 能力位。

`main.cpp` 是最小 consumer：动态加载用户传入的 `d3d9.dll`，创建隐藏窗口
D3D9 设备并查询 API1。缺少路径、导出、适配器或接口均为 `[FAIL]`，避免把
未执行验证伪装成成功跳过。

设计依据：[Microsoft D3D9 CreateDevice](https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3d9-createdevice)、[COM IUnknown](https://learn.microsoft.com/en-us/windows/win32/api/unknwn/nn-unknwn-iunknown)、[cppreference std::expected](https://en.cppreference.com/w/cpp/utility/expected)。

```powershell
cmake -S . -B ../../out/api-project-build/api1 -A Win32
cmake --build ../../out/api-project-build/api1 --config Release
..\..\out\api-project-build\api1\Release\sa-api1-status.exe "..\..\out\build\dxvk-x86\src\d3d9\d3d9.dll"
```

单元测试 `tests/status_client_tests.cpp` 不需要 GPU；库、示例和测试均可由
根 CMake `add_subdirectory` 引入。构建产物统一放在 `out/api-project-build`。
