# GTA-SA API1–API7 附属 C++23 子项目集

这里的七个目录是主程序 Bridge 的附属 API 子项目。它们按 API 版本保持独立的
代码边界、库边界和测试边界，但由 `src/bridge/legacy` 主工程拥有，不作为七个
独立运行时产品发布。每个目录都有自己的 `CMakeLists.txt`、`include/`、`src/`、
`main.cpp` 和 README；实现只依赖主程序提供的公开 SDK，不链接 DXVK 私有实现。

| 目录 | API | 主要职责 |
| --- | ---: | --- |
| `api1-status` | 1 | 状态、版本、能力位与 Vulkan interop 生命周期 |
| `api2-vulkan-pass` | 2 | Present command buffer pass 注册、排序、注销和失败隔离 |
| `api3-state-batch` | 3 | 常量范围构建、合并、边界验证和批量提交 |
| `api4-state-journal` | 4 | 完整 state journal 的 RAII、恢复、Reset/fail-closed |
| `api5-effect-batch` | 5 | EffectStateBatch 分类状态构建与先验证后提交 |
| `api6-state-draw` | 6 | 单个 state batch 加紧随 DP/DIP 的事务 |
| `api7-selective-journal` | 7 | selective journal、capture scope、同线程和恢复 |

主程序构建时，七个 `src/` 实现会作为 Bridge 的附属源文件一起编译；根聚合文件
`src/bridge/legacy/api-projects/CMakeLists.txt` 供主工程和开发验证统一枚举这七个
子项目。单个子目录的 CMake 只用于定向开发和测试，不改变运行时 DLL 的归属。

统一聚合构建示例：

```powershell
cmake -S src/bridge/legacy/api-projects -B out/api-project-build/all -G "Visual Studio 18 2026" -A Win32
cmake --build out/api-project-build/all --config Release
```

这些项目的 demo 需要传入 backend `d3d9.dll`；没有 D3D9/Vulkan 环境时应明确报告
不可运行。构建通过只证明 SDK consumer 和 C++23 工程正确，不能证明该 API 已经被
ProperShaders 或 GTA 主渲染路径采用。是否由 Bridge 在生产路径调用，仍以主程序的
consumer 代码、运行计数器和游戏回归证据为准。

设计参考：

- [Microsoft D3D9 CreateDevice](https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3d9-createdevice)
- [Microsoft D3D9 state blocks](https://learn.microsoft.com/en-us/windows/win32/direct3d9/state-blocks)
- [Khronos Vulkan command buffers](https://registry.khronos.org/vulkan/specs/1.3-extensions/html/chap6.html)
- [C++23 std::expected](https://en.cppreference.com/w/cpp/utility/expected)
- [C++ std::span](https://en.cppreference.com/w/cpp/container/span)
