# 2026-11-05 新增 Verbose 诊断（严格只在 `Level=2` 生效）

新增 6 类日志，**全部**是 `Detail::Verbose`，且在会做实际工作的地方（取 QPC、构造 `std::string`、加锁）前面另有一道 `VerboseLogging()` / `VerboseEnabled()` 提前返回。`Level=0/1` 时这些代码连函数体都不进，开销是一次 relaxed 原子读（仅在 layer 创建、context 重建、历史失效这些低频点）。

| 日志 | 位置 | 回答什么问题 |
|---|---|---|
| `NR skipped N frames: <reason>` | `model_settings.cpp` `NoteSkip`/`NoteCapture` | **这一局丢了多少帧 NR、为什么**。按"一次连续跳过"为单位精确计数；原因为 `NR capture slots busy`、绑定被拒、`refuse()` 的各类拒绝、捕获被拒 |
| `NR skipped M frames so far; now: <reason>` | 同上 | 原因逐帧交替时的兜底总量，每 600 帧最多一行，不会刷屏 |
| `NR layer created for WxH: <why>, create X.XXX ms, NGX=0x…` | `pass.cpp` | **建 feature 的耗时和原因**（`input resolution/format changed` / `model tuning committed` / `first input for this stream`）——这是最可能造成顿挫的一步 |
| `NR context reset to WxH fmt=F` | `pass.cpp` | 尺寸变化导致 165 MB 纹理组重建 |
| `NR context textures allocated WxH in X.XXX ms` | `pass.cpp` | 那 165 MB 实际花了多久 |
| `NR history invalidated` / `NR history reset requested by the caller` | `pass.cpp` | 时间累积被丢弃的频率 |
| `NR GPU avg (n=120): total X model Y ms` | `pass.cpp` `retire` | 不开面板也能拿 GPU 耗时（需 `ForceTiming=1` 或面板可见才会走计时路径） |

实现要点：

- 计数在 `PublishPass` / `ReportModelWait` 两个既有漏斗里挂钩，**不碰捕获路径**。这两个点是 NR 状态的唯一权威来源：`ready && actualPasses` 才算一次成功，其余都计入跳过；
- 一次跳过运行结束时才输出，长度 ≥2 才报，避免逐帧刷屏；逐帧交替由 600 帧总量兜底；
- 所有 reason 字符串都是静态字面量，`skipTally` 里存的是 `std::string` 副本，没有悬垂指针；
- `layer` 只在一处被 `reset()`（tuning），加上 Context 重建和新建 executor 三种情况，`layerReason` 全覆盖。

**一处自查发现的缺陷（已修）**：最初把计数挂在 `capture12` 的 `Report()` 上，但 `Capture12` 在 `pass->Record()` 之后**无条件**调 `Report(S_OK)`（忽略 Record 的 HRESULT），会导致每帧都记一次"成功"，连续拒绝时 run 永远到不了 2，只能在 600 帧兜底里看到。已把计数完全收回 `PublishPass` / `ReportModelWait`。

**注意**：`ModelRevision` 变化（切 NR 开关）或 stream 切换会重新构造 `PassExecutor12`，日志里报的是 `first input for this stream`，不是"NR 被重新启用"——那是唯一一处原因文字不够精确的地方。

**21 个目标保持 `Level=0`**，行为与上一版完全一致（`45120DB7CD11` / HDR `5B2B71A81B86`）。

# 2026-11-04 预览重构：预览不再拖累 NR（DX12），两个工程的视图状态机清理

## 修掉的真实缺陷：开了资源预览，NR 整帧被跳过

影之刃零（PBZ）实测暴露：一进 Resources 页画面就闪，日志里**每帧**都是

```
NR waiting: NR capture slots busy; original game frame preserved
```

因果链：

1. `capture12.cpp` 的 `direct[n] = ... && !preview` —— 只要预览开着，模型三路输入从"原地直读"全切成"私有副本"，每个捕获槽多占 ~130 MB；
2. `l.latest` 在预览期间钉住一个槽，更关键的是**面板在 `frame.captured` 里按 swapchain 缓冲数（3）各持一个快照**，只在环形槽下次复用时才释放；
3. 捕获槽池上限 8 → 面板 3 + latest 1 + 在飞 4 = 8，`AcquireCopy` 返回空 → `PublishCaptureBusy()` → **永久性**跳帧（不是抖几帧）。

日志铁证：`NR copy resource rebuilt` 正好 24 条 = 8 槽 × 3 资源。

## 改法：预览移到 NR 自己的录制 list 上（照 DX11 的做法）

DX11 一直是这么做的（`capture11.cpp:233`），所以它从来没有这个问题：预览图在当帧、同一张 list 里算完，输出到面板自己的 image，再交给 bridge 拷进共享纹理。不需要副本、不需要把槽留给面板。

DX12 现在一致了（`capture12.cpp`，`reads.Begin()` 之后、模型录制之前）：

```cpp
if (preview) { ... l.previews[device]->Record(commands, previewFrame); ... }
if (model) { ... }
```

于是顺手删掉：

| 删除 | 原因 |
|---|---|
| `direct[n]` / `directExposure` 里的 `!preview` | 预览和模型读同一份资源、同一张 list |
| `FrameLedger::latest`、`Captured12::previewGeneration` | 不再需要把捕获快照留给面板 |
| `Latest12()`、`ResetPreviewHistory()`（含 status.cpp 三处调用） | 同上 |
| `Frame::captured`、`Frame::preview` | 面板改用 `PreviewImage12()` 取账本里的图 |
| `Panel::Impl::previewError`、`previewDisplayed` | 失败结果改由捕获侧 `PreviewResult()` 发布；后者只写不读 |

验证（PBZ，`Level=2`）：`copy rebuilt: 0`、`slots busy: 0`、`preview failed: 0`，开预览时路由仍是 `color=direct depth=direct mv=direct`。

顺带删掉成为死引用的 `panel.cpp` / `panel_gpu.cpp` / `panel_impl.hpp` 里的 `#include "nr/preview.hpp"`（面板不再自己录预览，只用 `PreviewImage12()` 取图）。

## 顺带清掉的死状态机（两个工程）

面板 Resources 页无条件 `SetView(All)`、其它页 `SetView(Off)`，所以 `View::Color/Depth/Motion` 永远选不到：

- **DX12**：删 `enum class View`；`Preview12::Record(list, frame)` 只保留"三张并排"，单张改为私有 `RecordTile(..., unsigned input, ...)`；`Selected()/SetView()` → `PreviewSelected()/SetPreviewSelected(bool)`。
- **DX11**：同样删枚举；额外删掉 `Configure(View)` 与 `[Diagnostics] ResourceView` 键（它读的初始视图在任何时刻都会被面板覆盖，永不生效），以及只为"深度视图尺寸"服务的 `depthDesc` 和 `NativeCaptureState::frameView`（记录缓存图属于哪个视图，只有一种视图后恒真）。

## 已知未修：MV 预览间歇性左右摆动

现象：只在 MV 那一格预览、间歇出现，像两张画面在交替；**游戏画面与 NR 输出正常**。

已排除：一次性诊断打印了 573 次录制的全部常量（`src 2562x1441 origin 0,0 -> out 3840x2160 scale 1,1 gain 1`）**完全稳定**；`preview=1` 确实出现过；录制频率 ≈ 每帧一次，不是多 stream 交替。

剩余判断：面板在自己的 list 上读这张共享预览图，而它在 NR 的 list 上被写；同队列时有天然顺序，跨队列时没有 → 偶发读到正在被改写的那张。修法（面板读前插 fence 等待 / A/B 双缓冲）**用户决定不做**，因为只影响诊断预览。诊断代码已删除。

后续两次复测都正常（其中一次 `preview=1` 确实打开过），**判定为噪声/未复现**，不再处理。

## 本次部署

| 变体 | 尺寸 | MD5 | 目标 |
|---|---|---|---|
| DX12 标准 | 1122304 | `BF1C7853085F` | 20 个（含 PBZ） |
| DX12 HDR | 1142272 | `5146C3EF21E9` | manorlords |
| DX11 | 1228288 | `747531263830` | bg3 / roe / wildlands / bannerlord |

21 个 DX12 目标的 INI 全部 `Level=0`；DX11 四个目标同样 `Level=0`，`[Diagnostics] ResourceView` 键已从模板和四份 INI 删除。

# 2026-11-04 新增影之刃零（PBZ）

| 项 | 值 |
|---|---|
| 目标 | `pbz` → `F:\SteamLibrary\steamapps\common\PBZ\platform\steam` |
| 引擎 | UE（`Engine/`、`ProjectPBZ/` 目录布局），exe 直接放在 `platform\steam` |
| API | 导入 `d3d12.dll` + `dxgi.dll`（`D3D12CreateDevice` 存在），可部署 DX12 工程 |
| 部署 | `dxgi.dll` `814240A7DBDE`、`nvngx.dll_litedx12.dll` `55F0B748A47E`、`nvngx_dlssnr.dll` `894CE9718AD9`（310.9.16.0） |
| INI | 新写一份，`Level=0`、`[NeuralRendering] Enabled=1`、`RequireComputeSignature=0`，其余取模板默认 |

已加入 `tools/deploy.ps1` 的 `$games`，现在列表是 **21 个目标**（20 标准 + 1 HDR）。

部署时改的两处模板默认值，和新游戏保持一致用：

- 模板是 `[NeuralRendering] Enabled=0`（交给用户主动开启），现网所有目标都是 `1`；新目标按现网写成 `1`，否则 NR 不工作。
- 模板是 `Level=1`，现网所有目标都是 `0`；新目标按现网写成 `0`，要查问题时临时改 `2`。

另外确认过：这个目录原先没有 `dxgi.dll`，不存在顶掉别的代理（ReShade / OptiScaler）的风险。

# 2026-11-04 深度尺寸实验（已回滚）

问题：星战深度 2564×1444 小于 NR 的 3840×2160，`model` 比博德之门多 0.275 ms。假设是 NGX 在模型内部升采样深度。

做法：在 `pass.cpp` 的 guide 转换 dispatch 里顺带把深度双线性放大到 NR 尺寸（HLSL mode 2 加一条 4 点手动插值分支），并把 `DLSSNR.DepthSubrect*` 报成全分辨率，让 NGX 直接吃到 4K 深度。

实测（星战，同口径）：放大路径确认生效（当时临时加的 `NR depth guide` 日志显示 `2564x1444 -> 3840x2160`），`total 7.3 / model 7.3`，与基线 `total 7.66 / model 7.44`（15 帧均值）在噪声内，**没有收益**，且双线性放大深度有边缘光晕风险 → 代码全部回滚，临时日志也已删除。

结论：`model` 的时间不是深度升采样，是模型本身。NGX 内部那一步本来就不贵。

## 同一个 dispatch 换成 CopyResource 也不行

想法：参考实现用 `CopyResource` 把 typeless 深度拷成同尺寸的 `R32_FLOAT_X8X24_TYPELESS` 再交给 NGX，省掉着色器。实测算带宽不成立：

| 做法 | 读 | 写 | 合计 | 实测/推算 |
|---|---|---|---|---|
| dispatch → `R32_FLOAT` | 29.6 MB（8 B/px SRV） | 14.8 MB（4 B/px） | 44.4 MB | 0.047 ms ≈ 945 GB/s，已在带宽上限 |
| CopyResource → `R32_FLOAT_X8X24_TYPELESS` | 29.6 MB | 29.6 MB | 59.2 MB | 推算 0.063 ms，**慢 0.016** |

而且交给 NGX 的深度会变成 8 B/px，模型内部读深度也要翻倍。已回滚。

## NGX 的深度分辨率是怎么知道的

`DLSSNR.DepthSubrectBaseX/Y` + `DLSSNR.DepthSubrectWidth/Height`（`pass.cpp` 的 `Region()` 写的就是这四个键，参考实现 `dlssnr_forwarder.cpp:699-702` 用同一组）。它是"绑定资源内部的一个矩形"语义：

- NGX 由 `DepthSubrectWidth/Height` 与 `DLSSNR.Width/Height` 的差异得知需要升采样，**并自己完成**；
- 不能声明超过资源本身的尺寸（会让它越界读），所以"只改声明不改纹理"这条路不存在。

另外注意：`NR route:` 日志和面板的三个资源预览报的都是**游戏源纹理**（星战深度永远是 2564×1444），不是交给模型的那张。


# 2026-11-04 实测归因：DX12 不比 DX11 慢，是输入数据更难

同口径实测（两边都是 3840×2160 输出、同一个 NR 模型 310.9.16.0、面板关闭、`Level=2` 临时打开）：

| 阶段 | 博德之门 (DX11) | 星战绝地 (DX12) | 差 |
|---|---|---|---|
| `total` | 7.370 ms | 7.698 ms | +0.328 |
| `encode`（颜色→proxy + guide 转换） | 0.070 | 0.117 | +0.047 |
| `model`（NGX evaluate） | 7.219 | 7.494 | +0.275 |
| `resolve` | 0.082 | 0.087 | +0.005 |

样本：博德 31 条、星战 43 条。

## 0.33 ms 的来源

**1. `model` +0.275 ms —— 深度分辨率不匹配**

```
博德之门：颜色 3840x2160，深度 "direct SRV (no staging copy)"   ← 深度与输出同分辨率
星战：    color=3840x2160  depth=2564x1444                      ← 深度只有渲染分辨率
```

NGX 必须在模型内部把 2564×1444 的深度升采样到 3840×2160 才能做时间累积，这段计算在 `model` 区间内，插件无法干预。

**2. `encode` +0.047 ms —— 深度格式转换**

星战深度是 `R32G8X24_TYPELESS`（fmt=19），期望 `R32_FLOAT`，我们跑了一次 2564×1444 转换 dispatch；博德的深度可直接读，零转换。这项只有 0.6%，且改掉它要赌 NGX 接受 typeless 资源描述，风险不成比例。

**3. `resolve` +0.005 ms —— 噪声**

## 结论

DX12 侧没有结构性 GPU 开销。两个工程的 `Completed NR GPU` 测量窗口完全一致（都是 pass 级 t0→t1），DX11 面板另有一行 `Bridge chain` 才包含输入/结果拷贝，容易看混。

