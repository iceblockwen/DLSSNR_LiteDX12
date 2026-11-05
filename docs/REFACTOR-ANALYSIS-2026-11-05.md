# DLSSNR_LiteDX12 —— 重构前全量代码分析（2026-11-05）

目标：简化代码、提高可读性、热路径性能优化。本文是**逐模块静态阅读**的结论，未改动任何代码，未做实机计时。
流程图与逐单元顺序见 [ARCH-FLOW-2026-11-05.md](ARCH-FLOW-2026-11-05.md)；本文填补其第 6 节"待量化的热点"的定性部分。

行号以本文日期的代码为准。

## 0. 总结论

1. **最热的路径已经做过优化**：`command_guard` 的 `Track`（每个 draw 状态设置都会经过）在 NR 关闭时只做 1 次原子读 + 1 次 TLS 读；NR 开启、命中线程缓存时也只有几次比较。这里没有大块收益可挖。
2. **GPU 侧最大收益**：输入复制与 pass 内的 barrier 逐个提交，应批量化。
3. **CPU 侧最大收益在输入钩子，不在 NR**：面板一创建就会钩上 `GetAsyncKeyState` / `GetKeyState` / `GetKeyboardState` / `GetCursorPos`，面板不打开时游戏每次调用也要拿一把全局 mutex。
4. **代理与启动层有冗余**：dxgi 代理的工厂包装与主体的 detour 重复，还留下一个启动竞态窗口。
5. **日志系统基本不工作**：只有启动的几行真正写进日志，`GraphicsFailure` 是空函数，`ReportRejectedResource` 只格式化不输出。
6. 排版：大部分文件不符合 `AGENTS.md` 的 Allman 规则，需要单独提交。

## 1. L0 代理（`src/proxy/`）

### 1.1 文件职责

| 文件 | 作用 |
| --- | --- |
| `dxgi.def` / `version.def` / `winmm.def` | 导出表 `名字=ProxyN @序号`，CMake 据此生成跳转桩和名字表 |
| CMake `litedx12_proxy_stubs` | 生成 `ProxyN`：表里已有地址就 `jmp`；没有就进 `SlowN`，保存 rcx/rdx/r8/r9/xmm0-3，调用解析函数再跳转。栈对齐和 unwind 信息已核对，无误 |
| `proxy.cpp` | dxgi 入口：`ResolveDxgi` 加载 System32 dxgi；前 3 个槽换成 `CreateFactory*` 包装；加载失败时按签名返回兜底函数 |
| `proxy_forward.cpp` | version / winmm 入口：优先 `<name>-original.dll`，其次 System32 |
| `proxy_common.hpp` | `BootRuntime` 加载主体并调用 `Start`；`BootFailure` 写失败日志；`LoadOriginal` |
| `factory_origin.hpp` | `NativeFactoryCaller`：按返回地址判断调用者是否为 d3d11/d3d12/dxgi/RTSS |

### 1.2 发现

- **工厂包装多余**：主体 `InstallCapture`（`src/hooks.cpp:324-326`）已经 detour 了 System32 dxgi 的 `CreateDXGIFactory*`，判断调用者用的也是 `_ReturnAddress()`。现在的实际链路是：
  游戏 → 代理 `CreateFactory` → 系统函数（已被 detour）→ 钩子看到调用者是我们自己的 `dxgi.dll`，按"系统模块"跳过 → 回到代理，由 `WrapFactory` 包装。
- **启动竞态**：从 detour 提交到 `runtimeModule.store` 之间有一个窗口。这期间钩子跳过（调用者是我们的代理），代理也跳过（`runtimeModule` 为空），这个工厂就漏掉了。前 3 个槽改成纯 `jmp` 后，钩子看到的返回地址就是游戏本身，窗口随之消失。
- 只写不读的状态：`systemModule`、代理侧的 `startupError`；`runtimeModule` 只给 `WrapFactory` 用。
- 两个入口的 `Entry` 常量完全相同。
- "`GetModuleFileNameW` + 截到最后一个 `\`"重复了 3 次。
- dxgi 目标链接了 `user32`，但没用到。

