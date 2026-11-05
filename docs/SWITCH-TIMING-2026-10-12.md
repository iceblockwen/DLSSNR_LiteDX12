# 多次切换后 GPU 耗时增长：当前证据与诊断

后续状态：用户要求清理多余监测，本页描述的临时生命周期/显存/GPU 汇总日志已从生产源码移除，正常面板 GPU 计时及配套正确性修正保留。见 [清理记录](DIAGNOSTICS-CLEANUP-2026-10-12.md)。以下为当时诊断及历史日志证据。

用户确认：面板 Completed NR GPU 的总耗时增长，切回最初相同预设/风格并等待几秒后仍不恢复。因此不能用“风格成本不同”或切换瞬间的旧计时解释持续增长。

## 已确认的事实

- Jedi 最新启动段为 2026-10-12 00:01:29，PID 11832。检查时该段有 18 条成功创建后的 waiting 日志，没有 capture slots busy 或已记录的模型失败。
- 总耗时来自同一份 GPU recording 的起止 timestamp 差，每次读取覆盖上一次值，没有将切换次数、创建成本或历次模型耗时相加。
- 创建帧不记录这些计时；计时涵盖后续评估帧内的 guide 转换、encode、按需白点统计、模型和 resolve。
- 按现有所有权链，Recording 分别持有 Context 和它实际使用的 Layer。旧 Recording 在 fence 完成后移除，旧 Layer 进入 parked 回收，再调用模型 Release；没有查出随正常切换次数无限保留旧 Layer 的引用环。
- 但当前运行日志不记录 Release 调用结果、创建/释放数量或实际 GPU 耗时，无法证明每个旧模型都释放成功。原回收代码也忽略 Release 返回值；这会丢失排查释放失败所必需的证据。

## 已修正的计时问题

上次将相同尺寸的工作纹理改为复用 Context 后，旧的 `completed->context == context` 判断不再能区分模型。旧模型的已完成 recording 可以短暂覆盖新模型计时，这是上次修改漏掉的配套检查。

本轮要求 Context 和 Layer 都匹配，才读取该模型的时间。它修复旧模型计时串入，但旧录制在正常完成后是有限数量，因此不能单独解释用户描述的持续增长。没有把这项计时修正称为实际 GPU 性能问题已经解决。

## 本轮增加的证据

`NR feature create` / `NR feature release`：

- handle、NGX 返回码、进程 uptime；创建时同时记录 preset、style、尺寸。
- `created`：模型 Create 返回的非空句柄数量，包括失败但仍返回句柄的情况。
- `released`：Release 返回成功的数量；`release_failures`：Release 返回非成功的数量。
- `process_local`、`budget`：对应 adapter/node 0 的进程本地显存使用量和预算，单位 MiB。Query 不可用时为 -1，同时记录 HRESULT。

这些只在创建/释放时查询和写日志，不在每帧查询预算。[Microsoft 文档](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/ns-dxgi1_4-dxgi_query_video_memory_info)说明 CurrentUsage 是整个应用进程的使用量，所以它包含游戏自身资源，不能单凭这个数字增长就认定 NR 泄漏。

`NR GPU timing`：沿用面板已经开启的 timestamp 查询，记录该 Layer 的平均总耗时与模型耗时；至少 60 个有效已完成样本，首次报告后最多每 10 秒报告一次。没有新增 GPU timestamp、compute dispatch 或图像处理。

没有改变 Create/Evaluate/Release 的调用顺序、队列同步、回收判定、shader 或 UI。没有替游戏调用 NGX Shutdown，也没有自动重试失败的 Release：本地 NVIDIA SDK 明确说明 feature 句柄不计引用，Release 调用后不能再使用原句柄；不能为了“修泄漏”盲目重复释放。

## 如何区分原因

在同一场景、相同分辨率、同一预设/风格，保持面板开启，记录 A 的基线；A→B→A 重复几次，每次回到 A 停留至少 10 秒。

- 创建与成功释放的差值长期增大，或 release_failures 非零：继续追模型退役/释放链，不能认为正常缓存。
- 配对正常，但进程显存随反复 A→B→A 增长且不回落：需要区分驱动/模型内部保留与游戏自身分配；释放返回成功并不等于已证明显存一定立即下降。
- 模型平均时间增长：问题位于模型执行或其受到的 GPU 资源竞争；不能用修正显示数字当作解决。
- 模型平均时间稳定而总时间增长：追 guide/encode/按需统计/resolve 及 GPU 竞争，避免把所有增长归到模型。

