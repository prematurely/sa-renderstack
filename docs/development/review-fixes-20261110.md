# Measurement review 修复与全 HUD 验证（2026-11-10）

修复 PR #1 合并后收到的三条 review，并保留此前本地验收的通用 sampler 优化。
sampler 实现单独保存在提交 `7ecb22f33`；生命周期、采集和 HUD 修复在 `f6876b2fd`。
本轮保持公开接口、渲染质量、Vulkan 暂存与复制路径不变，游戏目录没有部署本轮产物。

## 三条 review 与真实行为回归

| Review | 修复 | 旧实现的实际失败 | 修复后 |
|---|---|---|---|
| [锁顺序反转](https://github.com/prematurely/sa-renderstack/pull/1#discussion_r4509025305) | SetObserver / NotifyBridge 统一先取设备锁、再取 observer 锁；回调仍同步执行 | 创建线程 Reset 与另一线程 Present 的回调内 GetAudit 相互等待，进程 watchdog 以 86 退出 | 33 项通过；覆盖同设备 API 重入、并发注销、注销后无回调；原有 final-release 回归继续运行 |
| [辅助设备夺取采集线程](https://github.com/prematurely/sa-renderstack/pull/1#discussion_r4509025311) | 在设置 owner、消费按键或推进状态前排除非选中设备 | 真实辅助 HAL 设备先从另一线程 Present 后，主设备按 F8 无法进入 Baseline | 8 项通过；主设备完整采集并取得真实 CPU 样本 |
| [非 ASCII 模块路径](https://github.com/prematurely/sa-renderstack/pull/1#discussion_r4509025313) | Toolhelp 使用 W API，将 UTF-16 明确转为 UTF-8 | ACP 936 下，混合中文、é、Ω 路径无法无损进入报告 | 9 项通过；从报告路径打开匹配 RSYM，并解析实际 Direct3DCreate9 地址 |

上述回归执行生产 runtime 和实际 D3D9/Vulkan 后端，没有模拟锁、模块枚举或设备状态。
完整 native runtime 回归为 82 项通过。原始 RED/GREEN 日志保存在
`out/review-fixes-20261110/`。

## 全 HUD 布局

原布局在 1280×720、`DXVK_HUD=full` 时会与左下帧时间图重叠。新增回归直接发布八行
诊断快照、执行 Present 并读取实际像素，包含真实格式的长热点函数名和状态统计。
旧布局的完整文本块匹配失败，新布局通过 2560×1600 和 1280×720 两种尺寸。

左列高度不足时，诊断块移到右上列；过长行显示省略号，完整符号仍写入报告。
文本适配仅在新快照或宽度变化时更新，逐帧复用缓存。测试同时检查原有显示/隐藏行为。
两种测试均使用默认 HUD scale。逻辑宽度小于 832、放大 scale，以及特别长的右侧
memory-details 列表未纳入像素矩阵；这些条件下仍可能出现空间不足。
实际截图保存在 `out/review-fixes-20261110/delivery-evidence/`，游戏配置未开启 full HUD。

## 性能对照

基线为已安装的 sampler 候选，SHA256
`1CB76F17306FDF25C40FCC6F822B8B948CE95FE45DD24EF7037D8E1F7EC6C806`。
最终测量 DLL 为
`714BF82766BCABF4D8C42262BDF40B8FD4A13850900FEB4A3570FD3B209E4B22`。
固定 sampler probe 为
`B0B2508A01AAC6C0DD23DC66B9DB8343AC14A8F0AB6377D14AAC62D57FD09554`。
每进程四轮交替顺序，最终 A/B 为 12 个独立进程、1,536 个样本；A/A 为六个进程。
所有行为检查通过，进程和调用线程均保持正常调度。

| 主线程周期变化 | 无多线程保护 | 有多线程保护 |
|---|---:|---:|
| 重复单 setter | -3.85% | +0.11% |
| 变值单 setter | +1.39% | -0.07% |
| 重复批提交 | -12.28% | -7.19% |
| 变值批提交 | -13.56% | -10.93% |
| StateBlock | -5.67% | -5.57% |
| draw 控制 | +0.41% | -1.82% |
| 重复 setter + draw | +0.47% | -0.47% |
| 变值 setter + draw | -0.49% | +0.64% |

sampler 热路径源码和既有真实行为 probe 保持原样。这里记录最终链接二进制的变化，
不把这些差值归因于 review 修复或换算为 GTA FPS。首次控制项波动和第二轮对照也保留在本地证据中。

Present 控制每种模式另做六组 A/B、三组 A/A，每次 128 次预热和 2,000 次 Present。
有 observer 的模式执行真实回调内 GetAudit；每进程检查 2,128 次回调和零提交失败。
最终 A/B：无 observer 的 wall time +0.37%、主线程周期 +8.61%；有 observer 为
+0.49%、-0.98%。对应 A/A 周期为 -5.67%、-3.47%；首轮 observer 周期 A/B 为
+5.79%、A/A 为 +6.50%。Present 周期波动仍大，保留无 observer 的 +8.61% 观测，
不据此宣称整个 Present 吞吐已经改善。[完整成对记录与验证清单](evidence/2026-11-10-review-fixes/README.md)。

复现入口：

```powershell
pwsh -NoProfile -File tests/measurement-backend-test.ps1 -CompileOnly -OutputDirectory out/present-probe
pwsh -NoProfile -File tests/measurement-lifecycle-benchmark.ps1 `
  -Baseline <baseline.dll> -Candidate <candidate.dll> `
  -Probe out/present-probe/measurement-backend-probe.exe `
  -OutputDirectory out/present-ab -Pairs 9
```

## 完整验证与产物

提交 `f6876b2fdf63b4f7f0c79aab069a89eb55c0a735` 的完整本地流水线为
**85 passed / 0 failed / 2 skipped**，没有使用 GPU 或环境测试跳过开关。
两项跳过为没有外部输入的 `live-dxvk-audit` 和 `live-bridge-config-audit`。
87 份逐项日志均已检查完整性并保存 SHA256，详见 evidence 的 `validation.json`。
运行于 2026-11-10 01:33:58～01:41:03（Asia/Shanghai）。

完整流水线重建后的后端 SHA256 为
`FB3A9924D63701FBAE66B8D724F0EEB06AA293A1C02C55F1125C8D50B57B392F`，
Bridge 为 `133C68F352DE05284C3A8B2282ECA48AE54E439B4943803866D1FE2BE1AEF1DF`。
性能数据使用上节明确标识的冻结二进制；提交版本标识更新后重建的二进制另列，未混称同一哈希。

本机 Clang 22 在未修改的 msxml.h 解析中发生过访问异常。本轮仅将编译器子进程临时
限制到 CPU 0；测试、基准和游戏的 affinity 没有改变，也未把此设置写入产品或 CI。
首个完整运行的 83 passed / 2 failed / 2 skipped 证据保留：两项失败来自历史 ABI
检查默认选中了当前游戏 DLL，以及依赖它的旧证据。最终运行显式使用 SHA256 为
`B0BBE0C98B132EA0B8E6BA0FA2C978D82C48063A50C2324BE3A8F7141FE7E9FF`
的原始归档，保留全部断言和关卡。

一次独立 whole-branch 审查覆盖 `659f9ed6..f6876b2f` 全部 23 个文件，未发现
Critical、Important 或 Minor 问题。审查额外检查了生产回调调用方、runtime 锁和
StateBlock 外层锁，并目视核对两张 full HUD 截图。FETCH4 的状态/顺序逻辑未改，
本轮未新增它的专用 shader 像素测试；新像素回归覆盖常规过滤、sRGB 和状态恢复。

游戏目录的两个 DLL、三个符号文件和三个配置文件在本轮前后 SHA256 均相同。
