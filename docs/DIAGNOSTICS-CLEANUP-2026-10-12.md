# 临时监测清理

后续核查：用户已删除独立 `NRBenchDX12` 工程。再次检查插件 `src`、`tools`、构建脚本及第三方源码，没有 GPU 负载、工作频率、温度、功率、NVML/NVAPI 或显存预算采样，也没有外部监测程序启动或跑分工程构建引用。`GetTimestampFrequency` 用于将 GPU 时间戳换算为面板耗时；`GetModelSettings` 仅在面板可见时请求计时。曝光读回属于图像处理，保留。旧备份和报告中的监测描述不进入当前构建。本轮未修改插件源码。

按用户要求，删除插件内这轮排查追加的临时监测：

- ModelRuntime 的成功创建/释放日志、生命周期累计计数、IDXGIAdapter3 保留和显存/预算查询。
- Layer 的 GPU 时间累加、60 样本/10 秒汇总日志。
- Panel 的焦点/Ins/挂接状态日志及输入锁等待计时。
- PassStatus 中无读取方的 timingAllocations/timingReuses 及其递增代码。
- Pass 内为临时日志引入、清理后不再使用的 graphics.hpp 头文件。

保留正常面板 GPU timestamp/读回、Context 与 Layer 同时匹配的计时检查、计时池安全复用、模型及资源的同步/回收、焦点缓存刷新、调用游戏 WndProc 前解锁的修正。Release 失败仍输出简短错误，成功时不写日志，不重试已经释放的句柄。

Release 构建与链接通过，无 MSVC warning/error。独立 NRBenchDX12 使用清理后的共享生产源码重新构建并短测：创建/释放 3/3、失败 0；511 对计时核对，平均差 0.000225952 ms，最大 0.000704339 ms；正常 runtime.log 为 0 字节。该短测只验证清理后模型路径和计时可运行，不覆盖游戏窗口或 hook。

清理当时保留了独立测试程序及其采集，测试采集不进入生产 DLL 的正常执行路径；该工程现已由用户删除。构建日志：`build/diagnostics-cleanup-build-release.log`；源码与二进制哈希：`build/diagnostics-cleanup-hashes.csv`；原短测位置为 `../NRBenchDX12/results/cleanup-pass/`，现已不存在。

新主体 DLL SHA256：`F3FB5E3D90D2758BB06982BC22F8BE3B11F79A01D4D0DBF3F6AAD4A657597F51`。最初仅更新工作区 `dist/Release`；用户随后授权，已于 01:58 部署绝地。本轮只读核对游戏目录主体 DLL 与此哈希一致。部署记录见 `build/deployment-jedi-20261012-diagnostics-cleanup.json`。