### 1.3 建议

1. 删除 `CreateFactory*` / `WrapFactory` / `FactoryUnavailable` / `originalFactories` / `_ReturnAddress` / `dxgi1_6.h`，以及主体的 `LiteAttachFactory` 导出。
2. 删掉无用状态，去掉 `Entry` 结构体，抽出 `ModuleFolder()`，把 `factory_origin.hpp` 挪到 `src/`，去掉 `user32` 链接。
3. 可选：合并两个入口源文件。dxgi 的兜底表要保留签名区分，因为 `CompatString` 返回 BOOL，统一返回 `E_NOTIMPL` 会被当成 TRUE。
4. 可选（收益很小）：转发表初始值指向 `SlowN`，快路径只剩一条 `jmp [tbl+n*8]`；所有 `SlowN` 共用一段慢路径。
5. 不建议：用 PE 导出转发替代桩（会失去 `-original.dll` 链式加载）；加 `DisableThreadLibraryCalls`（与静态 CRT 冲突）。

预计 proxy 部分从约 570 行减到约 250 行，对外导出不变。

**状态（2026-11-05）**：建议 1、2 已完成，可选项 3–5 不做。dxgi 工厂改由主体的 `FactoryExport` detour 统一包装；`LiteAttachFactory` 导出已删除。

## 2. L1 启动（`bootstrap` / `runtime` / `config_*`）

**状态（2026-11-05）**：文件已合并为 `runtime.*` + `config.*`（纯搬迁）；下列逻辑建议尚未实施。

### 2.1 职责

| 函数 | 作用 |
| --- | --- |
| `Start` | 加载 System32 dxgi（只为取路径）→ `Initialize` → `InstallCapture` → `StartConfigWriter` → `DiscoverModelRuntime` |
| `Initialize` | 选定配置文件、确定日志路径、读取并缓存 INI 开关、写启动头 |
| `Fail` | 设 `usable=false` 并记一行日志 |
| `Snapshot` | 加锁拷贝约 4KB 的 `Status`，实际只有 `pluginPath` 被使用 |
| `LogQueue` | deque + 条件变量 + 后台线程 |
| `config_storage` | 决定配置文件位置（环境变量 → `ConfigDirectory` → LocalAppData profile），写失败时回退 |
| `ConfigWriter` | 后台去抖写回：150ms，最多 1s，重试 3 次 |

### 2.2 发现

- 真正写日志的只有 `Initialize` 与 `Fail`。`GraphicsFailure`（`src/graphics.cpp:82`）是空函数；`ReportRejectedResource`（`src/nr/capture12.cpp:125`）只格式化、加锁、去重，不输出；`Diagnostics.Level=2` 没有效果。
- 重复状态：`Status.enabled` 与原子 `enabled` 是同一个值；`logging` 就是 `detail>=1`。
- System32 dxgi 被加载两次：`Start` 一次，`InstallCapture` 又一次。
- `Fail` 的日志文字固定为 "System DXGI forwarding failed"，与实际原因不符；HRESULT 按 `%lu` 输出成十进制。
- `ConfigWriter` 只有一个实例且永不析构：`std::function`、空的错误回调、析构与 `stopping`/`join` 逻辑都是死代码。
- 启动时有两块大分配：64KB（`ConfigDirectory`）和 256KB（`ReadRedirect`），只发生一次。

### 2.3 建议

- 用初始化时写一次的 `PluginFolder()` 替代 `Snapshot`。
- 只在启动时读的开关（`Usable`/`Enabled`/`SkipSmallSwapchains`/`DirectResolve`/`RequireComputeSignature`）放进头文件做 inline 读取；`ForceTiming` 会被面板修改，保留原子变量。
- `InstallCapture(system)` 接收 `Start` 已加载的句柄；`StartupError` 改为文件内私有。
- `ConfigWriter` 直接调用 `WriteConfigValue`。
- **待决定**：日志是恢复失败日志，还是精简为只写启动日志。决定后 `GraphicsFailure` / `ReportRejectedResource` / `LogQueue` 跟着处理。