博德的路径已是最优：`bridge: result resolved in place`（原地解析，零回写）+ `depth bridge: direct SRV`（深度零拷贝）。

## 本轮同时修的两处

- `nr/capture12_input.hpp` `InputReadStates12`：输入状态切换从整资源屏障改为只动 subresource 0，与同文件另外两个函数一致（游戏表面带 mip 链时不再刷新整个表面）
- `nr/panel_ui.cpp`（DX11）：MV 估计器的参数组（FOV / far plane / depth 乘数 / depth XY 映射 / 对数深度）先前无条件显示，现在只在"确实使用估计器"时显示（`generatedMotion || inputOrigin == NativeCapture`）；游戏提供原生 MV 时显示一行说明

## 临时诊断代码已全部移除

`NR GPU breakdown`、`NR history reset`、`NR model reset` 三条日志与 `resetCause` 追踪已删除，全仓零残留。产物：标准 `1122816 B`、HDR `1142272 B`、DX11 `1228800 B`，`/W4` 零警告，20 + 4 个目标逐一核对一致。

> 注意：每次重新构建后 MD5 都会变（MSVC 默认把链接时间写进 PE 头），所以跨构建比对只能用「尺寸 + 源文件时间戳」，不能用 MD5。同一份 `build\` 产物部署到多个目标时 MD5 才稳定。

## 深度分辨率的实测差异（2026-11-04 补充）

临时给 DX11 的 `depth bridge` 日志加上尺寸后实测：

```
博德之门：depth bridge: 3840x2160 -> 3840x2160 R32_FLOAT, direct SRV (no staging copy)
星战绝地：color=3840x2160  depth=2564x1444
```

博德给的是**与输出同分辨率的深度**（3840×2160），NGX 无需升采样；星战给的是**渲染分辨率深度**（2564×1444），NGX 在 `model` 阶段内部升采样到 4K。这就是那 0.275 ms 差值的来源，属游戏自身数据差异。

同时修正之前的错误说法：DX11 的 twin **只缩不放**（`if(frameWidth < shared.Width)`），源比帧小时原样保留，所以它并不是"总是给 NGX 帧尺寸深度"。

该临时日志已删除，`depth bridge` 恢复为原来的单行形式。

# 2026-11-03 全量开销核算（对照 10-28 基线）

`/W4` 零错误零警告。标准 `1122304 B @ 22:52:13`，HDR `1141760 B @ 22:52:16`。

**方法说明**：基线源码在 `H:\AgentWorkPlace\DLSS5\_backup\20261029-124803-before-aw2-fix\`（`DLSSNR_LiteDX11` + `DLSSNR_LiteDX12` 全量源码，`bin\` 里有对应产物：`LiteDX12-Release-nvngx.dll_litedx12.dll` = 1130496 B，正是 10-28 那版）。以下是对该源码树做逐函数 diff 的结果。

## 热路径逐函数对照（基线 → 现在）

| 函数 | 变化 | 判定 |
|---|---|---|
| `Capture12` | 342 → 344 行：删掉 3 处 `InvalidateHistory` 循环；`direct[n]` 条件与基线**完全一致**；新增 `spare12` 查找（仅缺 key 时） | 省 |
| `pass.cpp Record` | 661 → 626 行：每帧少一次 `CheckFeatureSupport`、少一次 `white.Resolve`、删掉 `sampledPre` / `reverseMeasurement` / `partialColorOutput` 计算与 `applyModel` 分支 | 省 |
| `command_guard Track` | 15 → 24 行：新增无锁快路径（命中直接返回，不取锁） | 省 |
| `gpu_ticket PrepareSubmission` | 删掉 `waited` / `waitedFence` 记账 | 省 |
| `GetTimestampFrequency` | 每次提交都查 → 仅未知时查 | 省 |
| `DrawSwapchain` | 少一次 `Snapshot()`（互斥 + 结构体拷贝） | 省 |
| `Panel::Draw` | 完全相同 | 0 |
| `Panel::Impl::Draw12` | 每帧 `QueryInterface(swap3)` + 2×`GetDescriptorHandleIncrementSize` + 每帧重写 SRV → 全部缓存 | 省 |
| `Present` | 完全相同 | 0 |
| `Present1` | 基线：`*p` 拷贝 + 面板可见才清空 + 每帧 `GraphicsSnapshot()`；现在：两次原子读，未编辑时**原样转发** | 省 |
| `LogFormat` / `Submitted` / `Discard` / `Collect` | 完全相同（`Collect` 仅 `debug`→`preview` 改名） | 0 |
| 等待原语统计 | `->Wait` 2/2、`SetEventOnCompletion` 2/2、`WaitForSingleObject` 3/3、`Flush()` 3/3、`Signal` 3/3 | 无新增阻塞 |

## 唯一比基线多做的三处

1. **NR 编辑过画面后的整屏 present**。基线从不整屏（所以会把 NR 编辑留在上一帧，是 bug）。4K 下约 33 MB/帧的额外 DWM 合成，正确性成本。
2. **`dsdc` 的 `ForceTiming`**：4×`EndQuery` + 1×`ResolveQueryData`。
3. **`GetModelSettings` 多一次原子读**（`ForceTiming()`）。

## 今天各项改动的独立核算

| 改动 | 每帧成本变化 | 判定 |
|---|---|---|
| `command_guard` 线程本地缓存 + generation | `Track()` 80–110 ns × 3–8 次 → 10–25 ns | 改善 |
| 面板 `swap3` / 预览描述符 / RTV 缓存 | 见上表 | 改善 |
| 执行器停车复用 | 事件级少一次 `CreateFeature` + 165 MB | 改善 |
| `refuse()` / 跳帧不再清零历史 | 事件级少 NGX `Reset` 尖峰 | 改善 |
| `Region()` 改用栈上键名 | 每帧少 ~20 次 `std::string` 堆分配 | 改善 |
| `Tuning()` / `ControlMask` 不再每帧重复 | 每帧少 9 次驱动参数写 | 改善 |
| `SurfaceNeedsFullPresent()` / `NoteEditedFrame()` | 2 次原子读 + 1 次原子写（基线在此处是互斥 + 结构体拷贝） | 可忽略 |

## 全树种「成本模式」计数（DX12，源码级）

| 模式 | 基线 | 现在 | 差 |
|---|---|---|---|
| `lock_guard` | 108 | 105 | −3 |
| `QueryInterface` | 31 | 19 | −12 |
| `CheckFeatureSupport` | 7 | 4 | −3 |
| `LogFormat(` | 20 | 19 | −1 |
| `GetPrivateProfile*` | 30 | 29 | −1 |
| `Log(` | 35 | 36 | **+1** |
| `ResourceBarrier` / `Transition(` / `CopyResource` / `CopyTextureRegion` / `Dispatch(` / `EndQuery` / `ResolveQueryData` / `Signal(` / `ExecuteCommandLists` / `GetCompletedValue` / `make_shared` / `make_unique` / `GetDesc()` / `CreateShaderResourceView` / `CreateUnorderedAccessView` / `CreateDescriptorHeap` / `GetDescriptorHandleIncrementSize` / `GetTickCount64` / `GetAsyncKeyState` / `GetForegroundWindow` | — | — | **全部相同** |

唯一的 `+1` 是启动时那条 `ForceTiming=1` 日志，不在任何每帧路径上。

## 每个变更文件的归类（DX12）

| 类别 | 文件 |
|---|---|
| 纯重组/改名，内容不变 | `nr/d3d12_util.*` → `nr/gpu_util.*`（diff 只有 include 行）；`proxy.cpp` 拆出 `proxy/proxy_common.hpp` + `proxy/proxy_forward.cpp`；`retry_once.hpp` 删除 |
| 删除死重 | `wrappers.hpp` −7425 B（移除 `class Factory`，含 12 处 `QueryInterface`）；`graphics.cpp` −670 B（移除每 Present 的 `GraphicsSnapshot()`）；`pass.hpp` −203 B（`applyModel` 等） |
| 热路径优化 | `pass.cpp` −1686 B；`command_guard.cpp` +1503 B 但新增内容全是缓存；`capture12.cpp` +1112 B 但新增内容全是 `spare12` 与 preview 门控；`gpu_ticket.hpp` −200 B；`panel_gpu.cpp` / `panel_impl.hpp`（`swap3`、预览描述符、`srvStride` 缓存）；`nr/status.cpp`（两个原子） |
| 新功能（非每帧） | `ForceTiming`（`runtime.*`、`model_settings.*`、`panel_ui.cpp`）；`Present1` 条件整屏（`wrappers.hpp`、`graphics.*`、`status.*`） |
| 仅面板打开时 | `panel_ui.cpp` +664 B |
| 仅启动 | `runtime.cpp` 的 `ForceTiming=1` 日志 |

## 同法对照 DX11

| 模式 | 基线 | 现在 | 差 |
|---|---|---|---|
| `QueryInterface` | 33 | 21 | −12 |
| `CheckFeatureSupport` | 6 | 3 | −3 |
| `Flush()` | 8 | 7 | −1 |
| `lock_guard` | 116 | 117 | +1（`depth11.cpp` 新增的 `MarkWrite`/`ContextLocked` 各一处，都在既有加锁函数内部，未增加加锁次数） |
| `Log(` | 33 | 34 | +1（`bridge:` 那条一次性日志） |
| 其它 GPU/COM/分配模式 | — | — | 全部相同 |

DX11 深度热路径逐函数对照：

| 函数 | 基线 | 现在 |
|---|---|---|
| `Binding` | 每次绑定都 `view->GetResource()` + map `Find()` | `BindView()` 先用 DSV 指针做身份比较，相同则直接返回 |
| `State` | 每次状态变化都 `state->GetDesc()` | 按 `lastState` 缓存，只在换状态时解析 |
| `MarkWrite` | 无此短路（基线在每次绑定时做 `Shape()`） | `lastWrite==current && lastWriteFrame==frame` 直接返回 |
| `ContextLocked` / `TrackingEnabled` | 每次 `RegisterLocked`（map 查找） | thread_local + `policyEpoch` 失效 |
| `depth_hooks.cpp` | 6 组 draw 钩子（每 draw ~60 ns） | −1808 B，draw 钩子全删 |

DX11 唯一「每帧比基线多」的地方：`Present` 从 `real->Present(sync,flags)` 直接转发改成走 `ForwardPresent` → `WithParentBypass`（`thread_local unsigned` 的 `++`/`--` 加一个 `__try/__finally` 帧），为的是让 RTSS 从 Present 内部加载 d3d11 时不触发工厂捕获。量级几十到几百纳秒。

## 仍存在但都在噪声级的浪费（未改）

- 每帧 33 次模型参数写，其中 `DLSSNR.Enabled/Width/Height/CreationNodeMask/VisibilityNodeMask` 5 项只在建层时被读
- `ListIdentity()` 每帧 4 次 `QueryInterface`（Capture/PrepareSubmission/Submitted/Discard）
- `commands->GetDevice` + 最多 3 次 `resource->GetDevice` 每帧
- 每帧 1×`make_shared<Recording>`、1×`make_shared<GpuTicket>`、1×`new RecordingCookie`
- 面板隐藏时 `Panel::Draw` 仍每 Present 做 `ForegroundMatches`（~3 次系统调用）与 `RetireCompleted`（ledger 锁）

合计约 10–30 µs/帧，相对 15.8 ms 的帧时间不足 0.2%。

## 结论

DX12 侧没有任何会明显增加开销或延迟的负优化。**DX11 侧有一处**：基线 `Present1` 是 `real->Present1(sync,flags,p)` 纯转发，11-03 被改成无条件整屏 —— 那才是真正的负优化。已按 DX12 相同方式条件化（`lite::SurfaceNeedsFullPresent()` = `nr::PanelVisible() || nr::EditedAnyFrame()`），并部署到 4 个 DX11 目标。

# 2026-11-03 NR 重建审计：去掉一次完全不需要的 `CreateFeature`

`/W4` 零错误零警告。标准 `1123840 B @ 22:48:41`，HDR `1143296 B @ 22:48:44`。已装到 MH Wilds，其余目标尚未刷新。

## 代价基准

一次 NR 重建 = **NGX `CreateFeature`** + `Context` 三张全帧纹理（4K 下 `R16G16B16A16_FLOAT` 66 MB ×2 + 色彩 33 MB ≈ **165 MB** 分配）。NGX 侧是几十到上百毫秒量级的同步工作，必然是肉眼可见的卡顿。

## 全部重建触发点

| 触发 | 位置 | 代价 | 频率 | 必要性 |
|---|---|---|---|---|
| 游戏释放自己的 NGX SR feature → 我们 erase 执行器 | `capture12.cpp` `ReleaseModelStream` | CreateFeature + 165 MB | 每次改 DLSS 档位 / 读盘 / 部分菜单（MH Wilds 实测每轮 2 次） | **不必要** → 已修 |
| 我们的录制被丢弃（list 被 Reset 但没 Execute） | `pass.cpp:409-421` | CreateFeature + 165 MB | 取决于引擎 | 有争议（见下） |
| 调参变化（面板滑条松手） | `pass.cpp:552-561` | CreateFeature（纹理保留） | 每次松手一次 | 基本必要 |
| 尺寸/格式变化 | `pass.cpp:534-551` | 165 MB | 换分辨率/档位 | 必要 |
| NR 开关切换（`ModelRevision`） | `capture12.cpp:587-591` | CreateFeature + 165 MB | 用户操作 | 必要 |

## 已修：执行器改为复用

游戏释放它自己的 SR feature 时，我们的 NR feature 与它**没有生命周期关系** —— 原实现却把整个 `PassExecutor12`（含 feature、`Context`、三张纹理）一起销毁，下一帧从零重建。

```cpp
// capture12.cpp ReleaseModelStream
if (l.spare12.size() < 2) {
    it->second->InvalidateHistory();          // 只丢时间历史
    l.spare12.push_back(std::move(it->second));
}
```

`Capture12` 里缺 key 时优先从 `spare12` 取，取出后同样 `InvalidateHistory()`；`spare12` 在 NR 关闭、`ModelRevision` 变化、设备退休三处一并清空，不会跨设备留存 GPU 对象。上限 2 个，最坏多占 2 套纹理。

复用的执行器内部仍会自己处理尺寸/格式变化（`Record` 里重建 `Context` 与 feature），所以换分辨率不会拿旧尺寸的 feature 去跑。

## 未改，需要决定的三处

1. **录制被丢弃 → `context.reset()`**（`pass.cpp:405-417`）。丢弃的列表里那些 `Transition` 从未执行，真实状态与我们的假设可能不一致，所以原实现整个换掉是"安全但昂贵"。更省的做法：保留 `Context`，下一帧用 `COMMON` 作为中立状态重新落一遍三张纹理的状态，代价只是一组 barrier。**本次未改**：MH Wilds 两次运行日志里一次都没出现 `NR: NR recording discarded`，无法确认收益；而要让它安全，要么依赖 `COMMON` 作为 `StateBefore`（驱动容忍但 debug layer 会警告），要么把状态跟踪改成"只在真正提交时推进"的重写，两者都不该盲改。丢弃事件本身已经会写日志，将来哪个游戏出现这行再单独处理。
2. ~~**`refuse()` 里置 `layer->first = true`**~~ → **已改**。被拒的帧根本没用过 layer，历史完好，`refuse` 现在完全不碰历史（`pass.cpp:287-297`）。
3. ~~**捕获槽忙 / 绑定缺失 / 无空闲槽 → `InvalidateHistory()`**~~ → **已改**。三处调用删除（`capture12.cpp:327/365/381` 附近），只保留 `PublishCaptureBusy()` 的状态发布。跳过一帧不再把 NR 的时间累积清零 —— 高负载下的持续抖动/拖影应当消失。

## 已知代价（#3 的取舍）

跳过帧后模型会带着"上次处理帧"的历史继续跑，色/深历史比当前帧旧几帧。正常情况下运动矢量是当前帧的，NR 能自我修正；只有跳帧极频繁时才可能出现短暂重影，此时宁可重影也不要每帧清零（清零会导致永远累积不起来 + 每次 Reset 都有一次尖峰）。

# 2026-11-03 `Present1` 回到基线纯转发（最省）

`/W4` 零错误零警告。**最终产物**：标准 `1122304 B @ 23:28:44`（SHA256 `4120A36B…`），HDR `1141760 B @ 23:28:47`（`FE1D1F8F…`），18 个标准 + 2 个 HDR 目标全部刷新。

## 最终行为

```cpp
const DXGI_PRESENT_PARAMETERS* forwarded=p;            // 游戏参数原样转发
#if defined(LITEDX12_HDR)
if(hdrOutput)forwarded=p?&full:nullptr;                // HDR 输出重写了整帧
#endif
```

- 面板可见时**不再**接管整屏（基线的这条也已去掉）
- `SurfaceNeedsFullPresent()` / `nr::NoteEditedFrame()` / `nr::EditedAnyFrame()` 全部删除，零残留
- `ForwardPresent` 里那次每帧 `GetModuleHandleW(L"RTSSHooks64.dll")`（loader 锁）改成**只探测一次**并缓存结果，稳态只剩一次 relaxed 原子读 + 分支

比 10-28 基线更省的地方：不再拷贝 `DXGI_PRESENT_PARAMETERS` 结构体、不再每帧查 RTSS 模块。

**已知取舍**：NR 整帧重写，某游戏若按部分脏矩形提交，脏矩形之外的 NR 改动会停在上一帧；面板同理。基线一直如此。

## 中间过程（供追溯）

中间产物 `1123328 B @ 22:20:38`（条件整屏）、`1122304 B @ 23:21:48`（面板可见才整屏）。

## 中间过程（供追溯）

`/W4` 零错误零警告。中间产物 `1123328 B @ 22:20:38`，HDR `1142784 B @ 22:20:42`。

## 起因

MH Wilds 在 10-28 基线上 64 fps，在最新版上 63 fps。两次运行的日志**结构完全相同**（同一路由、同样三次 `NR layer created`、无失败行），所以差别不在捕获/NR 路径，只可能在呈现层。

两版唯一的呈现层差异：`Present1` 曾无条件把游戏的 `DXGI_PRESENT_PARAMETERS` 换成空结构，等于每帧丢掉

- `pDirtyRects` → DWM 每帧合成整张 3840×2160（33 MB），而不是只合成变化区
- `pScrollRect` / `pScrollOffset` → 驱动/系统的整页滚动优化失效

4K 下约 2 GB/s 的额外合成带宽，掉 1 帧（1.6%）对得上。

## 修改

只在**我们自己确实改过 backbuffer** 时接管整屏：

```cpp
bool wholeSurface = litedx12::SurfaceNeedsFullPresent();   // 面板可见 || NR 编辑过任意一帧
#if defined(LITEDX12_HDR)
if (hdrOutput) wholeSurface = true;                        // HDR 输出重写了整帧
#endif
const DXGI_PRESENT_PARAMETERS* forwarded = p ? (wholeSurface ? &full : p) : nullptr;
```

- `nr::EditedAnyFrame()`：**粘性**标志，`PublishPass` 收到 `edited=true` 时置位（`nr/status.cpp`）。粘性避免了"这帧编辑了、下帧没编辑"导致的部分呈现错乱。
- `nr::PanelVisible()`：面板可见时必然重画整屏。
- HDR 构建里 `hdrOutput` 存在就一定整屏。

## 影响面

- NR 正常工作的游戏（`dsdc` 及其余目标）：`edited` 第一帧就置位 → 行为与之前完全一致，永远整屏
- NR 没有生效的游戏（如当前 MH Wilds）：回到基线的"转发游戏原参数"，代价消失

## 部署

标准版已铺到 MH Wilds（`F:\SteamLibrary\steamapps\common\MonsterHunterWilds`，`1123328 B @ 22:20:38`）。其余 18 个标准目标 + 2 个 HDR 目标**尚未刷新**，等 MH Wilds 实测确认后再铺。


`/W4` 零错误零警告。标准版 `1122816 B @ 20:21:52`，HDR 版 `1141760 B`（待重建）。

## 真正的原因

`DirectResolve=1` 时我们把 NR 结果以 **UAV 写进游戏自己的颜色资源**。这条 UAV 写在游戏后续帧内读取之前**没有保证完成**，于是消费方读到的是半新半旧的帧 —— 表现为顶部一条新内容、其余是旧内容，且边界逐帧乱跳。

## 唯一有效的修复手段：`EndQuery(TIMESTAMP)`

三道对照实测（同一份 INI，只切一个键）：

| 手段 | 效果 |
|---|---|
| `UAV` barrier（`D3D12_RESOURCE_BARRIER_TYPE_UAV`） | ❌ 现象不变 |
| 状态转换 `Transition(...)`（资源级 barrier） | ❌ 现象不变 |
| 末尾一次 `EndQuery(D3D12_QUERY_TYPE_TIMESTAMP)` | ✅ 整屏正常 |

原因：时间戳查询必须**精确反映执行点**，驱动为了得到该值会在插入点做一次完整管线排空（含我们前面的 UAV 写）。UAV barrier 只处理 UAV↔UAV 排序，状态转换只处理资源级同步，两者都不产生排空。

**这个结果同时纠正了上一节「消费方不在同队列下游」的推断** —— 排空能修好，说明消费方确实在同队列下游，只是缺一次排空让写入落地。

## 实现

新增 `[Diagnostics] ForceTiming`（默认 `0`）：面板关闭时也走 GPU 计时路径（4 个时间戳 + `ResolveQueryData` + 退役时 `ReadTiming`），也就是"面板开着"时本来就发生的那些命令。

- `runtime.cpp` / `runtime.hpp`：`std::atomic<bool> forceTiming`、`ForceTiming()`、`SetForceTiming()`
- `nr/model_settings.cpp`：`GetModelSettings()` 里 `if (ForceTiming()) gpuTiming = true;`；`SetForceTimingPersisted()` 写回 INI
- `panel_ui.cpp`：`GPU timing / errors` 段顶部的复选框 `Force GPU timing (panel closed)`，勾选即持久化到 `[Diagnostics] ForceTiming`

面板关闭时多出的开销：一帧 4 次 `EndQuery` + 1 次 `ResolveQueryData` + 1 次 readback `Map`。`Map` 让 CPU 等 GPU，**降低**输入延迟、略降吞吐（类似 Reflex 的单帧在飞）。

## 本轮删除的临时物

`NR heartbeat`、`NR target:` 日志、`ForceMeasure`、`FlushProbe`、`DirectResolveFlush`、resolve 后的 `UAV` barrier、`capture12.cpp` 里为诊断加的 `mix()` 字段 —— 全部移除，`ReadTiming` 条件与 `pass.cpp` 时间戳块恢复为基线形态。

收尾复查又删掉三处无行为影响的东西：`ModelRuntime::OwnsDevice`（声明+定义，从未被调用）、`pass.cpp` 里 `!ctx->proxy` 分支中重复的 `CheckFeatureSupport(UAV_TYPED_STORE)`（同一次调用里创建 context 时刚校验过同一 `format`）、以及 `gpu_ticket.hpp` 每次提交都重查的 `GetTimestampFrequency`（改为仅在频率未知时查）。

## 部署（最终）

标准 `1123328 B @ 20:46:20`（19 个目标）、HDR `1142784 B @ 20:46:23`（`remnant2` / `manorlords`）。只有 `dsdc` 的 INI 带 `ForceTiming=1`，其余 20 个没有该键（即默认 0）。

## 各游戏生效方式

- `dsdc`：INI 里 `ForceTiming=1`（必须开，否则回到"顶部一条"）
- 其余 16 个目标：默认 `0`，行为与之前完全一致

## 适用边界：只有 `DirectResolve` 需要它

关键不是"direct 不 direct"，而是**我们最后一步怎么把结果交给游戏**：

| | 收尾动作 | 谁保证游戏后续能读到 |
|---|---|---|
| 非 direct（20 个目标） | `CopyNrResult12`：`CopyTextureRegion` 拷进游戏颜色，拷完立刻 transition 出 `COPY_DEST`（`nr/capture12_input.hpp:68`） | 拷贝引擎的写 + 出 `COPY_DEST` 那次转换 |
| direct（`dsdc`） | 模型 compute 以 UAV typed store 直写游戏颜色，**资源状态不变**，仍是 `UNORDERED_ACCESS`（`nr/pass.hpp:53`） | 没人 → 只能靠末尾排空 |

所以 `ForceTiming` 的语义是"在录制末尾补一次完整管线排空"，只在**最后一步是 UAV 直写别人的资源、且不能改这个资源的状态**时才必须。21 个目标里 `DirectResolve=1` 只有 `dsdc`，`ForceTiming=1` 也只有它。别的游戏打开不会坏，但纯属白花 GPU 时间。

将来出现"部分画面跟不上 / 边界块状乱跳"（而不是整体不对）的游戏，第一个该试的就是这个开关。

## 它不是低延迟开关（纠正早前说法）

把它描述成"CPU 等 GPU、降低输入延迟"是错的。`ReadTiming` 入口就有非阻塞前检查：

```cpp
if (!timing || !frequency || !input.ticket->Complete()) return;   // nr/pass.cpp:179
```

`Complete()` 只是 `fence->GetCompletedValue()` 轮询（`nr/gpu_ticket.hpp:20`），GPU 没跑完直接 return，`Map` 永远不会阻塞 CPU。ForceTiming 只多出 GPU 侧的 4 次 `EndQuery` + 1 次 `ResolveQueryData`，对延迟零影响。

## 若将来真要做低延迟（本次不做）

DX12 侧现在没有任何 CPU 阻塞：`inFlight` 只靠轮询 `Complete()` 清理（`nr/pass.cpp:517`），capture 槽位占满时直接跳过 NR（`PublishCaptureBusy`），不会 stalls。要压到"在飞 1 帧"必须显式 CPU 等 GPU，天然落点是 `Present`/`Present1` 返回前等本帧最近一次 NR 提交的 fence —— `Submitted()` 里每次提交都已经 `queue->Signal(job->fence, next)`（`nr/capture12.cpp:668`），记下最新的 fence/value 即可。对照：DX11 native 路径已经有这种硬阻塞（见 `DLSSNR_LiteDX11/docs/DEPLOYMENTS.md` 同日条目）。

# 2026-11-03 死亡搁浅 DC「只有顶部一条有 NR」定位结论

## 现象

`DirectResolve=1` 时，面板关闭只有屏幕顶部约 100 行显示 NR 效果，边界呈**块状且逐帧变化**；面板打开则整屏正常。

## 已用日志排除的假设

| 假设 | 证据 | 结论 |
|---|---|---|
| NR 没执行 | `NR heartbeat: applied=1 edited=1 ready=1 resolve=game UAV` 在 `panel=0/1` 下完全相同 | ❌ |
| 输入路由不同 | `NR route:` 两状态下 `color/depth/mv` 全为 `copy`、尺寸格式一致 | ❌ |
| meter/回读同步掩盖问题 | `ForceMeasure=1` + 面板关闭仍马赛克 | ❌ |
| 解析目标是多 mip / 数组 / 有子矩形 | `NR target: 3840x2160 mips=1 array=1 fmt=24 flags=0x5 declared out=0,0 3840x2160` | ❌ 完全正常 |
| resolve 只写了一部分（几何裁剪） | `NR resolve: ctx=3840x2160 cb=3840x2160 dispatch=480x270 target=3840x2160`，`panel=0/1` 完全一致 | ❌ 整帧覆盖 |
| UAV 写入未发布 | 加了 `D3D12_RESOURCE_BARRIER_TYPE_UAV` 后现象不变 | ❌ |

## 品红探针的决定性观察

resolve 输出硬编码品红（每 2 秒切换一次）：

- 面板关：品红只出现在**顶部约 100 行**，边界呈**块状并逐帧变化**
- 面板开：品红**整屏**

## 结论

写入范围是整帧、格式尺寸全部正常，但**只有一小部分写入对最终画面可见，且可见区域逐帧随机** —— 这是「读取方在我们的写入落地前就读取了该资源」的典型特征。

面板打开时额外增加的 GPU 工作量恰好把读取方推迟到我们之后，所以看起来正常。

也就是说：**`DirectResolve` 把结果写回游戏颜色资源这条路径，与 DSDC 自身的消费时序存在冲突**；消费方不在我们写入的这条命令列表的下游，因此同队列的顺序保证覆盖不到它。这不是参数错误，也不是某一行代码写错。

这也解释了为什么 `DirectResolve=0`（私有输出 + 列表内拷回）同样不行 —— 拷回也在同一条列表里，同样赶不上那个消费方。

## 若要根治

只能改变写入时机，例如把 resolve 推迟到消费者之后（Present 前由我们自己的队列 + fence 完成），属于架构级改动。`dsdc` 目前仍保持 `DirectResolve=1` 的原始行为。

## 本轮代码现状

- `capture12.cpp`：`direct[n]` 恢复为 `!directResolve` 全局否决（保留一条实测结论注释）；新增 `NR target:` 一行日志（仅在路由变化时打印，记录解析目标形状与游戏声明的输出子矩形）
- `pass.cpp`：`DirectResolve` 路径 resolve 后保留一次 `UAV` barrier（未证实能修复上述问题，但无害且仅此路径执行）
- 品红探针、`NR resolve:` 每秒日志、`ForceMeasure`、`NR heartbeat` **全部已删除**
- 产物：标准 `1122816 B @ 19:03:44`，HDR `1141760 B @ 19:03:47`，17 个目标一致

# 2026-11-03 全部回退到 `DirectResolve` 优化之前

`/W4` 零错误零警告。产物尺寸回到与 02:12:15 那版**完全相同**：标准 `1121792 B @ 13:54:33`，HDR `1141248 B @ 13:54:37`。

## 已撤销的三项改动

| 项 | 内容 | 结论 |
|---|---|---|
| 输入直读 | `direct[n]` 曾收窄为"只有颜色拷贝" | 死亡搁浅实测重现马赛克，**不成立**，已恢复 `!directResolve` 全局否决 |
| `NR heartbeat` | `Level=2` 每秒一次的执行摘要 | 诊断用，已删除 |
| `Diagnostics.ForceMeasure` | 面板关闭时强制跑 meter | 实测 `ForceMeasure=1` + 关面板仍然马赛克 → **排除 meter/回读同步**，已删除 |
| resolve 后 UAV barrier | 试图解释"开面板才正常" | 未验证且属于猜测，已删除 |

## 实测结论（本次唯一收获）

- NR 在面板开/关两种状态下执行**完全相同**：`applied=1 edited=1 ready=1 resolve=game UAV`，输入路由也一致（`color/depth/mv` 全 `copy`）。
- 所以"开面板才正常"**不在 NR 链里**。
- `ForceMeasure=1`（面板关闭、meter 强制运行）仍然马赛克 → 与周期性 GPU 同步无关。
- `DirectResolve=0` 在死亡搁浅上确实不行（用户实测），这条开关不可替代。

## 现状

- 代码：`capture12.cpp` 的 `direct[n]` 判定 = `model && !directResolve && !preview && DirectShaderInput12(d, n == 1)`；注释记录了"directResolve 必须全拷"这条实测结论。其余三处诊断代码全部移除。
- 17 个目标全部刷新为 13:54:33 / 13:54:37。
- `dsdc` INI：`DirectResolve=1`、`Level=0`、`WhiteMode=2`、`RequireComputeSignature=0`，临时键 `ForceMeasure` 已删。

## 未解决

"开面板才正常、关面板马赛克"仍未定位。已排除：NR 未执行、meter 同步、输入路由差异、resolve 位置（`DirectResolve=0` 更差）。下一步方向只剩**面板绘制本身对 swapchain backbuffer 的状态转换**（它可能是唯一让 UAV 写入可见的动作），但需要原文层面的验证方法，暂不动手。

# 2026-11-03 `DirectResolve=1` 的输入直读修复（死亡搁浅 DC）

`/W4` 零错误零警告。标准版 `1121792 B @ 13:27:41`，HDR 版 `1141248 B @ 13:27:47`。

## 问题

[capture12.cpp:459] 的直读判定把 `directResolve` 当成了全局否决：

```cpp
direct[n] = model && !directResolve && !preview && DirectShaderInput12(d, n == 1);
```

但 `resolveTarget` 只会是 `resources[0]`（颜色）。深度和运动永远不可能与它别名，却被一起打回拷贝路径。于是 `DirectResolve=1` 为了省掉 1 次输出拷贝（33 MB @4K），多花了 3 次输入拷贝（颜色 33 + 深度 66 + 运动 33 = 133 MB），净亏 ~100 MB/帧。

## 修复

```cpp
direct[n] = model && !preview && DirectShaderInput12(d, n == 1) &&
            !(directResolve && n == 0);
```

`pass.cpp:366` 的「resolve needs immutable inputs」护栏不受影响 —— 它检查的是 `resolveTarget == 任意输入`，而颜色本身仍然走拷贝路径，深度/运动格式不同，不可能撞上。

| 配置 | 颜色 | 深度 | 运动 | 输出 | 合计 @4K |
|---|---|---|---|---|---|
| `DirectResolve=0` | 0 | 0 | 0 | 33 MB | 33 MB |
| `DirectResolve=1` 修复前 | 33 | 66 | 33 | 0 | 133 MB |
| `DirectResolve=1` 修复后 | 33 | 0 | 0 | 0 | **33 MB** |

## 影响面

17 个 DX12 目标里只有 **`dsdc`（死亡搁浅：导演剪辑版）** 开了 `DirectResolve=1`，其余 16 个走默认路径，行为不变。

## 部署

`dxgi.dll` + `nvngx.dll_litedx12.dll` 写入 17 个目标，34 项逐一核对尺寸与时间戳，全部一致。备份在 `build\deploy-backup\20261103-1330\<目标>\`。

**验证点**（`dsdc` 开 `Level=2`）：

```
NR route: model=1 preview=0 resolve=game UAV color=copy 2560x1440 fmt=X
          depth=direct ... mv=direct ... exposure=direct
          ↑ depth / mv 应为 direct（修复前是 copy）
```

# 2026-11-03 `DirectResolve=1` 输入直读尝试 —— 已回滚

`/W4` 零错误零警告。标准版 `1121792 B @ 13:33:45`，HDR 版 `1141248 B @ 13:33:50`。

## 尝试

曾把 [capture12.cpp:462] 的直读判定从"`directResolve` 全局否决"收窄为"只有颜色让路"：

```cpp
// 尝试过，已撤回
direct[n] = model && !preview && DirectShaderInput12(d, n == 1) &&
            !(directResolve && n == 0);
```

依据是 `resolveTarget` 只会是 `resources[0]`，深度/运动不可能与它别名。带宽上是 133 MB/帧 → 33 MB/帧。

## 结果：真机不成立，已回滚

死亡搁浅 DC 实测**重现了"未开 direct 时的错误"**。也就是说 `DirectResolve=1` 要求**所有输入都走拷贝**，原因与别名无关 —— 在这个游戏里深度/MV 的原地直读路径本身不可用（具体机制未查明）。

已恢复原判定：

```cpp
direct[n] = model && !directResolve && !preview && DirectShaderInput12(d, n == 1);
```

并在注释里记下这条实测结论，避免以后重复踩。

## 结论

`DirectResolve=1` 的 133 MB/帧代价**是这条路径的固有成本**，不是可优化的浪费。若将来要压缩，方向只能是"查明为什么直读深度/MV 会失败"，而不是调整条件。17 个目标里仍只有 `dsdc` 使用该模式。

## 部署

17 个目标全部回到 13:33:45 / 13:33:50 版本，文件级核对一致。备份 `build\deploy-backup\20261103-1334\`（即出问题那一版）。

# 2026-11-03 全目标刷新（18 个 DX12 目标）

`/W4` 零错误零警告。标准版 `nvngx.dll_litedx12.dll` `1121792 B @ 02:12:15`，HDR 版 `1141248 B @ 13:00:53`（`Build.ps1 -HDR`，`dist\Release-HDR`）。

## 写入范围

写入 `dxgi.dll`（171008 B @ 11-02 23:38）与 `nvngx.dll_litedx12.dll` 两个文件；INI 与模型按脚本语义**只在新目录里补写，已有的保留**。

| 目标 | 变体 | 结果 |
|---|---|---|
| remnant2 / manorlords | HDR | 1141248 B @ 13:00:53 |
| jedi / gta / wolong / mhwilds / wdlegion / fh6 / kcd2 / starfield / tsushima / nioh3 / dsdc / msfs2024 / sm2 / stellarblade / alanwake2 | 标准 | 1121792 B @ 02:12:15 |

`ronin`（`G:\Games\Ronin`）、`uncharted`、`dd2` 三个目录在本机不存在，未处理。

18 个目标 × 2 文件 = 34 项逐一核对尺寸与时间戳，全部与对应 `dist` 一致。按既有约束**不读 DLL 内容、不做哈希**。

## 回滚

`tools\deploy.ps1` 对这两个文件"always overwritten, no backup"，所以本轮手工备份到 `build\deploy-backup\20261103-1305\<目标>\`（16 个目录，各含旧的 `dxgi.dll` 与 `nvngx.dll_litedx12.dll`，即 10-28 那版 1130496 / 1149952）。`alanwake2` 此前已是新版，无需备份。

## INI 迁移（WhiteMode 重编号 + 死键清理）

部署脚本对已存在的 INI **只保留不覆盖**，所以 10-28 那批 INI 里 `WhiteMode` 还是旧编号（`{Automatic,Game,Measured,Manual}` = 0/1/2/3）。而 `model_settings.cpp:21` 是裸转换 `WhiteMode(GetPrivateProfileIntW(...))`，**没有旧值迁移** —— 直接跑新构建会把"Game"读成"Measured"、"Measured"读成"Manual"、"Manual"被钳成"Game"。

16 份 INI 按"保留原意"迁移（`alanwake2` 早已是新语义，未动）：

| 旧值 | 新值 | 原意 | 游戏 |
|---|---|---|---|
| 1 | 0 | Game | remnant2, jedi, gta, mhwilds, starfield, msfs2024, manorlords |
| 2 | 1 | Measured | wolong, kcd2, tsushima, nioh3, sm2 |
| 3 | 2 | Manual | wdlegion, fh6, dsdc, stellarblade |

同时：删除死键 `ApplyModel`（代码已删）、把注释行改成 `; 0 Game, 1 Measured, 2 Manual.`。`mhwilds` 额外把 `Level` 提到 **2** 用于排查抖动/拖影。

复核：每份文件减 1 行（37 B），`[NeuralRendering]` 段落结构完整。备份在 `build\deploy-backup\20261103-1305\<目标>\DLSSNR_LiteDX12.ini`。

**注意**：迁移后若回退到 10-28 那份 DLL，`WhiteMode` 语义会再反过来（新 `1` 在旧代码里是 Game，新 `2` 是 Measured）。只有 `0` 在两边都表示 Game。

## 本轮包含的代码变化

- `command_guard::Track()` 无锁快路径（`generation` 校验），见下一节
- `panel_gpu.cpp` / `panel_impl.hpp`：`swap3` 缓存、预览描述符缓存、`srvStride`

**全部 18 个目标尚无真机验证。** 第一嫌疑仍是 `Track()` 热路径，回滚方式见该节说明。

# 2026-11-03 面板开销收敛（预览描述符缓存 + 每帧 QueryInterface）

`/W4` 零错误零警告，产物 `nvngx.dll_litedx12.dll` `1121792 B`，部署到《阿兰维克 2》（`G:\EpicGames\AlanWake2`），与 `dist\Release` 尺寸与时间戳一致；`dxgi.dll`（171008 B）、INI、266 MB 模型未动。

## `command_guard::Track()` 把锁移出热路径

`command_guard` 是借用游戏 command list 的入场费：D3D12 没有 root signature / descriptor table / root constants 的查询 API，只能在游戏每次调用时影子记录（24 个钩子方法），插入 NR 前拷快照、录完逐条重放。原实现每次调用都要 `recursive_mutex` + `std::map<list*,State>::find`，约 80–110 ns；典型引擎每 draw 触发 3–8 次 → 1–2.5 万次/帧。

关键观察：**锁保护的是 map 的结构（插入/删除），不是 `State` 的字段**；而一条 D3D12 command list 同一时刻只被一个线程录制，所以 `State` 的读写在时间上天然串行。于是把「定位 State」从每次调用降到每线程一次：

- `StateBucket` 增加 `std::atomic<uint64_t> generation`，在 `Obtain()` 插入、`~Cookie()` 删除、`Obtain()` 失败回滚时 `fetch_add`（release）
- 线程本地 `trackCache`（bucket / list / `State*` / generation / epoch）
- `Track()` 热路径：`suspended` 与 epoch 判定后比对 generation，命中则**不拿锁、不查表**，直接改 `State`
- 冷却路径保留原逻辑（锁 + `Obtain` + epoch 复核），命中后回填缓存

安全论证写进了代码注释：`State` 的单写者是录制线程；`State` 被 erase 的前提是 list 引用计数归零，而 D3D12 不允许销毁正在录制的 list；generation 负责兜住「list 已销毁 / 地址被复用」的情况。每次调用降到 ~10–25 ns（lambda 本体除外）。

## 预览描述符不再每帧重写

`Draw12` 此前每帧对预览资源做 `GetDesc()` + `CreateShaderResourceView()`。现在按 `frame.preview->image.Get()` 与 `frame.previewSized` 比较，只在预览图变化时写描述符与尺寸；`GetDescriptorHandleIncrementSize(CBV_SRV_UAV)` 提到 `InitRenderer` 存进 `srvStride`（每个设备恒定）。`previewSized` 用 `ComPtr` 持有 —— 用资源指针做缓存键就必须持有它，否则地址可能被复用而误判。

## 每帧 QueryInterface 去掉

`Draw12` 每帧做一次 `swap->QueryInterface(IDXGISwapChain3)` + AddRef/Release。`InitRenderer` 里本来就有同样的 QI，现在结果存进 `Impl::swap3`，`Draw12` 只调 `GetCurrentBackBufferIndex()`（这个必须每帧取，它随呈现变化）。

`DestroyRenderer()` 里加 `swap3.Reset()`：持有的交换链接口不能活过渲染器，否则会拖住交换链释放 —— 和 DX11 侧 RTV 那条注释同一个理由（`BeforeResize` 会先调 `DestroyRenderer`，所以换 buffer 一定重建）。

至此两个工程的 Draw 路径都没有多余的每帧驱动/COM 调用。

## 未动

`FrameLedger` / `StatusRegistry` 两把锁的划分保持不变。状态锁在 `capture12.cpp` 只有 3 处（`Report` 与两处发布），每帧实际触发 2–3 次 ≈ 50 ns，合并成一把锁省不到噪声级别，却会让面板读状态去等录制 —— 方向相反。

# 最新部署：2026-10-29 阿兰维克 2 崩溃修复（根因：替换了 DXGI 工厂对象）

《阿兰维克 2》10-28 19:18 更新（`AlanWake2.exe` + 三个 `amd_fidelityfx_*_dx12.dll`；`sl.*.dll`、`nvngx_dlss*.dll` 均未变动）。更新后启用插件会失去设备：`Present` 返回 `DXGI_ERROR_DEVICE_REMOVED`，`GetDeviceRemovedReason()` 为 `0x887A002B`（`DXGI_ERROR_ACCESS_DENIED`），系统日志无 TDR。同一目录换用 OptiScaler 的 `dxgi.dll` 连续 32190 帧无异常。

## 定位（每次只改一个变量）

| 配置 | 结果 |
| --- | --- |
| `[General] Enabled=0`，只剩 dxgi 转发壳 | 正常 |
| 启用插件 + 不包装交换链 + 仍替换工厂对象 | 进不到主菜单 |
| 启用插件 + 不包装交换链 + **不**替换工厂对象 | 正常 |
| 启用插件 + 包装交换链 + 不替换工厂对象 + NR 开 | 正常，`NR model evaluated`，画面正常 |

结论：根因是**把原生 DXGI 工厂对象换成了插件自己的 `Factory` COM 对象**，与交换链包装器无关。游戏与其中的 AMD FidelityFX / Streamline 组件拿到的不是 DXGI 自己的工厂后，设备以 `ACCESS_DENIED` 被判定失效。OptiScaler 默认走 `HookToFactory`（读 `pFactoryVTable[10]` 再 `DetourAttach`），它的 `WrappedIDXGIFactory7` 是 `[Spoofing] DxgiFactoryWrapping`，默认关闭。

## 改动（本工程只保留这一处逻辑改动）

`src/hooks.cpp` — `AttachFactory` 不再 `new capture::Factory` 并替换 `*output`，改为只对原生工厂的 vtable 槽位下钩：`CreateSwapChain`（10）、`CreateSwapChainForHwnd`（15）、`CreateSwapChainForCoreWindow`（16）、`CreateSwapChainForComposition`（24）。这些槽位是所有同版本工厂实例共享的代码地址，一次成功即覆盖全部工厂；游戏与中间件始终持有 DXGI 自己的工厂对象。

随之删除只服务于对象替换的死代码：`capture::Factory` 类（约 85 行）、`FactoryIID`、`CaptureAdapterInterface`、`CaptureAdapter`、`Parent` / `HParent` 钩子、`lastParent`。

## DX11 同步

DX11 存在同一处隐患：`wrappers.hpp` 里的 `Factory` 包装类和 `AttachFactory` 里的对象替换，与 DX12 崩前完全同构。已按同样方式同步：删除 `Factory` 类（-81 行）、删除 `FactoryIID`、`CaptureAdapterInterface`，`AttachFactory` 改为只挂钩子（另保留它独有的 `D3D11CreateDeviceAndSwapChain` 钩子）。DX11 的 `CaptureAdapter` 是活的（`D3D11CreateDevice` 后取适配器），保留。

## 刻意做过又全部撤回的东西

排查期间加过、并最终全部删除的：`[Diagnostics] HookMask` 钩子分组掩码、`[Diagnostics] WrapForeignSwapchains`、`[Diagnostics] WrapSwapchain` 总开关、`ModelRequested() || DebugActive()` 录制门控、`ID3D12Device::CreateCommandQueue` 队列钩子、`Panel::Draw` 每帧补装、`installedDevice` / `installedQueue` 原子缓存、`VtableModule()` 与 `Swapchain N ... owned by ...` / `ResizeBuffers ...` / `NR model evaluated` 三条日志、`ResizeBuffers` 前等 GPU 空闲。

除上述逻辑改动外，两个工程其余文件与 2026-10-29 12:48 的备份**逐字节一致**，INI 模板也回到原样（无 `WrapSwapchain`）。二进制相对基线**只少一条字符串**（`Adapter GetParent capture failed`，死代码），无任何新增字符串。

## 多交换链：实测无需处理

改为 hook 工厂方法后，会看到比"替换工厂对象"更多的调用方：Streamline 自己建的链过去看不到，现在看到了。阿兰维克 2 因此为第二条链（vtable 在 `sl.interposer.dll`）也建了一个覆盖层面板。

曾在未实测的前提下推测这条链会抢走输入路由（`WindowSubclassRegistry::Active()` 返回 `owners.back()`），并为此加过机制。**2026-10-30 00:12 实测推翻了该推测**：两条链都被包装、NR 正常执行、Insert 菜单正常。原因：该注册表按 `HWND` 分路由，而 Streamline 那条链建在另一个窗口上；日志也显示它从未 `ResizeBuffers`、不走 Present，不是游戏的呈现链。代价仅是那条链上多一个从不绘制的 Panel 对象。

## 构建

| 变体 | 大小 | MD5 |
| --- | --- | --- |
| `dist\Release\nvngx.dll_litedx12.dll` | 1122304 | 见当次构建 |
| `dist\Release\dxgi.dll` | 171008 | 见当次构建 |
| `dist\Release-HDR\nvngx.dll_litedx12.dll` | 1141760 | 见当次构建 |
| `dist\Release-HDR\dxgi.dll` | 171008 | 见当次构建 |
| `dist\Release\nvngx.dll_litedx11.dll` | 1237504 | 见当次构建 |

构建是确定性的（相同源码两次构建哈希一致）。**注意：代理 `dxgi.dll` 与早期部署的 `3F7D7A207678` 哈希不同，但字符串集合与导出表完全一致**（15 个导出，序号 2/3/4/10–21 对齐系统 dxgi），差异只在构建元数据；判断"代理是否改变"要用源码字节比对，不能用哈希。

## 部署状态

目前只部署到 `alanwake2` 并验证。其余 20 个 DX12 目标与 4 个 DX11 目标仍是旧主体（含工厂对象替换），等待决定。该目录的 OptiScaler `dxgi.dll` 已改名 `dxgi.dll_optiscaler` 保留，未删除。

阿兰维克 2 配置：`Enabled=1`、`Level=0`、`RequireComputeSignature=0`（Remedy 引擎在 NGX evaluate 的命令列表上不绑计算根签名，与 dsdc / wdlegion / msfs2024 / fh6 / starfield 同类）、NR 开启。

# 最新部署：2026-10-25 死代码清理后的全量部署（HDR 目标按变体分发）

DX12 主体清理掉一批只写不读的成员后重新构建，`dist\Release` 为 1129984 字节（MD5 `CA05BFF539F0`），`dist\Release-HDR` 为 1152000 字节（MD5 `C359DA1219C3`）。两个变体分别部署：标准版铺到 jedi、gta、wolong、mhwilds、ronin、wdlegion、fh6、kcd2、starfield、alanwake2、uncharted、dd2、tsushima、nioh3、dsdc、msfs2024、sm2、stellarblade、coldwar，HDR 版铺到 remnant2 与 manorlords。PROXY `dxgi.dll` 标准版为 `3F7D7A207678`、HDR 版为 `7DFE2CA1B37A`，与各自主体对应；INI 与模型全部保留。

冷战的 `C:\Program Files\WindowsApps\38985CA0.CODCWGame_...` 与 `G:\WindowsApps\38985CA0.CODCWGame_...` 都是 junction，最终指向 `G:\XboxGames\Call of Duty- Black Ops Cold War\Content\`。因此冷战的插件只有一份，位置就是 `Content`；通过包路径写入等同于写 `Content`，不是第二个安装点，不要再去包路径重复部署。按包路径启动时日志里的 `Runtime:` 行会显示 junction 路径，那只是模块名解析结果，不代表另有一份文件。

首次分发时遗漏了 HDR 目标：`tools/deploy.ps1` 只有全局 `-HDR` 开关，逐目标循环时会把标准版写进 Remnant 2 和 Manor Lords。已修正脚本：新增 `$hdrTargets`（remnant2、manorlords），`-Target` 命中该列表时自动选 `dist\Release-HDR`，`-HDR` 仍可强制、新增的 `-Standard` 可反向覆盖；不带 `-Target` 运行时会逐游戏打印「应为哪个变体 / 当前是哪个变体」。两个 HDR 目录已按此重新部署，运行时长 1152000 字节。

本批未启动任何游戏。DX11 侧只重新部署上一轮构建（`A0A73ED7CE14`），源码未改。

# 最新部署：2026-10-24 新增 Call of Duty: Black Ops Cold War（标准 DX12）

按用户要求部署到 `G:\XboxGames\Call of Duty- Black Ops Cold War\Content`，登记目标 `coldwar`。使用标准非 HDR 的 `dist/Release`；部署前运行 `Build.ps1 -Configuration Release`，preview/white_point/pass 三个目标重新编译，主体重新链接通过。本次没有修改 DX12 运行时源码或 HDR 包。

目录原先没有代理、Lite 主体、配置或 NR 模型，无入口冲突。写入四个文件，全部 SHA256 与源文件一致：

| 文件 | SHA256 |
| --- | --- |
| `dxgi.dll` | C0341DAC326F77BA95CB41E4012C06A21D8430F74EB6083A75B0AFBA2B10628F |
| `nvngx.dll_litedx12.dll` | EEDC17F0A13B936FA485A88B5421AA6315F57249E35BE1081A646A7271B62EA2 |
| `DLSSNR_LiteDX12.ini` | 5931D00A44DE4F349FA55FB1ED3130682082450E2B328994731268A608EB839C |
| `nvngx_dlssnr.dll` | 2BC19BC09A7B89DD44F73E2AC0108FCC586E2154BDD0830F242F4F040E28789F |

NR 模型为 310.9.16.0，源文件签名 Valid。默认配置 NR 关闭、Preset=0、RequireComputeSignature=1。游戏原有 `nvngx_dlss.dll` 部署前后 SHA256 均为 `5CACE9593D6092ACF53B485208EFC08A7A3E7294DCBF1E51F2322941D825C726`，未替换原生 DLSS。

未启动游戏，实际 DX12/NGX 接入待用户运行确认；其余 DX12 游戏目录未刷新。[完整核验](../diagnostics/2026-10-24/coldwar-deploy/verified.json)。

# 最新部署：2026-10-24 新增 Manor Lords（WinGDK，HDR 变体）

按用户要求部署 **HDR 变体**到 `F:\XboxGames\Manor Lords\Content\ManorLords\Binaries\WinGDK`，新登记目标 `manorlords`（`tools/deploy.ps1` 的 `$games` 已同步）。该 GDK 包根是 `...\Content`，但 exe、`D3D12\D3D12Core.dll` 与 `Plugins\DLSS` 都在 `ManorLords` 下，因此按 SM2 的做法放在 exe 旁边，而不是包根。部署前核对：目录内没有 `dxgi.dll`、`nvngx.dll_litedx12.dll`、`DLSSNR_LiteDX12.ini`、`nvngx_dlssnr.dll`，无同名冲突，目录可写。

部署前先跑 `Build.ps1 -HDR`：`dist\Release-HDR` 的主体仍是当天 01:07 的旧链接，早于 15:02 的 HDR 源码，`preview.cpp`、`white_point.cpp`、`pass.cpp`、`hdr_output.cpp` 重新编译并重新链接；`dxgi.dll` 代理无需重编（Up-to-date）。随后用 `tools\deploy.ps1 -Target manorlords -HDR` 部署。

| 文件 | SHA256 |
| --- | --- |
| `dxgi.dll` | 3547034AA9191E9511168DB1CD4A45A16822459274778AC0A6A854E599E6CE02 |
| `nvngx.dll_litedx12.dll` | 037A2E1F0F89A299060864778FA998EA4840DE9B5DE483AC9D0B19A235909FB0 |
| `DLSSNR_LiteDX12.ini` | FA04A9C2A4879F47794BCC5C5857A87187C6F783DB5F1009AAA42DD09822448D |
| `nvngx_dlssnr.dll` | 2BC19BC09A7B89DD44F73E2AC0108FCC586E2154BDD0830F242F4F040E28789F（310.9.16.0，签名 Valid） |
| `Licenses/RenoDX_HDR_MIT.txt` | 1783 字节，HDR 变体附加 |

新目录首次部署写成 HDR 模板配置：NR 默认关（`NeuralRendering.Enabled=0`）、`Preset=0`、`RequireComputeSignature=1`，并追加 `[HDR]`（`Method=0`、`InputEncoding=2`、`PeakNits=1000`、`SdrReferenceNits=100`、`InputGrayLinear=0.18`、`OutputGrayNits=18`）。游戏自带的 `Plugins\DLSS`、`Plugins\StreamlineCore` 与 `D3D12\D3D12Core.dll` 均未改动；未启动游戏，DX12 / NGX 接入与 HDR 转换都待用户运行验证。

HDR 转换的运行条件按 README：Windows 已开启 HDR、关闭 Windows Auto HDR 与 RTX HDR、SDR `RGB10A2` flip-discard 交换链、阻塞 Present、一个 direct 队列；不满足时日志写 `HDR: conversion unavailable or failed`，NR 本身不受影响。游戏当前 `GameUserSettings.ini` 为 `bUseHDRDisplayOutput=False`；若在游戏内开启原生 HDR，插件会按「原生非 SDR 色彩空间」直接旁路，不做二次转换。是否命中 `RGB10A2` 交换链需看运行后的 `DLSSNR_LiteDX12.log`。

# 最新部署：2026-10-23 全列表更新（发现 dist 陈旧，全量重建后部署）

按用户要求把 `tools/deploy.ps1` 的 18 个 DX12 目标全部刷新到 `dist\Release`。部署前状态：jedi / gta / wolong / mhwilds / ronin / fh6 / kcd2 / starfield / alanwake2 / uncharted / dd2 / tsushima / nioh3 / sm2 为 `DE2176C5158B`（MD5），wdlegion / dsdc / stellarblade 为 `8C307C091219`，msfs2024 为 `61A7885ADF45`。

部署前先跑增量构建校验，发现 `dist\Release` 里的 `nvngx.dll_litedx12.dll`（`30F7B67F…`，2026-10-22 00:17）与现有源码不同步：`preview.cpp`、`white_point.cpp`、`pass.cpp` 三个目标被重新编译并重新链接（`48C04C99…`）。为避免再出现陈旧对象，随后执行 `Build.ps1 -Configuration Release -Fresh` 全量重建，36 个目标全部重编，最终产物为下表哈希，18 个目标按此刷新。

| 文件 | SHA256 |
| --- | --- |
| `dxgi.dll` | C0341DAC326F77BA95CB41E4012C06A21D8430F74EB6083A75B0AFBA2B10628F |
| `nvngx.dll_litedx12.dll` | B60128100D598E112FF2223E76AAF2F671F63295EB81B4617AE56332C61ED8CB |
| `DLSSNR_LiteDX12.ini` | 各目录既有文件保留，未覆盖 |
| `nvngx_dlssnr.dll` | 各目录既有模型保留，未覆盖 |

18 个目录逐个复核 `dxgi.dll` 与 `nvngx.dll_litedx12.dll` 的 SHA256 均与 `dist\Release` 一致。本次没有启动任何游戏（项目约定「真实游戏只由用户运行」）；同批 DX11 四个目标（roe / bg3 / wildlands / bannerlord）的更新记录见 [DLSSNR_LiteDX11 部署记录](../../DLSSNR_LiteDX11/docs/DEPLOYMENTS.md)。

# 最新部署：2026-10-21 新增微软模拟飞行 2024（DX12）

按用户要求部署到 `F:\XboxGames\Microsoft Flight Simulator 2024\Content`，DX12，作为第 17 个 DX12 登记目标 `msfs2024`（`tools/deploy.ps1` 的 `$games` 已同步）。部署前核对：目录内没有 `dxgi.dll`、`nvngx.dll_litedx12.dll`、`DLSSNR_LiteDX12.ini`、`nvngx_dlssnr.dll`，无同名冲突，目录可写。

| 文件 | SHA256 |
| --- | --- |
| `dxgi.dll` | 11076F50635B213BD3C04DBC5F07B24D4D43622A4A3F9CB1F71059A4F8CFC3CD |
| `nvngx.dll_litedx12.dll` | 4410C9C264E0954D3D42323030568D0B18AEEA1B0820AFF8644C0A910C49586A |
| `nvngx_dlssnr.dll` | 2BC19BC09A7B89DD44F73E2AC0108FCC586E2154BDD0830F242F4F040E28789F（310.9.16.0，签名 Valid） |
| `DLSSNR_LiteDX12.ini` | 5931D00A44DE4F349FA55FB1ED3130682082450E2B328994731268A608EB839C |

新目录首次部署写成模板配置：NR 默认关（`NeuralRendering.Enabled=0`）、`Preset=0`、`RequireComputeSignature=1`。游戏自带 `nvngx_dlss.dll`、`nvngx_dlssg.dll`、Streamline 与 GDK 配置文件均未改动。核验：`diagnostics/2026-10-21/msfs2024-deploy/verification.json`。

首次实测（00:01:22，PID 30736）与死亡搁浅、看门狗军团相同：NGX 输入捕获已经到达，但每帧被 `DX12 waiting for a captured compute root signature` 拒绝，模型未执行。已按同一授权把该目录 `RequireComputeSignature` 设为 0（仅改这一个键，`NeuralRendering.Enabled` 等保持原值）。

第二次实测（00:07:54，PID 7756）确认开关生效（日志出现 `Compatibility: RequireComputeSignature=0`，签名等待消失），但暴露下一个阻断点：捕获返回 `E_NOTIMPL`（`Unsupported texture or view format`），随后面板一直报 `Game DLSS/DLAA observed, but no NR input captured`。即 NGX evaluate 已观察到，但三张输入里有一张过不了 `Capture12` 的资源校验；现有日志只写 HRESULT，不能区分是哪张、哪条规则。

为此在 [capture12.cpp](../../src/nr/capture12.cpp) 增加拒绝详情日志（仅在失败路径、按不同内容去重一行）：

`NR capture rejected: <规则>; index=<0 颜色/1 深度/2 MV> size=... format=... dimension=... array=... mips=... samples=... flags=0x... state=0x...`

对应三条校验：非单样本 2D 纹理、着色器读取状态但资源禁用 SRV、UAV 状态但资源不允许 UAV。主体更新为 `70803B1985875F9B24BEEFC3D52389951ED1835FE8CBA6CA06B246A588DE882A`，只部署到该目录；上一版 `4410C9C2…` 与当时的日志备份在 `diagnostics/2026-10-21/msfs2024-deploy/before/`。

第三次实测（00:14:26，PID 26684）取得具体输入：

```
NR capture rejected: not a single-sample 2D texture; index=0 size=3840x2160 format=26 dimension=3 array=1 mips=12 samples=1 flags=0x5 state=0x8
```

即 DLSS 输出是 `R11G11B10_FLOAT`（26）、`TEXTURE2D`、单样本、`RENDER_TARGET | UNORDERED_ACCESS`（0x5）、当前 UAV（0x8），唯一不符的是 **12 级 mip 链**。此前的校验要求 `MipLevels == 1`，于是整帧被拒；而实际只需要最顶层 mip。

修正（不改语义、只去掉不必要的限制）：

- `Capture12` 允许 `MipLevels >= 1`；拷贝目标固定为 1 级 mip（`d.MipLevels = 1`）。
- `CopyShaderInput12` / `CopyNrResult12` 改为只操作 **subresource 0**：用 `CopyTextureRegion` 配 `D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX` 取代 `CopyResource`，屏障也从 `ALL_SUBRESOURCES` 收窄到 subresource 0，避免动到本帧未声明的其它 mip 状态。单 mip 资源行为不变（subresource 0 即全部）。
- 直接 SRV 路径的 `DirectShaderInput12` 仍要求单 mip，多 mip 目标自动走拷贝路径。

主体 `30F7B67F…`，只部署到该目录，配置与模型未变。待用户重开游戏确认 NR 是否真正执行。

这是第三个触发签名保护的引擎；目前仍保留按游戏关闭，不改成全局默认。

目前 `RequireComputeSignature=0` 的目标：`dsdc`、`wdlegion`、`msfs2024`、`fh6`、`starfield`。这五个都在游戏里确认过：它们在 NGX evaluate 的命令列表上从不绑定计算根签名，开启保护会每帧被拒；其余 16 个目标保持 1。

# 部署历史：2026-10-19 看门狗军团计算根签名开关

看门狗军团（`wdlegion`）NR 与死亡搁浅同样停在 `DX12 waiting for a captured compute root signature`：10 月 17 日加的保护要求录制前已捕获计算根签名，而该游戏在 NGX evaluate 的命令列表上没有绑定计算根签名，于是每帧被拒。参考 OptiScaler 只在 `RestoreComputeSignature` / `RestoreGraphicSignature` 打开时才要求可恢复，本工程此前是无条件保护。

新增 `[Diagnostics] RequireComputeSignature`，默认 1（保持现有保护），现只在 `dsdc`、`wdlegion` 两个目录设为 0；保护条件本身未删除。主体 `4410C9C264E0954D3D42323030568D0B18AEEA1B0820AFF8644C0A910C49586A` 只更新这两个游戏，其他游戏仍为 `075D7CBD…`。代理 `11076F50…`、模型 310.9.16.0、其余 INI 设置保持原样（包括该游戏自带的 `NeuralRendering.Enabled=0`）；旧主体、代理与 INI 备份在各自 diagnostics 目录。构建通过，未启动游戏，NR 实际恢复待用户确认。核验：`diagnostics/2026-10-18/dsdc-compute-signature/verification.json`、`diagnostics/2026-10-19/wdlegion-compute-signature/verification.json`。

# 部署历史：2026-10-18 全部 20 个目标换用新版 NR 模型 310.9.16.0

只替换 `nvngx_dlssnr.dll`：22 份模型副本中 21 份改写为新版，GTA 此前已是新版故保持；113 个保护文件前后 hash 不变，包括全部 INI、代理、插件主体与原生 DLSS / REFramework 文件。插件仍是 10 月 17 日正式构建（DX12 `075D7CBD…`、DX11 `B93997CE…`），未重新部署。preset 一律未改：19 个游戏目录为 `Preset=0`（新模型 Default 即 B），GTA 的生效 profile 保持 `Preset=1`（A）。旧模型按 SHA256 去重只备份一份。未启动游戏。[详细记录](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/docs/ALL-GAMES-NEW-NR-2026-10-18.md)；[部署核验](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/diagnostics/2026-10-18/all-games-new-nr-v2/deployment.json)。

# 部署历史：2026-10-14 星空验收后清理，全部 20 个目标

用户已确认星空修复有效。16 个 DX12 游戏全部部署正式主体 4CB7542E…；4 个 DX11 游戏重新部署正式主体 A6987DE5…，对应代理、主体和受保护文件均校验通过。现有 INI、模型、原生 NGX / REFramework 保留。临时诊断源与旧 DLL 已收起归档。[清理、完整版本与逐项部署报告](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/docs/CONFIRMED-CLEANUP-2026-10-14.md)。下方是历史记录。

# 星空历史：2026-10-14 创建参数快照修复，正式版

星空已部署正式 Release 主体 `4CB7542E5A3C8B44C3C5DB8CD01655A7D94A8CBAC385336588688911E082DE6B`，替换临时诊断主体；代理、INI、模型均按 hash 保留。创建元数据改为原 Create 调用前保存，根因已由实测日志和正式代码 CPU 用例确认；Release / Debug 通过，游戏验收待用户确认。其他 19 个目标维持此前审查修复版。[详细报告](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/docs/STARFIELD-CREATE-METADATA-2026-10-14.md)。下方是历史记录。

# 星空历史：2026-10-14 撤回尺寸特例，隔离诊断

当前 Evaluate 声明优先和 Starfield 专用纹理尺寸兼容两项都已撤回。正式 dist/Release 恢复审查修复版 32FFFC22…；星空游戏目录暂为保持该版本渲染行为的隔离 feature 诊断主体，其他游戏不变。代理、INI、模型保留。[最新排查报告](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/docs/STARFIELD-STREAMS-2026-10-14.md)及其中部署核验给出完整 hash。下方两个尺寸尝试均为已撤回的历史记录。

# 星空单项历史：2026-10-14 限定纹理尺寸兼容

上一轮 Evaluate 输出声明优先方案无效，已撤回。星空主体更新为 `2AE9DB6AE4A6962DAE38EDAAC50AC59DFCFC493B62A56615ABEBA40488C008B7`：仅 Starfield.exe 的零原点输出使用参考的当前纹理尺寸路线，其他游戏仍为审查修复版。代理、配置、模型保留。Release / Debug 和 CPU 检查通过，实际恢复待用户确认。[最新报告](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/docs/STARFIELD-TEXTURE-2026-10-14.md)。下方单项版本记录已被替代。

# 星空单项更新：2026-10-14 当前 Evaluate 输出声明

星空主体更新为 `34BA792BCC9C0DAB921CB27FB901C019F5F65193E99DC9F9CEB45A100936D1A8`，其余游戏维持下方审查修复版。代理、配置与模型保留；Release / Debug 和 CPU 检查通过，装备界面切回的实际恢复待用户确认。[详细报告](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/docs/STARFIELD-EQUIPMENT-2026-10-14.md)。

# 当前部署：2026-10-14 审查修复版

16 个 DX12 登记目标全部更新并核验。主体 `32FFFC22C89F4AD86F4492CA6FF4CCA67365FB2AF053980EF5B1EE6F63298E63`；代理 `11076F50635B213BD3C04DBC5F07B24D4D43622A4A3F9CB1F71059A4F8CFC3CD`，代理没有变化。既有 INI、模型和原生 DLSS / REF 文件保留。修复键盘轮询重复工作，并补齐 DX11 原生深度按需捕获、尺寸保护及 feature 历史归属。星空新增尺寸拒绝详情，持续 waiting 的具体尺寸原因仍待日志确认。四份构建和五组 CPU 检查通过，本轮没有启动游戏。

[本次详细报告](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/docs/AUDIT-FIXES-2026-10-14.md)；[部署核验](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/diagnostics/2026-10-14/audit-fixes/all-games-deployment.json)。此节为最新状态，下方历史记录保留。

# 当前部署：2026-10-14 曝光输入开关版

16 个 DX12 登记目标全部更新并再次核验。主体 `F7F7CD1FB8A52E9F99BC9244D3B258D7B8616CE30BA98EFBBCB8024F8AB70E52`；代理 `11076F50635B213BD3C04DBC5F07B24D4D43622A4A3F9CB1F71059A4F8CFC3CD`。既有 INI、模型和原生 DLSS / REF 文件按部署前 hash 保留。新增 pre-exposure / exposure scale 独立开关，关闭代入 1，保持原公式和纹理优先级。四次构建与 CPU 组合验证通过，本轮没有启动游戏。

[本次更新报告](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/docs/EXPOSURE-INPUTS-2026-10-14.md)；[部署核验](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/diagnostics/2026-10-14/exposure-inputs/all-games-deployment.json)。此节为最新状态；下方历史记录保留。

# 当前部署：2026-10-13 最终清理版

16 个 DX12 登记目标全部更新为 dist/Release 并核验 SHA256。主体 `169CAE370EEA5E1A87E8C65957A945FB665F7FEDBB9D17A24839CAE5C877D101`；代理 `11076F50635B213BD3C04DBC5F07B24D4D43622A4A3F9CB1F71059A4F8CFC3CD`。已有 INI、模型和原生 DLSS / REF 文件按部署前 hash 保留。本次没有启动游戏。全量清单及保护文件：[部署核验](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/diagnostics/2026-10-13/cleanup/all-games-deployment.json)；[最终详细报告](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/docs/REPAIR-REPORT-2026-10-12-13.md)。

本节覆盖下方旧记录中的“当前”“暂不批量更新”“待清理”。下方作为历史记录保留。旧 build 临时证据已归档到 DLSSNR_LiteDX12/diagnostics/2026-10-13/cleanup 对应 ZIP。

# 部署列表（DLSSNR_LiteDX12）

一份表：哪些游戏装了它、装的是哪一版、怎么装。改这张表的时候**同时改**
`tools/deploy.ps1` 里的 `$games`（脚本用那份表，文档只是给人看的）。

## 怎么部署

```powershell
.\tools\deploy.ps1                      # 不带参数 = 只列状态，不写任何文件
.\tools\deploy.ps1 -Target jedi          # 用 dist\Release 部署到列表里的某个游戏
.\tools\deploy.ps1 -Target jedi -Build   # 先构建再部署
.\tools\deploy.ps1 -Path D:\Some\Folder  # 临时目标（还没进列表）
.\tools\deploy.ps1 -Target jedi -OverwriteModel  # 连 nvngx_dlssnr.dll 一起换掉
```

**没有 `-List`，也没有 `-Target all`**（故意的）。`-Target all` 是“一次手滑改一整台机器”，已删除；
每次部署必须点名一个目标，所以不带参数的那次运行永远是只读的。

规则（脚本会照这个执行）：

- 只写两个 DLL：`dxgi.dll`、`nvngx.dll_litedx12.dll`——**无条件覆盖，不备份**、也没有暂存层（用户 2026-10-11
  拍定：“先备份再覆盖”和 `.deploy.tmp` 两套都删了，脚本现在就是一句 `Copy-Item -Force`）。所以这两个的覆盖是
  **不可逆的**，唯一要小心的是下面那条代理冲突；
- `DLSSNR_LiteDX12.ini` 和 `nvngx_dlssnr.dll`（165 MB 模型）**所在目录里已经有就原样不动**，只有没有才拷进去。
  理由：设置是你的，模型是个 165 MB 的大块头，都不该被一次部署抹掉。要强制换模型加 `-OverwriteModel`；
  要重置设置就自己把那份 INI 删掉再部署（INI 没有对应的开关，用不着）；
- **游戏自己的文件一律不碰**（`nvngx_dlss*.dll`、`sl.*.dll`、游戏自己的 INI、ReShade/RenoDX 的东西都不动）；
- 一个目录里**只能有一个 `dxgi.dll` 代理**。如果那里已经有别的 dxgi 代理（ReShade、OptiScaler、或隔壁 `DLSSNR_Lite`），部署会把**它的** `dxgi.dll` 顶掉——**而且是永久顶掉**（不再备份），那个 mod 在这个游戏里就不再生效了，要恢复得自己重新装一遍。往这种目录部署之前先看一眼。

## 列表

**本项目（DLSSNR_LiteDX12）只做 Direct3D 12**，所以这张表里**只放 DX12 游戏**。加条目之前先确认一下；
DX11 游戏装上去也没用（本项目只钩 DXGI 工厂 / D3D12 命令列表 / NGX 的 D3D12 入口），DX11 之后另开工程。
怎么确认见下一节。

| 名字 | 目录 | 现在装的是 |
| --- | --- | --- |
| `jedi` | `G:\EAGames\Jedi Survivor\SwGame\Binaries\Win64` | `12DCF9E64648` = 最新监测清理版（2026-10-12 01:58 部署）；用户清空目录后重新安装，配置和模型缺失，已补齐；四个文件 SHA256 均与源文件一致，未启动游戏 |
| `gta` | `G:\SteamLibrary\steamapps\common\Grand Theft Auto V Enhanced` | `12DCF9E64648` = 最新监测清理版（2026-10-12 02:54 部署）；两个插件 DLL SHA256 与源文件一致，原有配置和 NR 模型哈希保持不变，未启动游戏。记录：`build/deployment-gta-20261012-diagnostics-cleanup.json` |
| `wolong` | `G:\XboxGames\Wo Long- Fallen Dynasty\Content` | `12DCF9E64648` = 最新监测清理版（2026-10-12 02:21 部署）；补齐配置，保留原有 NR 模型；插件 DLL/配置 SHA256 与源文件一致，模型哈希保持不变，未启动游戏。记录：`build/deployment-wolong-20261012-diagnostics-cleanup.json` |
| `mhwilds` | `F:\SteamLibrary\steamapps\common\MonsterHunterWilds` | `12DCF9E64648` = 最新监测清理版（2026-10-12 02:24 部署）；补齐配置，保留原有 NR 模型和其他模组文件；插件 DLL/配置 SHA256 与源文件一致，模型哈希保持不变，未启动游戏。记录：`build/deployment-mhwilds-20261012-diagnostics-cleanup.json` |
| `ronin` | `G:\SteamLibrary\steamapps\common\Ronin` | `12DCF9E64648` = 最新监测清理版（2026-10-12 02:25 部署）；补齐插件、配置和 NR 模型；四个文件 SHA256 与源文件一致，未启动游戏。记录：`build/deployment-ronin-20261012-diagnostics-cleanup.json` |

DX11 游戏（例如 `H:\Games\RiseOfEros`）不进这张表。

### api 是怎么确认的（别只写“我记得”）

按可靠度排序，新条目必须至少过一条：

1. **游戏运行时的模块**（最硬，需要跑一次）：`Get-Process <游戏> | % { $_.Modules } | ? ModuleName -match 'd3d1'` → 出现 `d3d12.dll` 就是 DX12。
2. **我们自己的日志**（免费，而且是运行时的真相）：`DLSSNR_LiteDX12.log` 里出现
   `Game DLSS/DLAA observed`（由 `panel_ui.cpp` 根据 `GameDlssSnapshot()` 打印）就说明**游戏调了
   `NVSDK_NGX_D3D12_*`** —— `ngx_observer.cpp` 只钩这三个 D3D12 名字，DX11 游戏调的是
   `NVSDK_NGX_D3D11_*`，我们根本看不见，也就永远不会出现这行。
3. **exe 里的字符串**（快，但**两边都有时无效**）：
   `findstr /i /m /c:"d3d12.dll" /c:"D3D12CreateDevice" <exe>`。
4. `dumpbin /imports`（只看静态导入，且 dumpbin 不在 PATH 上，在 VS 安装目录里）。

各条目的证据：

| 名字 | 结论 | 依据 |
| --- | --- | --- |
| `gta` | dx12 | exe 里**只有** D3D12 标记（`d3d12.dll`/`d3d12core.dll`/`D3D12CreateDevice`），**没有任何** D3D11 标记；目录里还有 `sl.*.dll`（Streamline 仅 DX12/Vulkan）与 `amd_fidelityfx_dx12.dll` |
| `jedi` | dx12 | exe 里 D3D12 与 D3D11 标记**都存在**（Frostbite 两个后端都链了）→ 字符串判定不了；靠第 2 条：2026-10-11 22:09 的日志里有 `Game DLSS/DLAA observed`，即游戏确实走了 D3D12 NGX 入口 |
| `wolong` | 用户指定 DX12 | 2026-10-12 用户明确要求作为 DX12 游戏加入并部署；本次读取 Xbox 版 `WoLong.exe` 被系统拒绝，未调整权限，未启动游戏，实际 NGX D3D12 接入待用户运行验证 |
| `mhwilds` | 用户指定及配置 DX12 | 用户指定 DX12；现有 `config.ini` 为 `Capability=DirectX12`，含 `[RenderDeviceDirectX12]`。EXE 同时含 D3D11/D3D12 标记，不能仅靠字符串确定运行后端；未启动游戏，实际 NGX D3D12 接入待用户运行验证 |
| `ronin` | 用户指定 DX12 | 用户明确要求作为 DX12 游戏加入并部署；EXE 同时含 `D3D11CreateDevice`/`D3D12CreateDevice`，不能仅靠字符串确定运行后端；未启动游戏，实际 NGX D3D12 接入待用户运行验证 |

还没有进列表、但可作候选（隔壁 `DLSSNR_Lite` 曾用过的目录，我们没部署过）：
`H:\Games\RiseOfEros`（无 NGX 调用，本项目无用，见记忆）。

## 要跑 NR 还需要什么（三个文件不够）

NR 运行时找两个 NVIDIA 文件，缺一个就跑不起来：

| 文件 | 从哪来 | 要不要部署 |
| --- | --- | --- |
| `_nvngx.dll`（NGX 核心，~1.4 MB） | 注册表 `HKLM\SYSTEM\CurrentControlSet\Services\nvlddmkm\NGXCore` → `NGXPath` 指向的 DriverStore 目录；也查游戏目录、`System32` | **不用**。日志已证明能自动找到（`NGX core: ...\DriverStore\...\_nvngx.dll (version 32.0.16.1742)`） |
| `nvngx_dlssnr.dll`（模型/snippet，**165,840,496 字节**） | 必须在**游戏目录**（`model_runtime.cpp` 只在自己的目录找它：“an explicit plugin dependency”） | **要**。本机现有副本：GTA 目录与 `H:\Games\RiseOfEros`，哈希都是 `5B9443990497` |

所以：只装三个文件 = 只能测面板/鼠标；要 NR 就必须把模型拷进游戏目录。脚本现在会**顺带拷它**：
源默认 `H:\AgentWorkPlace\DLSS5\nvngx_dlssnr.dll`（310.8，165,840,496 字节，`5B9443990497`），
可用 `-Model <路径>` 换源。规则：目标目录**没有**才拷；已有就保持不动（除非 `-OverwriteModel`，那就直接覆盖，同样不备份）——INI 同理；
**绝不覆盖游戏自己的 `nvngx_dlss.dll`/`nvngx.dll`**。列表（不带参数跑脚本）会报每个目录是 `model ok` 还是 `NO MODEL`。

另外 NR 开关是 INI 里的 `[NeuralRendering] Enabled`（默认 0）。想在游戏里开：面板里勾上
（会写回生效的那份 INI），或者让我改文件。

## INI 可能被重定向（先确认“生效的是哪份”）

`config_storage` 的规则：写游戏目录失败时会回落到用户 profile，并从此以 profile 为准：

```
%LOCALAPPDATA%\DLSSNR_LiteDX12\Profiles\<exe名>-<哈希>\DLSSNR_LiteDX12.ini
```

2026-10-11 观察到：**GTA 已有 profile**（`GTA5_Enhanced-851792630aab9068`，17:46 写的，2324 字节）
—— 所以在 GTA 里，游戏目录那份 `DLSSNR_LiteDX12.ini` **不被读取**；Jedi 当时还没有 profile，
用游戏目录那份。改设置前先看这个目录。

## 部署记录

| 日期 | 目标 | dxgi.dll | nvngx.dll_litedx12.dll | 备注 |
| --- | --- | --- | --- | --- |
| 2026-10-11 | `gta` | `6768D69581DF` | `8D1591A64AC6` | 目录里**实际**的版本（17:43 由用户自己拷入，是 P1-2 之后的中间构建；17:46 跑过一次，生成上面那份 profile）。更早那份 `A46E176B9119`/`903A9499BD51` 已被覆盖 |
| 2026-10-11 | `jedi` | `105324CFFD7F` | `E6CFBB9CF231` | 首个部署（21:52）。22:09 已跑过：插件加载正常、游戏自己的 DLSS 调用被抓到（`Game DLSS/DLAA observed`）、NR 因缺 `nvngx_dlssnr.dll` 未跑（`0x8007007E`）。**用户实测：鼠标/面板正常，没有被锁** |
| 2026-10-11 22:17 | `jedi` | `105324CFFD7F` | `E6CFBB9CF231` | 重部署并把 NR 模型拷了进去（`nvngx_dlssnr.dll` `5B9443990497`）。当时脚本还把三件套备份了；比对过那份 INI 与我们的完全一致→没丢任何设置 |
| 2026-10-11 22:19 | `jedi` | — | — | **事故**：我把 `-Target all` 接进了 `Select-Object -First 6`，PowerShell 在读到第 6 行后**杀掉了子进程**，那次 `Copy-Item` 只完成了“建文件、分配长度”，结果 `DLSSNR_LiteDX12.ini` 变成 **1967 字节全零**（GTA 因为排在后面，完全没被碰到）。当时加的防护是 `<目标>.deploy.tmp` 暂存，后来又删了（见 22:45 那行）。**教训：永远不要把有副作用的命令接进 `Select-Object -First`**（它会杀掉上游进程）。之后干脆把 `-Target all` 整个删掉了 |
| 2026-10-11 22:20 | `jedi` | `105324CFFD7F` | `E6CFBB9CF231` | 修复重部署；四个文件哈希均对：`dxgi.dll` `105324CFFD7F`、运行时 `E6CFBB9CF231`、模型 `5B9443990497`、INI `1C06715B7C08`（可正常读取） |
| 2026-10-11 22:28 | `gta` | `105324CFFD7F` | `E6CFBB9CF231` | 刷新到当前构建（顶掉 17:43 那份中间构建 `6768D69581DF`/`8D1591A64AC6`）。当时的备份里 INI 与本次要写的**完全一致**（说明没丢设置）；模型已有 → 保持不动 |
| 2026-10-11 22:45 | — | — | — | **部署脚本精简到底**：`deployment-backups\`（12 个文件 / 4.8 MB，其中 11 个是当前构建的副本）整个删除，同名文件直接覆盖；随后 `.deploy.tmp` 暂存层也按用户要求删掉（多余）。脚本现在只有两条 `Copy-Item -Force` + 打印哈希。理由：那份拷贝要么是垃圾（同内容），要么是不可重建的旧二进制——而用户不需要它 |
| 2026-10-11 22:5x | — | — | — | **INI 改成“有就不动”**（用户：“ini 和模型都是有就不动”）：两个 DLL 依旧无条件覆盖，`DLSSNR_LiteDX12.ini` 只在目标目录缺它时才拷——它装的是设置，现在没了备份，一次重部署就能把你在面板里调的东西抹平。列表里多一列 `ini ok`/`NO INI` |
| 2026-10-12 | `jedi` | `95A21662E57F` | `7E0B4F25DE3D` | 用户授权部署 Opt DX12 对照修复构建。两个 DLL 的 SHA256 与 dist/Release 一致；现有 INI 与模型部署前后 SHA256 不变。未运行游戏。校验记录：`build/deployment-jedi-20261012.json` |

## 2026-10-12：新增部署目标

| 目标 | 游戏目录 | 本次操作 |
| --- | --- | --- |
| wdlegion | G:\UBISoft\Watch Dogs Legion\bin | 部署 |
| fh6 | G:\XboxGames\Forza Horizon 6\Content | 部署 |
| kcd2 | G:\XboxGames\Kingdom Come- Deliverance II\Content | 部署 |
| starfield | G:\XboxGames\Starfield\Content | 部署 |
| alanwake2 | G:\EpicGames\AlanWake2 | 部署 |
| uncharted | G:\SteamLibrary\steamapps\common\Uncharted Legacy of Thieves Collection | 部署 |
| dd2 | F:\SteamLibrary\steamapps\common\Dragons Dogma 2 | 仅登记，文件不变 |
| tsushima | F:\SteamLibrary\steamapps\common\Ghost of Tsushima DIRECTOR'S CUT | 部署 |
| nioh3 | F:\SteamLibrary\steamapps\common\Nioh3 | 部署 |
| dsdc | F:\XboxGames\DSDC\Content | 后续新增并部署 |

八个部署目标的主体 SHA256 均为 `F3FB5E3D90D2758BB06982BC22F8BE3B11F79A01D4D0DBF3F6AAD4A657597F51`，代理、配置、模型也与对应发布来源逐项核对一致。缺少的 INI 与 NR 模型已补齐；龙之信条 2 的插件 DLL、INI 及模型有无状态均与操作前一致。未启动游戏。

部署前与核验记录：`build/deployment-extra-games-20261012-before.json`、`build/deployment-extra-games-20261012-verified.json`，同时含本轮两个 DX11 目标。

随后按用户要求新增 `dsdc`，部署相同 DX12 Release，补齐缺少的配置和模型。两个插件 DLL、INI、模型的 SHA256 均与发布来源一致；游戏自身 `nvngx_dlss.dll` 未覆盖，未启动游戏。

## 2026-10-13：Space Marine 2 部署与死亡搁浅启动隔离

- 新增 `sm2 = F:\XboxGames\Warhammer 40,000- Space Marine 2\Content\client_pc\root\bin\pc`，部署当前 DX12 Release。两个 DLL 与新建 INI 的 SHA256 均与 dist/Release 一致；已有 NR 模型保留，SHA256 为 `E16BCF15E16E13F527491CDF7845B2FE6521A738D8F7C9C721866A8496E1FC8E`。未启动游戏。
- 死亡搁浅用户报告启动直接退出、无提示。23:52:22 的最新日志完成了 hook 安装和 runtime 文件发现，NR 开关为 0，模型尚未初始化；日志不包含异常地址，不能认定具体崩溃函数。Windows 没有对应本次启动的 Application Error/APPCRASH 堆栈；23:13:32 的 RADAR_PRE_LEAK_64 不是本次启动崩溃报告。
- 临时仅修改死亡搁浅 INI 的 `[General] Enabled=0`，隔离全部 capture hooks，保留 DXGI 转发 DLL。原配置保存于 `build/dsdc-before-isolation-20261013.ini`；元数据保存于 `build/dsdc-isolation-20261013.json`。该诊断状态暂时关闭面板和 NR，需要用户重新启动游戏提供结果后继续定位并恢复。其他设置和模型未改，未自动启动游戏。

用户反馈总开关关闭后仍退出，错误代码 0x0。00:11:16 新启动日志确认 `enabled=0`，未安装 hook，未初始化模型；错误码本身没有定位到异常模块。随后核实代理 SHA256 为部署来源，临时将游戏目录 `dxgi.dll` 重命名为 `dxgi.dll.litedx12-disabled`（内容不变，原配置总开关仍为 0），让游戏不再由本地代理启动插件。等待用户再次启动反馈，尚未认定具体故障层或完成修复。

用户确认移开代理后能进入主界面，说明与插件加载相关。第三步已恢复原 `dxgi.dll`，将主体临时重命名为 `nvngx.dll_litedx12.dll.disabled`，General.Enabled 保持 0。此时代理应只转发系统 DXGI，Boot 预期记录主体缺失错误；等待用户启动结果，区分代理与主体装载。文件内容与发布哈希均未修改。

## 面板/鼠标测试（不依赖 NR）

三个文件就够：面板键 **Insert**（写死的 `VK_INSERT`，没有 INI 键），日志写游戏目录
`DLSSNR_LiteDX12.log`。`[NeuralRendering] Enabled=0` 时 NR 不参与，不影响面板。

## 每个目标要一起记的信息

- 用什么启动不再影响兼容分支；`DirectResolve` 等开关完全由 INI 决定（小于 100 px 的交换链已改为一律不包装，不再有开关）。
- 鼠标/面板是不是 raw-only（日志第一行会写）；
- 有没有别的注入器（RTSS、Steam Overlay、ReShade）——影响 `Present` 路径和光标归属；
- 备份目录名，方便回滚。

用户确认第三步（原代理存在、主体移开）能正常进入菜单，证明当前代理纯转发路径可用，故障进一步限于主体装载/初始化。第四步部署 build-only `dsdc-loadonly-dxgi.dll`（SHA256 `B6FEE8C7B28F3BCBD89B83C19245A9AA1B040B9063A6912BF2C0203F3FA6ABCE`），恢复原主体。诊断代理仅 LoadLibrary 主体并查找 Start，不调用 Start，也不发布 runtimeModule，因此不调用 LiteAttachFactory；成功装载后写独立 `DLSSNR_LiteDX12-loadonly.log`。原代理保留为 `dxgi.dll.litedx12-disabled`，General.Enabled 仍为 0。生产源码及 dist 未修改，等待用户实测。

2026-10-13 后续隔离与定向更新：死亡搁浅只加载主体的日志确认装载成功但仍退出，中性改名无效；延迟三个依赖后、只延迟 D3D12 后均可进菜单。正式补上与 Opt Release 一致的 D3D12/compiler delay-load，并补 RR feature 13 与 NGX EvaluateFeature_C 捕获。只更新死亡搁浅、Alan Wake 2（主体 DF7D9B562AC4）；死亡搁浅恢复正式代理与原 INI，等待完整 Start/面板验证。Space Marine 2 关闭总开关后用户回复选项 1，按能进菜单理解；现在恢复原 INI，使用只禁用 On12 hook 的 build-only 诊断主体 F15F2709D6EA，待实测。其他 DX12 游戏未刷新。详情 `docs/STARTUP-CAPTURE-2026-10-13.md`，核验 `build/affected-games-20261013-verified.json`。

2026-10-13 下一轮：用户确认死亡搁浅启动/面板正常，但实际场景 NR 卡在 tracked Reset；Alan Wake 2 识别成功，保持原部署；Space Marine 2 禁用 On12 仍崩溃。正式修正 DX12 交换链必须由 ID3D12CommandQueue 创建的判据，编译及实际 hooks.cpp 的 CPU 边界/引用计数检查通过。Space Marine 2 刷新为正式 1DFDED619D24，恢复正常 On12，含延迟依赖修正；死亡搁浅为 build-only 有限 Reset 诊断主体 7EFEEAFE05CC。原 INI、模型、代理均保持原哈希。未启动游戏，等待本轮反馈。核验 `build/active-game-diagnostics-20261013.json`；临时诊断定位后恢复正式主体。

2026-10-13 当前：死亡搁浅与 Space Marine 2 已更新正式主体 118D56B5…、代理 11076F50…（待游戏复测）。其他游戏未批量更新。详见 STARTUP-CAPTURE-2026-10-13.md 的“对照 ref 后的当前部署与证据修正”。

2026-10-13 最新：Space Marine 2 定位到 RTSS 首次 Present 内 On12 无效队列；正式主体 1917F24EA55E…/代理 11076F50635B… 已部署，日志证实队列替换，用户确认“已经可以玩了”。保留这版，不随最新 dist 批量刷新。死亡搁浅 118D56B5 出现 NR 闪烁与退出卡死后先回退，再定向部署 build-only 有限输入/ledger/Record 诊断主体 B0554B12CFA8… 和代理 11076F50635B…，包含 canonical COM 身份匹配修正；原 INI（NR=0）、模型、原生 SR DLL 哈希未改。等待短暂 NR 场景复测，尚未声明死亡搁浅修复。当前核验 `build/dsdc-ledger-diagnostic-deployed-20261013.json`；完整证据在 STARTUP-CAPTURE-2026-10-13.md 最新一节。此前“当前”条目为历史部署记录。

2026-10-13 12:55 后续：死亡搁浅用户确认 NR 仍闪，Apply NR result 关闭则正常，Model input 正常但 Raw model output 闪。有限日志评估/Signal/fence 正常。仅部署主体 DFBD1E775535…，build-only 把 NR display proxy alpha 固定为 1 的单项对照，其余行为保持 B0554B12；原 INI、代理、SR 和 NR 模型哈希未变。等待用户图像验证，此时未确认修复。核验 `build/dsdc-opaque-proxy-deployed-20261013.json`。战锤等游戏未改。

2026-10-13 最新批量诊断：用户补充 Resources 中 depth/MV 不闪，NR debug 中闪。死亡搁浅主体更换为 `99F1665900F550A7AA1C7BCA63D8FB42EB476FBA3370FFD6ED0C40F80A746B80`，四次自动 GPU 样本与有限关闭列表重复执行检测，沿用当前 alpha 对照，不宣称已修复。地平线 6 主体更换为 `F6EA4D37D61BEE76CAEAB3ED250752F7B7AF278DE1652847A19E1C60A0086701`，有限 factory/交换链/Present/面板接入日志，待实测。两款原代理、配置、SR/NR 模型均保留并复核哈希，其他游戏未改。核验 `build/batch-audits-deployed-20261013.json`。全部问题确认修好后统一移除诊断与临时文件；总报告工作记录见 `docs/REPAIR-REPORT-2026-10-12-13.md`。

2026-10-13 20:00/20:01 实测后最新：用户确认 DS 仍闪、先关 NR 后退出不卡；FH6 Ins 仍无反应。新日志及 ref 实现对照后，FH6 定向更换正式主体 `659A75A7C7D8FE8DBF39FCF765F84B40BE7D329127E714625FC9A11BD0E13CE2`（单键轮询/消息边沿合并、根窗口焦点）；DS 定向更换 build-only `6D90D6B4D84F17682C3BEDDC1E6CF18B69904FC6171874F39C358CE5A5FF1B5F`（ref 同族 depth 复制，有限采样保留）。不宣称 DS 闪烁根因已确定。两个游戏原代理/配置/模型核对不变，其他游戏未改；核验 `build/ref-alignment-deployed-20261013.json`。详细证据/限制及清理范围已进入总报告工作记录。
 
## 2026-10-13：私有 guide 定向诊断／地平线空 PSO 修正

- DSDC `F:\XboxGames\DSDC\Content` 主体更新为 `43D8569AB85B156AD5D504BF07F572A23F39463233FF929A123DB323CA7E5BB1`，仅定向诊断：depth/MV 捕获副本、恢复正式颜色 shader/R32 depth 转换、四帧七扫描线采样；闪烁仍待验证，未推广正式 capture/pass。
- FH6 `G:\XboxGames\Forza Horizon 6\Content` 主体更新为正式版 `42AADAEC048314CBCB24C385C441F8039817DC6063A0629E2250AAF67A3E53CD`：已知 Reset 后的空 PSO 可执行 NR，未知录制状态检查保留；面板已由用户确认可以打开，NR 尚待运行。
- 记录 `build/private-guides-pso-deployed-20261013.json`，备份 `build/before-private-guides-pso-20261013/`。保持两款当前配置、代理和模型；没有启动游戏、没有更换战锤或其他部署。详情见修复工作报告最新章节。

## 2026-10-13 21:28 测试后：DS 原游戏 UAV 直接 resolve

FH6 用户已确认面板/NR 可以，保留正式主体 42AADAEC048314CBCB24C385C441F8039817DC6063A0629E2250AAF67A3E53CD。DS 私有 guide 版本仍闪，未推广；只更新 DSDC 主体 BE13B13E30004AA56DD23FA5B727C2E5414D2CC294EC422AE448EC369AD19652，独立原图读取 + compute 直接写原游戏 UAV，有限扫描线和一次完整图像捕获。该版本仍需实测，不是已验收修复。记录 build/dsdc-direct-resolve-deployed-20261013.json，当前 INI/代理/模型保持，其他游戏未更新。

## 2026-10-13：死亡搁浅通过／龙信 2 更新早期部署

死亡搁浅 BE13B13E 定向版本用户确认闪烁和半幅缺失都消失，正式收敛与诊断清理尚未完成，当前保留已验证版本。龙信 2 从只登记时的早期主体 F3FB5E3D、代理 677976A7 更新为当前正式主体 42AADAEC048314CBCB24C385C441F8039817DC6063A0629E2250AAF67A3E53CD、代理 11076F50635B213BD3C04DBC5F07B24D4D43622A4A3F9CB1F71059A4F8CFC3CD。使用原 tools/deploy.ps1 定向更新，备份与部署核验见 diagnostics/2026-10-13/dd2-before-compatibility/deployment.json。当前配置与原生 SR/RR/FG/NR 模型保持，游戏未启动，龙信 2 待实测；其他游戏未更新。

2026-10-13 22:16 龙信 2：正式版升级及 General.Enabled=0 均仍报 DD2.exe+0x90F550E。补查 Opt 官方 Wiki 的 REFramework 前置要求后，已部署官方 nightly 01432 dinput8.dll（70A2A3C24030D8BC3695554E7F929FD1B0B4B757FB5BA2C9D34F42D7155005AC）与最小 re2_fw_config.txt；REF 菜单 Delete、默认关闭，本插件仍 Ins。已恢复原插件配置 General.Enabled=1、NR.Enabled=0，主体42AADAEC/代理11076F50保持。待用户正常启动/实际场景 NR 验证，不提前称根因修复。包、DLL、revision 另按用户要求复制到 E:\Mod\REMod\REFramework。记录 diagnostics/2026-10-13/dd2-reframework/deployment.json。
