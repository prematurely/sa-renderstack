# 2026-11-08 纹理映射与失败重试优化

两条路径已实现候选并通过自动行为回归。受压 mip-chain 上传和失败重试的主线程工作量减少；整个 Present 流程的墙钟收益尚未确认，本轮不视为游戏性能验收通过。没有部署、启动游戏、提交或推送。

## 触发证据与本轮范围

最新现场来自 PID 35304 / captureId 2，见 `out/perf/20261108-f8-35304-2/findings.md`。107.56 ms 长帧中有纹理数据准备栈；同一采集中出现约 2,500 条 DayVector 签名拒绝日志。现场采样不能单独证明源映射驱逐是这次长帧的原因，本轮先修复独立夹具能够重现的机制。

1. **通用上传源映射生命周期**：UpdateTexture 和 UploadManagedTexture 的整个 mip/face 上传事务中保护正在使用的源映射。其他 LRU 冷映射仍在原回收点回收，事务结束后源映射重新参与回收。没有改变暂存缓冲、CPU 数据复制、上传顺序、dirty rect 或公开接口，也没有按 MOD 名称分支。
2. **既有兼容补丁的失败重试**：已 PIN 的模块记录第一个不匹配的调用点。后续安全点只复查该六字节签名，稳定拒绝时跳过完整预检和日志；签名恢复则重新预检和安装。临时 PIN 错误继续重试。这是既有兼容补丁机制的开销修复，不是通用 sampler 状态调用的去重。

没有新增加载线程或并行源页预热，也没有改周期性 CRC 重试路径。大量重复 sampler / texture / constant 调用仍是后续独立工作。

## 真实入口回归

新增测试通过真实 x86 D3D9 / Vulkan 设备以及 Bridge 工厂、CreateDevice、Present、Reset、Shutdown 入口运行。观察实际 MapViewOfFile / UnmapViewOfFile 和模块 PIN 调用；测试专用导入表观察器不进入产品 DLL。

| 自动场景 | 检查数 | 失败 |
| --- | ---: | ---: |
| 1 MiB 受压上传 | 1,569 | 0 |
| 100 MiB 普通预算 | 1,520 | 0 |
| 单 mip 对照 | 819 | 0 |
| 不匹配签名 | 38 | 0 |
| 临时 PIN 失败后恢复 | 38 | 0 |
| 签名恢复、成功安装与退出还原 | 143 | 0 |

覆盖 managed、SYSTEMMEM→DEFAULT、完整 mip-chain、重复写入、partial rect、cube 全 face/mip、managed 资源存活时 Reset、释放和退出。每个二维 mip 检查两个像素采样点，每个 cube face/mip 检查一个采样点；不等于全图逐字节比较。

同一最终受压夹具运行原 backend：1,569 检查中仅六项映射次数断言失败；二维源每次上传 map/unmap 各 11 次，cube 各 60 次。候选均为各 1 次，像素采样通过。

还保留了两个预期失败的变异验证：

- 全局延迟所有映射回收的早期候选违反冷映射回收顺序，最终夹具能够检出。
- 永久记住失败而不再检查签名的早期候选无法从签名恢复，最终夹具能够检出。

回收顺序测试证明无关视图在后续 mip 复制前已回收，未测量整个 32 位进程的虚拟地址峰值。补丁夹具使用惰性假 ASI 提供输入字节，不执行假的替换调用，不加载游戏 MOD。

## 重复 A/B 与 A/A

基线固定为开工前安装版本：backend `943827FE52992B02973BDECFCF51E2AB168D06176D4D0F73217F5305D143A50D`，Bridge `1484A9869FC85347950E7974EDF8B806D74A7886470A89E95156BCEF4C5EAEE8`。

每种上传场景分别计量 managed / SYSTEMMEM，64 次预热后计时 512 次上传。每个场景 6 组 A/B、3 组 A/A，AB 和 BA 交错。最终实现有两轮周期计量；第二轮使用完整流水线重新构建出的同一份交付二进制。计时主线程在设备/工作线程预热后固定到同一个允许的逻辑 CPU，退出计时后恢复；真实行为回归和游戏没有设置该限制。

以下是配对变化的中位数，负数代表减少。CPU 周期不换算成毫秒或游戏 FPS。

| 场景 | 首轮 CPU 周期 | 最终产物 CPU 周期 | 首轮墙钟 | 最终产物墙钟 |
| --- | ---: | ---: | ---: | ---: |
| 受压 managed mip-chain | -7.27% | -6.99% | -8.15% | -6.82% |
| 受压 SYSTEMMEM mip-chain | -7.58% | -7.38% | -8.04% | -8.70% |
| 普通 managed mip-chain | -4.14% | +2.24% | +0.58% | -0.53% |
| 普通 SYSTEMMEM mip-chain | -0.56% | -0.53% | +1.46% | -0.58% |
| 受压 managed 单 mip | +0.43% | +3.03% | +0.47% | +5.63% |
| 受压 SYSTEMMEM 单 mip | +0.49% | +1.59% | +0.56% | +1.78% |

两轮受压 mip-chain 的 CPU 周期均为 6/6 配对减少；512 次上传映射数稳定从 5,632 降到 512。普通预算均为零额外映射；单 mip 均为 512 次。普通和单 mip 不计作收益；最终单 mip 墙钟有回退，应保留为验收限制。最终单 mip CPU 周期 A/A 绝对变化中位数为 managed 3.62%、SYSTEMMEM 2.28%，没有足够证据把其墙钟变化归因到新增 CPU 工作。