## 3. L2 接线（`hooks.cpp` / `wrappers.hpp` / `detour_hook.hpp`）

### 3.1 热路径

| 路径 | 频率 | 每次做什么 | 结论 |
| --- | --- | --- | --- |
| `Swapchain::Present` → `DrawSwapchain` → `Panel::Draw` | 每帧 | 递归锁 + map 查找；`PrepareModel12` 每帧调 `Runtime().Initialize`，又加一次锁 | 约 100ns，写法可以更干净 |
| `ForwardPresent` | 每帧 | RTSS 探测已缓存；`__try/__finally` 开销可忽略 | 可以 |
| `ExecuteCommandLists` 钩子 | 每次提交 | 没有待提交帧时只读 1 次原子；**有待提交帧时，每个 list 做 2 次 `QueryInterface`+`Release`，还要拿递归锁查 map** | 值得优化 |
| list `Reset` | 每次 Reset | **slot 10 被 detour 了两层**：`hooks.cpp:171` 的 `ResetList` 和 `command_guard.cpp:204` 的 `Reset` | 合并 |
| `GetProcAddress` → `ResolveNgx` | 进程里每次查找 | 按序号直接返回；按名字最多 4 次 `strcmp` | 可以先比一次 `"NVSDK_NGX_D3D12_"` 前缀 |

### 3.2 死代码与多余写法

| 位置 | 说明 |
| --- | --- |
| `hooks.cpp:69`、`:208` | `static std::atomic<bool> reported{}` 没人用 |
| `wrappers.hpp:59-62,75-78,89` | `HRESULT hr;{...}return hr;`、`forwarded=p` 等多余中转变量 |
| `wrappers.hpp:20` | `WrapSwapchain` 模板只是包了一层 `reinterpret_cast` |

## 4. L3 观察（`nr/ngx_observer.cpp`）

| 位置 | 说明 |
| --- | --- |
| `:91-94`、`:128-131` | 读一个只写一次的函数地址也要加锁，可以改成原子变量 |
| `:116,151,192` | map key `{handle, 12u}` 的第二项恒为 12，直接用 `void*` |
| `:188` | `released` 只赋值不读 |
| Evaluate 整体 | 每帧 1–2 次，约 20 次按字符串查参数，与 GPU 上的 NR pass 相比很小，主要是可读性问题 |

## 5. L4 管线

### 5.1 `nr/command_guard.cpp`

- `Track`：NR 关闭时约 2 次读就返回；NR 开启、命中缓存时只有 hash 和 5 次比较。**已经很省**。
- `CommandStateScope`（`:394`）每次 evaluate 都 `make_unique` 并深拷贝 `State`：两个 `Root`，每个参数自带 64 个 word 的数组，最坏约 67KB，还要逐个 AddRef。
  NR 录制期间 `suspended` 会挡住跟踪，游戏这个 list 的影子状态不会变，所以离开 scope 时直接按原状态恢复即可，存一个指针就够。
- 每次 evaluate 都调一遍 `InstallCommandState`：GetDevice、2 次 QI、2 把锁、多次 `Installed` 线性查找。可以像 `InstallOnce` 那样缓存上次的 vtable 指针。
- 风险（中等置信度）：`Obtain` 新建节点会递增 bucket generation，同一 bucket 里其他 list 的线程缓存会全部失效。不影响正确性，只是命中率可能偏低。
- `:222-223` 清空 Table 参数的代码与 `Root::Invalidate` 重复。

### 5.2 `nr/capture12.cpp`

- 提交时先比裸指针：`pending` 额外记下 `Capture12` 当时拿到的 list 指针，Execute 时先比指针，比不上再做 `QueryInterface`。
- "清空 job 引用"写了 3 次（`:360-368`、`:375-382`、`Collect`），应抽成 `Captured12::ClearBorrowed()`。
- `ReportRejectedResource` 只格式化不输出（见 2.2）。

### 5.3 `nr/capture12_input.hpp` + `nr/pass.cpp`（GPU 侧）

