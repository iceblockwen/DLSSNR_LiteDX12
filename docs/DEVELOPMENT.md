# DLSSNR_LiteDX12 —— 架构与当前状态

本文是**架构锚**：任何改动先对照这里。规则先于文件，文件先于实现。

现状流程图（加载 / 接线 / 每帧两条链）与逐单元重构顺序见 [ARCH-FLOW-2026-11-05.md](ARCH-FLOW-2026-11-05.md)。

## 目标与范围

Windows x64 的 **Direct3D 12 only** DLSS Neural Rendering 插件（个人使用）。

- 唯一输入源：**游戏自己**的 `NVSDK_NGX_D3D12_*` DLSS / DLAA 调用（我们只旁观）。
- **不做**无 DLSS 的合成路径（不需要 backbuffer 抓帧、深度查找、MV 估计）。那套留在 `DLSSNR_Lite`。
- 不做 upscaler 替代、不做 spoofing / FrameGen / Hudfix。那是 OptiScaler 的范围。
- 不含测试工程。

## 六条架构规则

| # | 规则 | 由来 |
| --- | --- | --- |
| R1 | **一帧只有一条路径**：`observe → capture → guard → pass → 提交 → 退役`。没有第二条入口。 | 观察与管线曾糊在一起 |
| R2 | **渲染线程内不阻塞**。唯一允许的等待是面板关闭 / Resize 的**有界**等待。 | 掉帧要变成"跳过本帧"而不是"卡住" |
| R3 | **每帧可能失败的初始化必须锁存**，且锁存能被用户开关重置。 | GTA / RTSS 的教训 |
| R4 | **跨线程共享状态只有一个 `FrameLedger`**。渲染线程与提交钩子只通过它交换。 | ticket/fence/cookie 曾散在 3 个文件 |
| R5 | **我们的 GPU 对象只在真实 fence 之后释放**。（优于 OptiScaler 的 park-N 启发式） | 精确退役 |
| R6 | **游戏命令列表状态：要么完整恢复，要么这帧不跑 NR**（绝不猜）。 | bindless 引擎（MHW）的教训 |

附加：**代理 `dxgi.dll` 里不许有任何图形 / UI / 模型逻辑。**

## 分层与唯一接口

| 层 | 内容 | 唯一的公开契约 |
| --- | --- | --- |
| L0 宿主 | `dxgi.dll` 转发系统导出、启动主体 | 无（只有 `Start` / `LiteAttachFactory` 两个私有导出） |
| L1 基础 | 日志、INI 读写与后台写、`GraphicsStatus`、Detours 事务模板、GPU 票据 | `runtime` / `config` / `graphics` / `detour` / `GpuTicket` |
| L2 接入 | 唯一的 Detours 安装点；Factory / Swapchain 纯转发包装 | 谁被钩、怎么钩 |
| L3 观察 | `GetProcAddress` 打桩，截获游戏自己的 NGX SR evaluate | **`FrameInput`（只有它跨层）** |
| L4 管线 | 帧账本（槽池 / 在途 / fence 退役）、命令列表守卫、NR pass、模型运行时 | `Submit(FrameInput) -> PassStatus` |
| L5 表现 | 面板（壳 / UI / GPU）、Resources 预览、view/scale/flip、键鼠 | 只读 `StatusSnapshot`，不碰管线内部 |

## 目录规划（3 个目录，够清楚、churn 小）

```
src/            宿主 + 基础 + 接入
  proxy/（dxgi 代理 proxy.cpp + dxgi.def，替代入口 proxy_forward.cpp + version/winmm.def，
         factory_origin.hpp 供代理与 hooks 共用；跳板与导出名表由 .def 在 configure 时生成）
  bootstrap.*（主体入口，由代理的 Boot 线程加载）
  runtime.* graphics.hpp graphics.cpp（交换链侧：registry + 面板出入口）
  config_writer.*（异步去抖写） config_storage.*（INI 落在哪）
  detour_hook.hpp retry_once.hpp
  hooks.cpp wrappers.hpp
src/nr/         观察 + 管线
  ngx_observer.*（观察游戏自己的 NGX 调用） frame_input.* d3d12_util.*
  capture12.*（帧账本 + 采集，共一份内部状态） command_guard.*
  pass.* model_runtime.* model_settings.* gpu_ticket.hpp
  ngx_*.hpp guide_contract.hpp contracts.hpp
  white_point.* preview.* shaders/（三个 .hlsl，configure 时编成 CSO 内嵌）
src/（表现层，尚未建 panel/ 目录）
  panel.*（壳 + 生命周期 + 输入） panel_impl.hpp（Panel::Impl 状态）
  panel_gpu.cpp（D3D12 渲染） panel_ui.cpp（ImGui 窗口） ui_widgets.*（无状态控件）
```