失败重试使用实际 Bridge 加固定 backend、同一个假模块输入，预热 16 次后计量 2,000 次 Present：

| 计量轮次 | CPU 周期配对中位数 | CPU 减少组数 | CPU A/A 绝对变化中位数 | 墙钟配对中位数 | 墙钟 A/A 绝对变化中位数 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 首轮固定逻辑 CPU | -37.77% | 6/6 | 12.68% | -7.73% | 14.22% |
| 最终产物固定逻辑 CPU | -27.56% | 6/6 | 4.26% | +42.53% | 9.99% |
| 补充等待统计、固定逻辑 CPU | -34.96% | 6/6 | 2.47% | +4.52% | 9.57% |
| 补充等待统计、正常线程调度 | -40.51% | 6/6 | 4.18% | -1.44% | 6.59% |

稳定签名拒绝从夹具整个生命周期的 2,024 次 PIN / 拒绝日志降为 1 次。行为收益和 CPU 周期收益可重复；墙钟结果方向不稳定，尤其最终产物一轮的 +42.53% 不能丢弃，因此不确认整体 Present 吞吐收益。

补充读取现有线程统计接口，正常调度下基线 / 候选的 CS 空等中位数约 383 / 410 ms，墙钟中位数约 469 / 474 ms；已完成 CS 同步等待和资源 GPU 同步等待均为零。这说明该 32×32 空呈现夹具并非纯 CPU 工作基准，CS 多数时间空等。没有进一步确定异常的具体呈现等待点，也不能以这些计数断言游戏 GPU 不忙。GPU idle 是现有估计值，不是 timestamp-query GPU 耗时。

GetThreadTimes 的短窗口值受 15.625 ms 等量化影响，保留原始数据但不用其百分比接受收益。未引入产品线程亲和性、睡眠或人为节流来改善基准数字。

## 完整验证与限制

最终命令：

```powershell
pwsh -NoProfile -File tools/test.ps1 -BridgeReference 'D:/GTA San Andreas/Temp/SA-RenderStack-backups/pre-alpha.1-20260829-230407-3d998b6e/d3d9.dll'
```

最终结果 **82 passed / 0 failed / 2 skipped**。两个跳过均为可选外部审计：未提供 external DXVK source，以及 external Bridge/config。新增四项 native gate、导出验证、依赖来源验证均通过。

首次完整运行是 81 passed / 1 failed / 2 skipped：thread-journal-no-compat 在未改动的 Vulkan 头文件 vkCmdEndQuery 声明处出现编译错误。未修改头文件、编译参数或弱化检查；同命令单独重跑 445 checks / 0 failures，完整重跑也通过。首次故障未能复现，根因未确定，原始日志和首次失败 JSON 均保留。

最终只读审查提出的跨平台条件编译、无关 LRU 及时回收、活跃签名恢复，以及基线重映射地址观察问题均已处理。实际构建和运行验证限 Windows x86；不声称已做其他平台构建。

两个产品 DLL 的静态导入 DLL 集合相较基线一致，没有新增运行时 DLL 依赖。此项不表示无需现有 Vulkan 驱动，也不是对全部既有功能依赖的重新认证。

没有进行同存档首次出门或室外稳态游戏采集，不能据此称 107 ms 尖峰已修复、提高了多少 GTA FPS，或允许忽略上述控制场景与呈现吞吐限制。

## 产物与追溯

所有证据位于 `out/perf/20261108-upload-state`：

- `source-before/`、`worktree-before.patch`：保留编辑前已有的脏工作区。
- `red-final-baseline/`、`green-final-mapping/`、`green-final-retry/`、`native*`、`retry/`：原始行为输出。
- `review-mutation-*`：早期错误候选的预期失败。
- `cycles-*-ab.csv`、`validated-*-ab.csv` 及同名前缀目录：两轮最终实现 A/B 原始输入输出。
- `cycles-benchmark-summary.json`、`validated-benchmark-summary.json`：配对与 A/A 统计。
- `waits-pinned-*`、`waits-normal-*`、`retry-waits-summary.json`、`retry-wait-profile.cpp`：补充等待统计，只用于诊断。
- `superseded-v1/`、最早 `upload-ab*` / `repeat-upload-ab*`：已替代实现的数据，不作为最终候选收益。
- `pipeline-results.json`：首次失败；`pipeline-results-repeat.json`：最终通过；具体完整日志路径包含于 JSON。
- `package-manifest.json`、`import-dependencies.json`、`install-before.json`、`install-after.json`：指纹、静态 DLL 依赖与安装目录保护校验。
- `candidate-package/`：两个最终 DLL 和匹配的 RSYM；仅供候选审查，未部署，整体性能验收未闭合。

最终候选 Bridge SHA-256 为 `A2407964045254C9D010E496D5A7BAB8D3AA2925960A48CB99F01BA603853391`，backend 为 `ECB34529DA46E6671D628D8BC56366017D17A625EDDB8B44F8226085252DA574`。RSYM 内的 binary SHA-256 已逐一匹配。

游戏目录两份 DLL、SA.RenderStack.ini 与 dxvk.conf 的 SHA-256 均与开工前一致。