| # | 位置 | 问题 | 建议 |
| --- | --- | --- | --- |
| G1 | `capture12_input.hpp:58-77` | 每个复制都是"2 个 barrier + copy + 2 个 barrier"，3 个输入就是 12 次单独的 barrier 调用，copy 之间被迫串行 | 合并成"全部前置 barrier → 3 个 copy → 全部后置 barrier" |
| G2 | `pass.cpp` 多处 `Transition` | 相邻的 transition 逐个提交 | 合并成一次 `ResourceBarrier(n, …)` |
| G3 | `pass.cpp:657-661` | 描述符堆只复用"本帧刚回收的那一个"，某帧没有录制完成时就新建 shader-visible 堆 | 改成固定数量的堆池，按 fence 复用 |
| G4 | `preview.cpp:42`、`white_point.cpp:80` | 预览的每个 tile、白点计各自有描述符堆，一帧里切换 4–5 次堆 | 共用录制里的那个 24 槽堆 |
| G5 | `white_point.cpp:12-21` | 每次测量都分配 vector 并对 4096 个值完整排序 | 用 `nth_element` 求 3 个分位数，缓冲区复用 |
| G6 | `pass.cpp:664,750,781` | 每个描述符都重新调 `GetDescriptorHandleIncrementSize` | 构造时缓存 |
| G7 | `model_runtime.cpp:128-130` | 每帧约 30 次 `Set*`，每次单独加一次递归锁 | 整组参数只加一次锁 |

### 5.4 `nr/pass.cpp` 可读性

| 位置 | 说明 |
| --- | --- |
| `pass.hpp:8-19` `FeatureBackend` | 虚接口只有 `ModelRuntime` 一个实现，直接用 `ModelRuntime&` |
| `:19` `Resource()` | 只是转发一次 |
| `:238,490,576,617,953` | "Verbose only…"注释描述的日志代码已删除，注释不再成立 |
| `:181` | 排版错位，`{` 后面直接跟了代码 |
| `:484` | 第一次 `white.Resolve(…, 0)` 的返回值被丢弃，第 537 行又调一次。唯一作用是模式切换时先 Reset，**需确认后再合并** |
| `:744-754` 与 `:776-795` | 设置堆、根签名、PSO、描述符表、常量再 Dispatch 写了两遍，应复用 `run` |
| `preview.cpp` / `pass.cpp` / `white_point.cpp` | 根签名和 PSO 的创建重复 3 次，可抽 `ComputePipeline(device, ranges, constants, bytecode)` |
| `preview.cpp:43` | 每个 tile 都拷贝整份 `PassFrame`，带多次 AddRef |
| `model_settings.cpp:59` | `ReportModelWait` 是空函数，有 2 处调用 |

## 6. L5 表现（`panel*` / `input*`）

### 6.1 输入钩子：游戏线程上的开销

钩子在面板创建时就装上（`panel.cpp:352` → `InstallPanelInput`），与面板是否可见无关。

| # | 位置 | 面板关闭时游戏每次调用的开销 | 建议 |
| --- | --- | --- | --- |
| I1 | `input_keyboard.cpp:69-78`，被 `OnAsync`/`OnKey` 调用 | `GetAsyncKeyState`/`GetKeyState` 每次都拿全局 mutex 查 `pollRelease` | 维护原子计数"当前是否还有需要拦截的按键"，为 0 时直接返回，不拿锁 |
| I2 | `input.cpp:229-235` `OnKeyboard` → `PanelKeyboardPollMask` | `GetKeyboardState` 每次都拿锁、构造 256 项数组、再循环 256 次 | 同上，无需拦截时直接返回 |
| I3 | `input.cpp:217-225` `OnGetPos`/`OnSetPos` | 每次加锁写 `askedCursor`；打开面板时 `:313` 会重新取值，这些写入没有用 | 只在拦截期间写 |
| I4 | `input_directinput.cpp:72-93` `OnState` | 递归锁 + map 查找，键盘设备还要循环 256 次 | 同 I1 的短路 |