## 文件映射（现 → 目标）

| 现在 | 目标 | 动作 |
| --- | --- | --- |
| `nr/diagnostics.*` | `nr/capture12.*`（帧账本）+ `nr/status.*`（面板状态） | ✅ 已拆（P1-1，锁序 账本→状态 已固定） |
| `nr/frame.hpp`（杂物袋） | `nr/frame_input.hpp`（`FrameInput` / `Guides` / `Exposure`）+ `nr/d3d12_util.hpp`（格式、转换、`ThrowIfFailed`）+ `nr/preview.hpp` | ✅ 已拆（P1-2） |
| `nr/frame.hpp` 的 `Input11` | — | ✅ 已删（DX11 遗留） |
| `InputOrigin::NativeCapture` / `External` | 只留 `GameDlss` | ✅ 已删（无生产者） |
| `nr/frame.hpp` 的 `Frame12` | `PassFrame` | ✅ 已改名 |
| `nr/capture.cpp` | `nr/ngx_observer.cpp` | ✅ 已改名（同名于其职责：观察游戏自己的 NGX 调用） |
| `nr/capture12.*` | — | ❌ 不做：账本（fence/待提交/拷贝池）与采集共用同一份内部状态，拆开必须把内部状态暴露成头文件，只换来导航收益 |
| `nr/command_state.*` | `nr/command_guard.*` | ✅ 已改名 |
| `nr/pass.*` | 保持 `nr/pass.*`（在 `nr/` 内再加 `nr_` 前缀是冗余）；白点 → `nr/white_point.*`；调试视图 → `nr/preview.*` | 拆（剩 pass.cpp 自身的白点/调试视图部分） |
| `src/panel.cpp` 的 `GraphicsSnapshot` / `GraphicsFailure` / swapchain 绑定 | `src/graphics.cpp` + `src/graphics_registry.hpp` | ✅ 已搬（P1-4，109 行：registry + 5 个入口 + 状态发布） |
| `src/panel.cpp` 的 widget 层（tooltip / 滑条 / 数值弹窗 / Reset） | `src/ui_widgets.{hpp,cpp}`（`lite::panel_ui`） | ✅ 已搬（P1-5，150 行逐行搬移并做了哈希校验） |
| `src/config_writer.*` + `config_storage.*` | — | ❌ 不做：两者是分层（“INI 在哪” vs “何时/多频繁去写”），合并等于让一个文件背两个职责 |
| `src/panel.cpp`（1543） | `panel_impl.hpp` + `panel_ui.cpp` + `panel_gpu.cpp` | ✅ 已拆：`panel.cpp` 壳/生命周期/窗口、`panel_gpu.cpp`（`Wait`/`Drain`/`DestroyRenderer`/`InitRenderer`/`Draw12`）、`panel_ui.cpp`（`BuildResourceUi`/`BuildPipelineStatus`/`BuildUi`）；16 个方法体搬完后逐个 SHA256 比对，0 差异 |
| `LITE_TEST_HOOKS`(34 处) / 截图 / 测试导出 | — | ✅ 已删（宏从未被定义，全是死码） |
| 幂等快照的 `size`/`version`、`ArchitectureStatus` 及其 hook 标志 | — | ✅ 已删（只被测试导出读，写完无人读） |
| `namespace lite` → `litedx12` | 全工程 | ✅ 已改（53 个文件，逐文件回读校验；CMake 目标同为 `litedx12_runtime`/`litedx12_frames`） |
| 三个 compute shader（`lite_color` / `lite_preview` / `lite_meter`） | `src/nr/shaders/*.hlsl` + fxc 预编译成 CSO 内嵌 | ✅ 已改：configure 时 fxc `/T cs_5_0 /E Main /O3` → 字节数组头文件；`D3DCompile` 与 `d3dcompiler` 依赖彻底消失（渲染线程不再解析 shader 源码） |
| 描述符 heap | 48 个轮转（Opti） | ❌ 不做：我们是 **fence 确认完成后**才复用上一个 recording 的 heap，在飞数量就是 heap 数量；换成 48 轮转等于用“猜一个在飞窗口”替换掉一条精确保证 |

