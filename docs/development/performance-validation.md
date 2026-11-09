# DLL 性能改动的验证方法

优先降低 CPU 端重复工作，并保留画质、公开 COM/SDK 接口与状态恢复语义。
构建产物保存在 `out/`；性能验证不会安装 DLL，也不会修改游戏配置。

## 当前单项绑定优化

启用 `d3d9.gtaSaCoalesceSamplerBindings` / `d3d9.gtaSaCoalesceTextureBindings`
时，原实现即使只有一个 dirty slot，也构造并捕获完整的 21 项数组。
`TryD3D9SingleBinding` 对非零单 bit mask 复用现有 `BindSampler` / `BindTexture`，
避免该数组的初始化、移动和命令载荷。零项、多项继续使用原批处理。
单项函数与批处理继续共享现有 binding cache、sampler key 和资源引用语义。
此改动没有新增运行时配置开关。

第一批还测量了常量比较与更新共用扫描的方案。其尾部变化微基准有收益，但部分
不变/整段变化和组合 draw 场景出现回退，因此已撤回全部常量生产代码改动。
新增的常量行为回归保留，避免后续优化破坏原行为。

截至第二批，控制场景复验确认三项 16-register 不变调用存在稳定回退。曾测试比较循环对齐、
局部更新扫描和双寄存器比较，部分控制场景有收益，但均在完整 on/off 矩阵引入其他
回退，未达到保留标准。全部第二批常量生产代码实验已撤回，继续保留第一批已验证的
单项绑定优化。三项已确认的控制回退仍未修复，不能将本批工具和回归补强视作 FPS 收益。
实验 DLL、二进制布局诊断和原始测量记录保存在本地 `out/perf/20261022-controls/`。

## 第三批：常量控制布局修复（2026-10-22）

三项已确认的 16-register 不变控制回退已恢复到第一批之前基线附近。本轮保持比较、复制、
状态记录、journal 和 audit 算法及公开接口；修复 x86 Clang 构建中受相邻函数大小影响的热点布局。
PS/int 早期比较使用 `code_align(64)`，VS float 保持已测的 16 字节位置。VS int 在设备 setter 的
journal 捕获之后、更新调用之前加入预头对齐 directive，绕过当前 while 对齐 metadata 的丢失。
指令布局由正常编译、汇编和链接生成；生产 DLL 不进行运行时代码改写，也不修改游戏调度设置。

最终六个私有 setter 的有效指令和控制流经忽略 NOP/重定位后均与基线一致。
共享 helper 内的早期 directive 候选引入过额外指针 spill，虽然行为测试通过，仍造成约 4.68%
短写入回退，因此拒绝。将 directive 移至设备调用点后，栈帧恢复为 8 字节，spill 消失，
该 case 正式普通调度复验约 +0.66%，固定调用线程诊断约 -0.62%。

固定公开 COM 入口、设备、数据和 clone allocation 的受控实验支持整段代码布局具有因果影响。
只移动孤立 27 字节循环没有复现 12–15% 回退，故不把问题简单归因为循环头的单一对齐值，
也不声称已确定具体 CPU 取指/解码/分支预测机制。

`tests/constant-layout-test.py` 读取最终 PE/COFF 并反汇编，检查四个比较循环和 VS int 更新循环的
已测位置，作为 required gate。它不是整函数指纹检查；即使该 gate 通过，编译器或代码结构改变
仍需重复 A/B。新增逐 lane 真 COM 测试验证每个首次变化位置、单 bit 特殊值、4/8/12 字节输入偏移、
完整 bank 哨兵和相同重写；每种配置共 7567 项检查。

最终候选完整流水线 54 passed / 0 failed / 2 外部审计 skipped。普通调度 12 进程×4轮、on/off 各
78 case 的正式 A/B 已记录；三项目标 on 调用耗时约改善 10–13%，历史基线对照约在 -1.70%～+0.36%。
某个尾部变化 case 在普通调度点估计约 +5.18%，固定调用线程复验约 +0.16%，对应 A/A 约 +1.41%；
不将这些点估计写成零，也不据此宣称所有 workload 或 GTA FPS 都有提升。

本机完整原始证据和候选边界见 `out/reports/performance-controls-rootcause-20261022.md` / `.json`。
游戏测试固定为葛洛夫街，重点测街区切换与室内到室外；微基准不证明该瓶颈已经解决。

## 真实行为回归

- `renderstack-dxvk-state-dedup-test` 在 Release 中使用始终有效的检查，验证空 mask、
  所有 21 个单 bit mask 的槽位与调用次数、多 bit mask 的回退。
- `api-state-runtime-tests` 通过真实公开 COM 调用验证 float/int 的 bitwise 往返、
  NaN、正负零、未按 16 字节对齐的数据、最大范围、状态块和 journal。包含选择性捕获
  关闭前后的重叠写入，明确锁定完整原范围快照。