另一个风险（中等置信度）：`Panel::Impl::WindowProc` 处理每条窗口消息都要拿 `Graphics().mutex`，而 Present 线程画面板时全程持有这把锁。面板可见时，游戏的消息循环可能被阻塞。

### 6.2 其它

- 面板隐藏时 `Panel::Draw` 只有 `PollMenuKey`（几个 user32 调用）和两次早退检查，开销可忽略。
- `input.cpp:257` `FinishInputTransaction` 是空函数，可以直接复用 `detour::Transaction`。

## 7. 排版

`AGENTS.md` 要求 Allman 大括号、所有 `if`/`for` 带括号并换行。基本不符合的文件：

- 压缩成一行的写法：`hooks.cpp`、`wrappers.hpp`、`nr/command_guard.cpp`、`detour_hook.hpp`、`bootstrap.cpp`、`config_*.cpp`、`nr/white_point.cpp`、`nr/model_settings.cpp`、`nr/model_runtime.cpp`、`nr/preview.cpp`、`nr/gpu_util.cpp`、`input*.cpp`。
- K&R 风格且有大量不带括号的 `if`：`nr/capture12.cpp`、`nr/ngx_observer.cpp`、`nr/pass.cpp`、`graphics.cpp`、`panel*.cpp`、`nr/status.cpp`。

按规则，排版单独提交，并保证逻辑逐字节不变。

**状态（2026-11-05）**：已完成。`src/` 下 55 个 `.cpp` / `.hpp` 统一用仓库根的 `.clang-format`（Allman + 4 空格 + `InsertBraces` + `ColumnLimit: 0`，不排序 include），逻辑无改动；构建 Release / Debug 均通过。

## 8. 路线

| 阶段 | 内容 | 风险 |
| --- | --- | --- |
| A. 删死代码 | proxy 工厂包装、`LiteAttachFactory`、各处未使用的变量和空函数、失效注释、`FeatureBackend` | 低 |
| D. 抽公共代码 | `ComputePipeline`、`ModuleFolder`、`ClearBorrowed`、合并 `run` | 低 |
| B. CPU 热路径 | I1–I4、合并 Reset detour、提交时先比裸指针、`CommandStateScope` 去拷贝、`InstallCommandState` 缓存 | 中 |
| C. GPU 热路径 | G1–G7 | 中，需实机验证画面 |
| E. 排版 | 全工程统一成 Allman，单独提交 | 低，改动量大 |

建议顺序：A → D → B → C → E。排版放在最后，免得和逻辑改动的 diff 搅在一起。

## 9. 待决定

1. 日志：恢复失败日志（保留异步队列）还是精简为只写启动日志。
2. `pass.cpp:484` 的第一次 `white.Resolve` 能否合并到第 537 行。
3. version / winmm 入口源文件是否合并进 dxgi 入口。

## 10. 第二轮复查（`14a9c8c` 之后）

**A 批已做**（行为不变，−153 行，Release/Debug `/W4` 零警告）：删 `retiredDevices`、`CopyShaderInput12`、`ModelRuntime::Set*`、writer `Slot/Verified`、`renderStage`、`Entry` 重复字段、`firstObservation`、`InputOrigin`、恒 false 的 `converted`、恒 true 的 `submissionDependencies`；四处"清空 frame"合为 `Captured12::ReleaseFrame()`；`PassExecutor12::Impl::mutex` 删（全部调用在账本锁内）；`WindowFocused` 并入 `ForegroundMatches`；`WhiteMeter12::Record` 去掉与 pass 重复的校验；`StartupError` 文件内私有；`DrainParkedFeatures` 无停车时免锁；`Region` 改常量 key 表（去 16 次 `snprintf`）。日志模块按决定保留。

**待做**（行号以本节写成时为准）：

### B1 无关 list 的 Reset / Execute 不拿账本锁

