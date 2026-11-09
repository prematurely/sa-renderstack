# 通用 sampler / 状态批提交优化验证

## 范围与结论

本文归档 2026-11-09 首次本地验证时的状态。基线为 main
`659f9ed60084f16dba98bb4cd396edeca2ab17cc`；当时的候选位于本地分支
`codex/sampler-state-batching-20261109`，尚未部署、提交或发布。
后续提交与 review 修复验证见 [2026-11-10 报告](review-fixes-20261110.md)。
普通 sampler setter 在持有原有设备锁、排除 StateBlock 录制后，从权威设备状态判断
同值调用；诊断未开启时跳过 audit scope 设置。已有 API5 批提交和 StateBlock Apply
复用外层设备锁，避免每项重复进入设备保护逻辑和已经完成的 sampler 参数验证。
两个内部入口共用编译期锁策略的实现，编译时内联，避免普通变值调用多一层函数调用。

本轮没有新增状态镜像、异步队列、MOD 匹配或运行时依赖。SDK/COM 接口、配置、
Vulkan 暂存、资源复制和上传路径不变。保持顺序应用每个变值，保留原有脏状态传播、
Journal 首次捕获、FETCH4/MAGFILTER 交互和状态录制语义。

## 可重复设备测量

固定同一 x86 Release probe、相同工具链和设置。正常调度，不修改 benchmark/game
亲和性、优先级或系统设置。每次进程创建两个真实设备，交替 AB/BA 和设备创建顺序，
每进程 4 轮，12 个独立进程。下表为“每进程成对变化中位数，再跨进程取中位数”；
负数表示候选开销降低。批提交每次含 16 个 sampler 状态。

| 场景 | 无多线程保护：主线程周期 | 有多线程保护：主线程周期 | 有保护：QPC 耗时 |
|---|---:|---:|---:|
| 同值单 sampler setter | -0.76% | -5.57% | -5.22% |
| 变值单 sampler setter | +0.35% | -0.44% | -0.53% |
| 同值 sampler 批提交 | -5.54% | -38.48% | -38.19% |
| 变值 sampler 批提交 | -2.39% | -34.86% | -35.17% |
| 交替 StateBlock 恢复 | +0.02% | -40.73% | -39.53% |
| draw 控制 | +0.27% | +0.10% | +0.02% |
| 同值 setter + draw | +0.50% | -1.36% | -1.08% |
| 变值 setter + draw | +0.16% | -0.59% | -0.73% |

有保护的重复 setter、两种 batch 和 StateBlock 在全部 12 个进程中线程周期更低。
同一 baseline DLL 的两个副本、相同 probe 的 6×4 A/A 中，所有场景周期变化中位数
为 -1.24%～+1.96%；因此不把无保护同值 setter、单次变值或 draw 的小差异算作确定收益。
最终控制项没有出现首轮那种一致的明显回退。完整原始数据保存在
`out/perf/20261109-sampler-batch/ab-v3/` 和 `aa-final-probe/`。
每个 A/B 进程 45,390 项检查通过、128 个计时样本，合计 1,536 个样本。
CS 快照和完成时间也保留；系统 CPU 时间粒度不足以由这些短样本推算精确 CS 占用。

迭代记录没有删去：v1 的普通变值调用周期回退约 5.57% / 10.68%，反汇编确认多了一层
调用，因此拒绝；v2 消除该调用，但无保护变值 batch / StateBlock 回退约 2.5%，也未接受。
v3 只在普通 setter 放置提前同值分支，批量入口使用单次相等判断，消除了这些控制回退。

## 真实行为回归

新增 native probe 直接加载后端，不加载游戏或 MOD，验证全部 21 个 sampler 槽位、
13 类原始状态、同值/变值 StateBlock 录制、录制期间 live state 不变、重叠 Journal
首次捕获、StateBlock 在 Journal 内应用、批次先验证后修改、重复条目最终值、Reset、
active audit 同值计数和录制调用不计数。真实像素覆盖点/线性、wrap/clamp、边框色、
sRGB、StateBlock/Journal 恢复；并发批次和 StateBlock 快照验证批次锁覆盖。

审查前基线和候选均通过 compatibility 开/关 × 多线程保护开/关四种组合：compat 开启
分别 9,734 / 10,952 项，关闭分别 3,003 项（模块清单可能使检查数增加 1）。
暂时放入一个跳过同值录制的错误实现时，真实回归出现 823 项失败，包括状态恢复和像素；
错误实现只留作 `negative/` 证据，从未部署。FETCH4 序列本轮检查原始状态和顺序，
没有新增 FETCH4 shader 像素测试；相关脏状态逻辑逐字保留。

