# API2 Vulkan pass 库与示例（C++23）

`include/sa_api2/pass_registration.hpp`/`src/pass_registration.cpp` 提供
`PassRegistration` 静态库。它以 `std::span` 接收名称，以 `std::expected` 返回
HRESULT；RAII 持有 token 和 COM device 引用，`close()` 显式返回注销结果。
内部 trampoline 让 callback userdata 与 token 同寿命，失败后标记关闭，避免
后续调用悬挂。

`main.cpp` 注册严格 API2 pass：只读取不可变 frame，绝不提交、结束或改变
command buffer/图像布局；故意失败的 callback 用来验证后端隔离错误。缺少
路径、适配器、API2 接口或实际回调均为 `[FAIL]`，不会伪造 `[SKIP]` 通过。

契约依据：[Microsoft Vulkan interop guidance](https://learn.microsoft.com/en-us/windows/win32/direct3d12/vulkan-interoperability)、[cppreference std::span](https://en.cppreference.com/w/cpp/container/span)、[cppreference std::expected](https://en.cppreference.com/w/cpp/utility/expected)。

`Present` 用于驱动后端现有 command buffer；如果回调没有实际到达，程序以
`[FAIL]` 退出，避免运行时检查伪绿。仅命令行缺少 DLL 参数时返回 usage。

```powershell
cmake -S . -B ../../out/api-project-build/api2 -A Win32
cmake --build ../../out/api-project-build/api2 --config Release
..\..\out\api-project-build\api2\Release\sa-api2-vulkan-pass.exe "..\..\out\build\dxvk-x86\src\d3d9\d3d9.dll"
```

单元测试 `tests/pass_registration_tests.cpp` 覆盖空参数拒绝，不需要 GPU；
所有构建产物统一放入 `out/api-project-build`，也可由根 CMake 引入。