- 现况：NR 开着时几乎每帧都有 pending，`Pending()` 快路径失效。`Discard`（`capture12.cpp:791`，每次游戏 Reset）先 QI 再拿账本锁查 map；`PrepareSubmission` / `Submitted`（`:710` / `:747`，每次 Execute 两次）拿锁后对每个 list 先比裸指针、不中再 QI。`Capture12` 持同一把锁跑完整个录制（含 NGX Evaluate / Create），多线程录制的引擎会在这里排队。
- 方案：pending 上限本就是 4（`:364`），加 `std::atomic<IUnknown*> pendingIdentity[4]` + `std::atomic<ID3D12CommandList*> pendingRaw[4]`，在锁内随 `pending.emplace/erase` 同步写。Reset/Execute 先比裸指针，不中再 QI 比 identity，都不中直接返回，不拿锁。
- 正确性：游戏必须在 Close 与 Execute 之间自己同步，这已给出 happens-before；假阳性只会多拿一次锁。
- 收益：去掉 NR 期间所有线程的 Reset/Execute 锁争用。风险低。

### B2 command_guard 影子状态 ComPtr → 裸指针

- 现况：`State::pipeline`、`heaps[2]`、`predicate` 与 `Root::signature`（`command_guard.cpp:35,74-77`）都是 ComPtr，每次 `SetPipelineState` / `SetDescriptorHeaps` / `SetComputeRootSignature` / `SetGraphicsRootSignature` 都要 AddRef 新对象、Release 旧对象。多线程共用同一个 PSO / 堆时，两次互锁会在同一 cache line 上来回争用。闭合的 list 也会把游戏对象一直留到下次 Reset。
- 方案：改裸指针。恢复只发生在同一次录制内（evaluate 期间的 `CommandStateScope`），D3D12 本来就要求应用让 list 引用的对象活到执行完；Reset 时状态清零，不会用到上一次录制的指针。
- 收益：每个状态设置调用省 2 次互锁。风险：低，靠的是 D3D12 规则而不是假设。

### B3 ExecuteIndirect 不再清零和拷贝 1KB

- 现况：`Indirect::Call`（`:638-640`）每次构造 `IndirectInfo{}`，约 1032 字节清零；`GetPrivateData` 再拷一遍；常量参数还要在栈上清零 `zero[64]`。
- 方案：`SetPrivateData` 只存头部加 `count` 条 change（常见签名 24–56 字节）；读取时按返回的字节数校验；`zero` 改成 `static constexpr`。
- 收益：每次 EI 省约 2KB 内存操作。GPU-driven 引擎每帧上百次时才明显，其余可忽略。

### B4 `PrepareModel12` 移出 Graphics 锁

- 现况：`DrawSwapchain`（`graphics.cpp:44`）持 Graphics 锁调用 `Panel::Draw`，后者调 `RetireCompleted` 和 `PrepareModel12`（`panel.cpp:509-512`）。首次 `Runtime().Initialize` 要做 LoadLibrary core / model、`NVSDK_NGX_D3D12_Init`、`GetCapabilityParameters`，耗时几百毫秒。这期间游戏消息线程进 `WindowProc`（`panel.cpp:221`）会被同一把锁堵住，表现为窗口无响应、输入积压。
- 方案：`DrawSwapchain` 在锁内取出 panel 的 device，解锁后再调 `RetireCompleted` / `PrepareModel12`，然后重新加锁调 `Draw`。顺带消除 Graphics→Ledger 的锁嵌套。
- 收益：消息线程不再陪着 NGX 初始化等待。渲染线程本身仍会卡这一次，见 D9。

### C5 帧尾 barrier 合并