独立审查未发现产品正确性缺陷，指出过滤/sRGB 恢复后的像素覆盖不足。已补充普通与
批提交两条路径下的 StateBlock/Journal 恢复：红蓝纹理 point → linear → point，
灰纹理 sRGB on → off → on。故意丢弃这些状态恢复的临时后端产生 524 项失败，明确
包含 8 次新增恢复像素错误：过滤仍为 `0xff7f007f`，sRGB 仍为 `0xff808080`。
错误后端保存在 `negative-restore/`，正确源码在 finally 中按原始字节恢复并核对哈希。

完整流水线已接入 `sampler-batch-native` / `sampler-batch-no-compat`。最终运行
`tools/test.ps1` 的 86 项逐项日志为 **84 passed / 0 failed / 2 skipped**，没有使用
GPU 或环境测试跳过开关。两项跳过仅为未提供外部源码的 `live-dxvk-audit`、未提供
外部 Bridge/config 的 `live-bridge-config-audit`。新增像素回归在正常调度下，基线与
候选均通过四种设备模式：compat 关闭 3,089 / 3,089，开启 10,068 / 11,286 项。
完整流水线中的最终重建 DLL 也通过对应回归；模块清单可能使检查数增加 1。

构建环境尝试均保留：首轮正常调度 80 passed / 4 probe 编译失败 / 2 skipped。
将整个验证子进程树限制到 CPU 0 后为 83 passed / 1 failed / 2 skipped；唯一运行期
失败是 mixed-device 测试未观察到要求的 CS 等待，不能把该轮算作通过。同一 probe
恢复正常调度后，基线与候选均 1,881 项通过。最终只限制本轮 Clang 子进程到 CPU 0，
运行期保留正常调度（父进程 affinity `4294967295`），全部必需 gate 通过。
没有修改测试断言或把 affinity 限制写入产品/测试脚本。

工具重连后，最终自动 JSON 和 compiler event 汇总变为全零字节。原文件保留；所有
86 项 combined log 均非空，84 项具有完整 `exitCode=0` 尾记录，2 项明确记录跳过原因。
`out/perf/20261109-sampler-batch/full-validation-recovered.json` 明确标记为从逐项
日志恢复，保存每项路径和 SHA256；没有将旧轮成功项拼接冒充新轮结果。最终一轮是
PID 10472，结束于 2026-11-09 12:10:48（Asia/Shanghai）。

## 构建与产物

基线 DLL SHA256：`5BC02EFF844EFA1A5178D1C8B2621885496DB6218E2BF0A740306ED88A1B3FFD`。
A/B v3 DLL SHA256：`78FE93E3A2CEECD215914F5F2C9457DA6E4008C0FC815628EC81A7CB8922FB94`。
最终重建 DLL SHA256：`1CB76F17306FDF25C40FCC6F822B8B948CE95FE45DD24EF7037D8E1F7EC6C806`。
二者只在 COFF 链接时间戳和 `.buildid` 内容不同，排除这两项后的整文件 SHA256 相同，
代码、数据、导入及异常处理等内容逐字节一致。验证脚本和结果保存在
`verify-binary-equivalence.py` / `final-binary-equivalence.json`。
最终 DLL 和哈希匹配的 `.rsym` 保存在 `out/perf/20261109-sampler-batch/candidate-final/`。
导入比较共 264 项，无新增或删除；原有 7 个 Windows DLL 导入模块相同。

正式 `tools/package.ps1` 按既有规则拒绝未提交工作区；本轮保留可审查的未提交改动，
没有绕过正式发布规则。仅整理本地后端候选和来源/验证清单，不将其称为正式发布包。
游戏目录原有 8 个 DLL、配置和符号文件在本轮前后哈希相同。

本机 Clang 22.1.7 的两次正常调度 v2 构建分别在未修改的 ResolveZ 代码生成、LockImage
解析时异常退出；日志和预处理崩溃复现文件均保留。同一源码在仅限制构建子进程到 CPU 0
后成功，v3 采用相同构建诊断设置。这不证明编译器或主机问题的根因已经修复。
独立设备回归与 A/B 使用正常调度；最终完整流水线仅临时限制 Clang 子进程到 CPU 0。
未把限制写入产品、测试脚本或构建脚本。编译器异常根因仍未确认。

## 验收边界

当前证据只证明本机设备 fixture 的主线程提交成本变化及回归通过，不能换算为 GTA FPS，
也不能认定出门长帧改善。该长帧曾包含纹理复制/缺页路径，本轮按要求保持它们不变。
游戏画质和切换体验仍需后续授权部署后，在同一存档、路线、视角与等待时间下验收。