这些是待验证的分支，不是已查实的根因。真实游戏由用户运行。

## 构建

功能改动：`src/nr/pass.cpp`、`src/nr/model_runtime.cpp`、`src/nr/model_runtime.hpp`。Release 构建和链接通过，无 MSVC warning/error；Ninja dry-run 为 no work to do。

诊断版主体 DLL SHA256：`2EC485E96AB42734EA20FB6F7C1329DF4835C2C5CFA180EA9D147CDE711A12AE`。源码修改前备份：`build/switch-timing-backup-20261012/`；最终构建日志：`build/switch-timing-build-release.log`。

构建时没有该诊断版的游戏运行日志。后续运行与 Opt 对照证据见下节；持续耗时增长的根因仍未确认。

## 部署

沿用上一轮部署授权，2026-10-12 00:20:04 +08:00 已部署到 Jedi：`G:\EAGames\Jedi Survivor\SwGame\Binaries\Win64`。两个 DLL 与 Release 产物 SHA256 一致；现有 INI 与模型部署前后 SHA256 不变。部署记录：`build/deployment-jedi-20261012-diagnostics.json`。未启动游戏。

## 后续实际日志与 Opt 对照

用户确认两边现象一致。只读检查到 Opt 测试段 2026-10-12 00:44:38–00:46:53，版本 `v10.0.0-dev (9918b95)`，Jedi 同一游戏、3840×2160 输出、2560×1440 guides、preset 0。当前模型版本 `310.8.0.0`，SHA256 `E16BCF15E16E13F527491CDF7845B2FE6521A738D8F7C9C721866A8496E1FC8E`，与此前 LiteDX12 部署/运行的模型一致。用户已切换游戏安装到 Opt；此前 LiteDX12 部署记录属于历史，禁止直接覆盖当前 Opt 测试安装。

| 证据 | 先前样本 | 后续同风格样本 | 增长位置 |
| --- | --- | --- | --- |
| LiteDX12 preset 0 / style 2 | total 7.806、model 7.632 ms | total 8.070、model 7.894 ms | 插件其余计算约 0.174–0.180 ms，基本不变 |
| Opt preset 0 / style 0 | 00:45:48 total 7.98、model 7.75 ms | 00:46:40 total 8.16、model 7.92 ms | Opt 其余计算约 0.23–0.24 ms，基本不变 |

LiteDX12 的旧句柄 Release 记录都返回成功，最终 created=11、released=10、release_failures=0；尚有当前活动模型，不能把这一个差值算作泄漏。反复切换阶段 process_local 约 7809–7812 MiB，没有随着次数大幅堆积。原运行日志已被用户切换 Opt 时移走/删除，工作区保留只读分析副本 `build/switch-timing-latest-session.log` 和 CSV；Opt 副本在 `build/opti-switch-comparison-20261012.log` 和 CSV。

LiteDX12 第一次切换前，同一个句柄的 total 已在 20 秒内由 7.806 上升到 7.981 ms，说明耗时变化不只发生于 Create/切换事件。Opt 切回 style 0 的样本依次为 7.98、8.09、8.04、8.13、8.16 ms，存在上升与回落；这段约一分钟日志不能证明它会随切换无限增加或永远不恢复。

因此可以确认这不是 LiteDX12 独有的现象；模型计时区间及其受到的 GPU 竞争是优先排查范围。两边共有 NVIDIA 模型、驱动、游戏负载，也可能共有接入约定问题，不能据此断言插件完全无关，更不能断言 NVIDIA 模型泄漏。

此前只读 `nvidia-smi` 在用户测 Opt 时采到过 570 W / 570 W、99% GPU 利用率和 SW Power Cap=Active，随后低负载时不再 Active。这些不是 LiteDX12 那轮的同期样本，不能据此认定增长源于功率限制。[NVIDIA 的定义](https://docs.nvidia.com/deploy/nvml-api/latest/api/group__nvmlClocksEventReasons.html)中，SW Power Cap 表示频率被调整以满足功率上限；后续需要把这种状态与同一轮 NR 计时同时记录，才能验证频率变化是否解释增长。

下次有效对照应先在同一场景/分辨率/帧率限制下保持单一风格至少 60 秒，记录稳定基线，再做 A→B→A；同时读取频率与功率事件。先验证不切换时是否同样漂移，避免把随运行时间的变化误判为切换累积。

本次只读游戏日志/文件哈希，保存工作区报告与分析副本；没有修改游戏、模型回收/执行代码、显卡设置，也没有部署。用户要求暂停游戏写入和部署仍有效。
