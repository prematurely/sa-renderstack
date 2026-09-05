# M1: 可选的现有线程调度

第 17 章是研究提案。本次先实现 M1 中能在现有线程生命周期内验证的部分；M2–M5 继续作为独立工作，不把代码片段或预估收益当成已验证实现。

## 范围与设计

- 产品源码是 `SA-RenderStack`，基于当前工作树继续开发；已有修改先保存到 `out/m1-baseline/preexisting.patch`，不改游戏根目录 DLL、INI 或旧源码快照。
- `[Affinity] PerThread=0` 是新模式总开关，`Mmcss=0` 是 CS 线程的附加开关，两者默认关闭。根 `SA.RenderStack.ini` 优先，不存在时回退 `scripts/BridgeD3D9.ini`；Bridge 所有配置和 backend 调度读取规则一致。
- 公共 Windows helper 使用 CPU Sets 软亲和性；按 EfficiencyClass/CoreIndex 识别性能核和 SMT，同一性能等级内按稳定拓扑排序。device-owner 选择第一物理性能核的一个可用逻辑处理器，CS 选择第二物理性能核，后台/编译线程仅在异构且存在低效率等级核时选择低等级集合。
- 尊重进程/线程 affinity 的交集，排除 parked/其他进程专用 CPU set。不支持的拓扑（包括本轮的多组、x86 无法表示的 mask）、不足的核心、API 缺失或错误，保留 OS 调度并记录原因；不得扩宽进程 mask。
- CPU Sets 不保证独占、不会禁用 SMT 同胞，不是隔离游戏/渲染线程的数据依赖。
- MMCSS 仅在 DXVK CS 自身的局部作用域注册 `Games`、`AVRT_PRIORITY_NORMAL`，退出/异常时同线程注销；不设置 TIME_CRITICAL、Pro Audio 或实时进程类。device-owner 本轮仅设置 CPU Sets，因 unwrapped 模式没有可控的主线程退出接口，不为其申请 MMCSS。
- 初始化发生在成功 CreateDevice 的线程（只登记一次）、CS 函数及自有后台函数入口；不强制启用逐调用 wrapper，不改变 draw 顺序或 RenderWare 主循环。
- 既有硬编码调度属用户当前未提交改动：新模式关闭保留 legacy 行为；开启时跳过 backend 的 `0xFFFF` 进程限制，后台线程改用拓扑策略，Bridge 仅 startup 按显式进程配置设 mask，后续不周期覆盖。保留已有 LFH 和其它无关修改。
- 所有 helper 初始化失败都不得使设备创建/CS 线程失败；恢复 CPU Sets 前确认 assignment 仍是本模块设置的值，避免覆盖第三方后续修改。

## 非目标

不引入 TLS early hook、全局堆替换、未经验证的游戏地址 patch、新 render thread、DQS、AVX/FMA 数学替换或不存在的多 draw API。性能收益只通过后续固定场景 A/B 判断。

## 验证

纯策略测试覆盖异构/同构、SMT、乱序、单核、mask 冲突、不可用 core；Windows 自有短命测试线程验证 opt-in 应用与恢复、默认关闭及失败回退；双编译器 x86 构建验证；现有 Meson/Bridge/API 回归。游戏画面和 FPS 验收单独标记，不能由单元测试代替。

## 官方依据

- https://learn.microsoft.com/en-us/windows/win32/procthread/cpu-sets
- https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-system_cpu_set_information
- https://learn.microsoft.com/en-us/windows/win32/api/avrt/nf-avrt-avsetmmthreadcharacteristicsw
- https://learn.microsoft.com/en-us/windows/win32/api/avrt/nf-avrt-avrevertmmthreadcharacteristics
- https://learn.microsoft.com/en-us/windows/win32/procthread/multimedia-class-scheduler-service

以上网页于 2026-09-05 直接读取。搜索工具返回 HTTP 404，故通过 HTTPS 直接读取 Microsoft Learn 原文。