- 逐 lane 回归逐一改变 16 个寄存器的每个分量，使用 4/8/12 字节错位输入；首次写入后立即全 bank 读回，再单独验证相等重写。
- 常量扫描边界回归遍历每种 stage/type、多个奇偶长度及所有首次变化位置；首次变化
  后立即读取整个常量 bank，再单独验证相等重写，包含提交范围前后的 canary。
- `constant-submit-benchmark ... check` 在真实 D3D9/Vulkan 设备上运行 shader，
  使用 event query 等待并读回像素。测试采样槽 0/5/15，红/蓝纹理交替、棋盘纹理的
  点/线性过滤、边框色、解绑、多槽加权采样，以及 stateblock/journal 恢复。
- `tools/test.ps1` 在 reference / optimized 两种配置运行上述 GPU 回归。
  `-SkipGpuRuntimeProbes` 会明确记录对应的 skip，不能作为 GPU 验证通过。

## 可重复的 A/B

先保存使用相同工具链构建的基线与候选 x86 DLL，再使用同一个 Release probe：

```powershell
pwsh -NoProfile -File tests/constant-submit-benchmark.ps1 `
  -Baseline <baseline-d3d9.dll> -Candidate <candidate-d3d9.dll> `
  -Probe out/build/api-projects-x86/Release/constant-submit-benchmark.exe `
  -OutputDirectory out/perf/binding-ab -Pairs 12 -Dedup on
```

`-Dedup on` 对应 probe 的 `on` 配置，同时开启常量去重、两项 binding coalesce 和
resource binding cache；`off` 同时关闭这四项。每个配置中基线与候选使用相同设置。
脚本不修改优先级、亲和性、系统电源计划或驱动设置。

每组交替 AB/BA 进程顺序，预热后记录每次提交的 QPC 时间。每个进程应输出 78 个
固定 case：68 个常量 setter case、5 种 draw 序列及各自包含 GPU 完成的时间。
draw 序列包含仅常量、单纹理、单采样器、单槽纹理和采样器、两个活动槽同时切换。
shader 实际使用纹理；计时纹理为相同白色，颜色差异在独立像素回归验证。

结果 JSON 保存 DLL/probe 哈希、原始样本、中位数、成对变化中位数和更快的组数。
脚本检查每个进程的 case 集合、重复、迭代次数及时间有效性。比较各序列，避免仅选取
最快的微基准。不要并行运行编译、其他 GPU 测试或另一份基准来污染采样。

小型合成场景不能证明 GTA 的 FPS 提升。只有用候选在相同游戏场景测量帧时间，才能
判断最终 FPS 收益；不安装候选时应明确保留这项边界。

## 控制场景的配对复验

跨进程 AB/BA 仍可能受调度和频率变化影响。第二批用同一 DLL 做旧流程 A/A 时，
曾出现约 31% 的组合 draw 假差异。因此新增同进程配对模式，先校准再解释 A/B：

```powershell
pwsh -NoProfile -File tests/control-path-benchmark.ps1 `
  -Baseline <baseline.dll> -Candidate <candidate.dll> `
  -Probe out/build/api-projects-x86/Release/constant-submit-benchmark.exe `
  -OutputDirectory out/perf/control-ab -Processes 12 -Rounds 4 `
  -Profile on -Cpu any -Cases all