- 现况（非 DirectResolve，本帧已编辑）：compose 之后依次是 output UAV→SRV（`pass.cpp:1123`）、proxy + answer SRV→UAV（`:1149`）、`reads.Restore()` 每个直读输入各一次（`capture12.cpp:688`）、`CopyNrResult12` 前 output SRV→COPY_SOURCE + color→COPY_DEST、拷贝后再各转回（`capture12_input.hpp:156`）。共 4+k 次调用，而且 output 走 UAV→SRV→COPY_SOURCE、color 走 SRV→UAV→COPY_DEST，各多一个中间态。下一帧还要把 output SRV→UAV（`pass.cpp:1115`）。
- 方案：Record 不在内部收尾，而是把这些 barrier 交给 Capture12 合成一批：output UAV→COPY_SOURCE、proxy / answer SRV→UAV、直读输入恢复、color 直接转 COPY_DEST；拷贝后一批：color→原状态、output→UAV（下一帧的 `:1115` 随之消失）。
- 收益：帧尾从 4+k 次 barrier 调用降到 2 次，去掉 2 个中间态。每次 barrier 都可能触发驱动的 pipeline drain 或缓存 flush。
- 风险：中。状态链写错会花屏或 device removed，需要开 D3D12 debug layer 跑一次。

### C6 InputReadStates12 批量

- 现况：`Begin` / `Restore`（`capture12_input.hpp:112-127`）对每个直读输入单独调 `ResourceBarrier`，最多 4 次。
- 方案：改用现成的 `BarrierBatch12`，它本身就只转 subresource 0。
- 收益：开头和结尾各从最多 4 次降到 1 次。风险极低；与 C5 合做时，Restore 会并入 C5 的帧尾批次。

### C7 guide 转换 barrier 批量

- 现况：`pass.cpp:965-1008` 对 depth / MV 各做一遍：twin SRV→UAV → dispatch → twin UAV→SRV，GPU 上两个 dispatch 被串行化；之后 `run(0)` 完再单独转 proxy（`:1044`）。
- 方案：进循环前一批把两个 twin 转 UAV；两个 dispatch 连续发出（写不同资源，没有冒险，可以并行）；`run(0)` 之后一批把两个 twin 和 proxy 转 SRV。
- 收益：模型之前少 2–3 个 GPU 串行点。只有需要格式转换或深度翻转时才有。风险低。

### D8 调参只改强度时不重建 feature（需实测）

- 现况：`SameTuning` 不等（`pass.cpp:698`）就 `layer.reset()`，下一帧重新 CreateFeature，有一次卡顿，并且 `first = true` 丢掉时域历史，画面会闪一下。但 `Tuning()`（`:1061`）每次 evaluate 都会重写全部 8 个参数。
- 疑点：如果模型每帧都读 Intensity / Structure / Tone / Skin，这四个就不用重建；Preset / Style / AutoMask / UICorrection 可能只在创建时生效。参考工程里没有能证实这一点的代码。
- 实测：临时让 `SameTuning` 只比较后四个，构建后在游戏里拖 Intensity 0↔1，看画面是否即时变化。变了就采用，没变就保持现状。

### D9 NGX 初始化移到后台（需实测）

- 现况：开启 NR 后第一次 Present 在渲染线程同步跑完整个 `Initialize`（LoadLibrary、NGX Init、能力查询），游戏会冻住一下。
- 方案：发现 device 后由后台线程调 `Initialize`；渲染线程只读原子 ready 标志，没好就跳过 NR。CreateFeature 必须用游戏的 list，仍留在录制里。
- 疑点：NGX 是否允许在非渲染线程 Init，以及是否会与游戏自己的 NGX 初始化竞争共享 core。需要在游戏里验证。

### D10 WM_INPUT 不走 Graphics 锁（需实测）

- 现况：`WindowProc` 对每条消息都拿 Graphics 锁（`panel.cpp:221`），`WM_INPUT` 还在锁内调 `GetRawInputData`（`PanelRawHotkey`）。面板打开时渲染线程整个 `Draw`（UI 构建加录制，约 0.2–1 ms）都持有这把锁。8kHz 鼠标的每条消息都可能等这么久，造成输入延迟和消息积压。
- 方案：窗口路由表加独立锁，或者改为只读快照；`WM_INPUT` 的热键判断不依赖面板状态，可以在锁外完成。
- 风险：高。Graphics 锁同时保护 panel 的生命周期（`route->Active()`），拆开需要重新论证析构顺序。

### 不做

预览 3 个 tile 合并（只省代码量）；输入钩子 16 次 transaction 合并（只影响启动，而且一个失败会连累全部）。