## 与 OptiScaler 的关键差异（为什么我们不照抄）

| 机制 | OptiScaler | 我们 | 结论 |
| --- | --- | --- | --- |
| 角色 | NGX provider（自己导出 NGX） | 旁观者（`GetProcAddress` 打桩） | 保持旁观：同等信息，1/10 代码 |
| 命令列表状态 | 停止自己的跟踪 + 可选 `RestoreRoot` | 全量快照 / 恢复 | 保留我们的（R6） |
| 资源退役 | park 32 次 evaluate 后释放 | fence 精确确认 | 保留我们的（R5） |
| 提交跟踪 | NR 路径完全不 hook | hook 队列 Execute + 列表 Reset | 保留（R4/R5 需要） |
| 每帧门控 | 仅"创建帧不评估" | 4 槽上限 + 未完成跳帧 | 保留 |
| 描述符 | 48 个轮转 heap | 复用 retired heap（fence 确认后） | ❌ 保持我们的：精确优于猜测 |
| shader | **预编译 CSO** | ✅ 已照改：fxc configure 时编译，字节码内嵌（三个 shader） | 已达成 |

## 构建 / 调试

```powershell
./Build.ps1                     # Release -> dist/Release/
./Build.ps1 -Configuration Debug
```

部署三件套：`dxgi.dll`、`nvngx.dll_litedx12.dll`、`DLSSNR_LiteDX12.ini`。
日志 `DLSSNR_LiteDX12.log`（同目录）。面板 Insert；NR 默认关（`[NeuralRendering] Enabled`）。

## 当前状态与未完成

- GTA V Enhanced 已验证可进入并正常运行（关键：`SkipSmallSwapchains=1`，辅助小链不包装）。
- NR 本身尚未在游戏里开过（`Enabled=0`）。
- 重构进行中：见"文件映射"表的动作列。**每步只做纯搬移 / 改名，完成后必须编译通过**；游戏不跑。
- 进度（全部完成，每步都编译通过、`/W4` 零警告）：P1-1 `diagnostics.*` → `capture12.*` + `status.*`；P1-2 `frame.hpp` → `frame_input.hpp` + `d3d12_util.*` + `preview.hpp`；P1-3 改名 `ngx_observer.cpp`/`command_guard.*`；死码清扫（`LITE_TEST_HOOKS` 34 处、测试导出、`WriteBmp`/截图、`ArchitectureStatus`、快照 `size`/`version`、`DebugOutput()`、空 `idle`、重复的 `retainedBuffers`、空壳 `TabFlags`，共 −308 行）；P1-4 `graphics.cpp` + `graphics_registry.hpp`；P1-5 `ui_widgets.*`；P1-6 面板三拆（`panel_impl.hpp` + `panel_ui.cpp` + `panel_gpu.cpp`）；P1-7 `namespace litedx12`；P2-1 fxc 预编译 CSO。
- 搬移的做法（请照做）：**定位行范围 → 逐行原样搬 → 对搬走的块 SHA256 前后比对 → 再编译**。面板拆分后 16 个方法体逐个比对，0 差异；`ui_widgets.cpp` 的 150 行也做过同样校验。不要手抄搬移代码，也不要用脚本解析 `Panel::Impl` 这种方法体/成员交错的块——PowerShell 大小写不敏感的变量名（`$m` 碰掉 `$M`、`$l` 碰掉 `$L`）已经坑过一次。
- 不做（有理由，写在映射表里）：拆 `capture12.*`、合并 `config_*`、48 轮转描述符 heap。
- 下一步候选（需你点头）：`preview.cpp` 的 `Preview12` 每次开面板会建 2 个描述符 + 一条 PSO（只在开启时一次）；`input.cpp` 是最后一处手写的 `DetourTransactionBegin` 链；`pass.cpp` 914 行仍可再分（白点交互、计时读回）。
- 每次动完都要复核 `dist/Release` 的三个文件哈希；**部署要等你发话**（GTA 里现在是 `A46E176B9119` / `903A9499BD51` / `AC87313E79E0`）。
- P2 待定（需用户确认）：shader 预编译、轮转 heap 描述符、`panel` 深度拆分。

最后更新：2026-10-11