```

- `pair` 在同一调用点测量两个真实 D3D9/Vulkan fixture，按 seed 打乱 workload
  顺序、轮换 DLL/device 创建顺序、每轮交替 AB/BA。每个 fixture 先完成 127 项真实
  行为检查，包括 shader 像素读回。相同路径共享同一设备；不同路径要求模块基址不同，
  并完成 254 项检查。需要同时测量共享设备 A/A 和相同内容的双模块 A/A。
- `all` 包含原有全部 78 个 case；`controls` 包含 9 个重点控制 case。每个进程先对
  各轮成对百分比取中位数，再跨独立进程取中位数；报告保留各进程变化和全部样本。
- `Cpu any` 为正常调度；可用 `Cpu 0` 做定位诊断，仅临时固定 probe 调用线程，退出
  时恢复原 affinity。它不改变 DLL 调度策略，也不修改游戏、进程优先级、电源或驱动。
  QPC 给出 ns/call；QueryThreadCycleTime 单独记录 cycles/call，不将 cycles 换算成时间。
- runner 严格校验检查数量、元数据与模块拓扑、固定迭代数、case/round/variant 完整性、
  有限正计时及固定 CPU 一致性。拒绝重用已有输出命名空间，运行前后按路径检查
  DLL/probe 的 SHA256，避免把旧成功报告与新原始数据混合。
- `control-path-protocol-test.ps1` 用真实设备输出的变异副本验证缺少检查、错误迭代、
  重复/缺失样本、错误模块/affinity 元数据等会失败。布局诊断脚本仅保存在实验记录中，
  对齐方案已拒绝，因此不把它作为最终版本的 required gate。
- 完整流水线在 reference/optimized 配置各运行共享和双模块 pair、协议回归与原有
  状态和像素测试。CI 只验证行为和输出完整性，不使用机器相关的计时阈值。

## 工程参考

- [NVIDIA Nsight 工作流](https://docs.nvidia.com/nsight-graphics/UserGuide/expected-workflow.html)：
  先确定 CPU/GPU 瓶颈，再定位对应开销。
- [Microsoft D3D9 API profiling](https://learn.microsoft.com/en-us/windows/win32/direct3d9/accurately-profiling-direct3d-api-calls)：
  状态调用可能延迟到 draw，需测量包含 draw 的序列和完成边界。
- [DXVK PR 5686](https://github.com/doitsujin/dxvk/pull/5686)：
  常量管理优化的微基准收益不能直接代替游戏 FPS 验证。
- [Google Benchmark 降低方差](https://github.com/google/benchmark/blob/main/docs/reducing_variance.md)
  和[随机交错](https://github.com/google/benchmark/blob/main/docs/random_interleaving.md)：
  控制系统噪声，并用交错重复测量评估优化。
- [Clang code_align](https://clang.llvm.org/docs/AttributeReference.html#code-align)：
  对循环指定代码对齐；实际收益仍由最终二进制的本地 A/B 决定。
- [Microsoft QueryThreadCycleTime](https://learn.microsoft.com/en-us/windows/win32/api/realtimeapiset/nf-realtimeapiset-querythreadcycletime)：
  原始线程周期计数不应直接换算为耗时。

布局 directive 参考：[GNU assembler p2align](https://sourceware.org/binutils/docs/as/P2align.html)。
它请求汇编位置的边界，并不保证某个 C++ 语法节点在最终代码中的位置；因此以链接后检查和 A/B 为准。

## 当前候选的验证记录

2026-11-09 发布快照包含下述历史工作及后续诊断、状态生命周期和上传映射改动。
最新上传/重试候选的实际测量、回退数据和验收限制见
[2026-11-08 上传与重试报告](upload-state-20261108.md)。其中的 CPU 周期收益
不代表游戏 FPS 或整个 Present 流程的吞吐收益；该快照通过源码/行为验证后发布为候选。

## 纹理绑定与资源生命周期验证（2026-10-27）

NVIDIA Skills 当前目录没有直接面向本项目 D3D9/DXVK DLL 的源代码优化技能。本轮使用其
技能发现流程，并参考上面的 NVIDIA Vulkan / Nsight 方法，先验证已有 CPU 成本，不引入
新的运行时 SDK，也不修改驱动设置或游戏安装。

`constant-submit-benchmark <dll> on|off lifetime` 在真实 D3D9/Vulkan 设备上执行 423 项检查。
它覆盖单槽与双槽的真实换图、无 draw 的换出/恢复、stateblock/journal 恢复、线性/sRGB
视图、释放应用持有的纹理后再显式等待 GPU，以及 Reset 后重复验证。队列生命周期用多个
不同 RT 保存各次 draw，后续 draw 会替换旧缓存 view，避免最后一个缓存引用意外掩盖问题。
这不是对所有可能线程调度交错的穷举。新模式独立于原有 check / bench / pair 的检查数量。
混合命中像素检查分别验证“第一槽命中、第二槽换图”和相反次序，避免只用白色计时纹理
掩盖 pending 数组压缩后的错误槽位。重复计时使用归档、固定哈希的 benchmark 可执行文件；
随后追加生命周期检查时不改写该归档计时文件。

配对基准新增 `-Cases textures`：单槽缓存命中、双槽命中、混合命中、sRGB 切换，以及原有
单槽/双槽换绑控制，CPU 提交与 GPU 完成合计 12 个 case。旧 `controls` 的 9 个和 `all`
的 78 个 case 保持不变；输出仍经过完整性、迭代数、哈希和模块拓扑检查。

```powershell
pwsh -NoProfile -File tests/control-path-benchmark.ps1 `
  -Baseline <baseline.dll> -Candidate <candidate.dll> `
  -Probe out/build/api-projects-x86/Release/constant-submit-benchmark.exe `
  -OutputDirectory out/perf/texture-ab -Processes 12 -Rounds 4 `
  -Profile on -Cpu any -Cases textures
```

命中路径改善必须同时检查换绑控制和原有常量矩阵；不能把局部减少引用计数直接写成
GTA FPS 收益。链接后机器码检查只补充成本证据，不能替代像素、生命周期和重复计时。

本轮标量、指针快照和仅批量三种候选均未达到控制场景的保留标准，生产源码已逐字节恢复
到本轮输入。保留的是上述真实行为回归、纹理 workload 与协议验证，不是新的 DLL 性能收益。
尝试用过的原子操作机器码诊断脚本及拒绝候选保存在
`out/perf/20261027-nvidia-binding/`，不将未保留优化的成本约束注册成稳定版本的必需 gate。
