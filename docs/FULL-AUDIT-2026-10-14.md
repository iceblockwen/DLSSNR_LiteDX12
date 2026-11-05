# DX12 / DX11 完整工程审查

日期：2026-10-14。审查基线为最近一次曝光开关更新后的源代码和已构建版本。本轮只审查，未修改插件源代码、INI、游戏文件或部署状态。

后续状态：用户圈选的第 4—7 项已修复并部署，验证与实现边界见 [修复记录](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/docs/AUDIT-FIXES-2026-10-14.md)。下文保留修复前的审查证据；第 1—3 项仍未修复。星空新报告的持续 waiting 已找到部分输出拒绝日志，具体尺寸根因尚待数据。

## 结论

仍有需要处理的问题。优先处理三项稳定性风险：**DX12 已关闭命令列表重复提交后的资源保留、DX11 光标恢复的锁顺序、DX11 输入 detour 安装的线程保护**。另外发现两项明确的持续性能浪费，以及两项 DX11 输入契约 / 历史归属问题。

这里的“确认”指源码路径或 CPU 检查确认；不代表已在某款游戏中复现崩溃。未把这些发现直接归因为此前死亡搁浅、战锤、绝地或骑砍的问题，也没有根据静态审查虚构帧率提升。

| 编号 | 优先级 | 工程 | 问题 | 证据程度 |
| --- | --- | --- | --- | --- |
| 1 | P1 | DX12 | 首次提交完成后，重复执行同一已关闭列表不再更新保留与 fence | 生命周期缺口确认；游戏是否使用该模式未新增验证 |
| 2 | P1 | DX11 | 持有 nativeMutex 恢复鼠标 capture，可与 GraphicsMutex 形成反向等待 | 锁顺序与同步回调路径确认；未运行游戏复现 |
| 3 | P1 | DX11 | 输入 detour 只登记当前线程，漏掉执行被修改代码的其他线程 | 源码与 Detours 官方契约确认 |
| 4 | P2 | DX11 | 已使用游戏 NGX guides，仍执行原生深度 draw 统计与备份 | 调用链确认；耗时未实测 |
| 5 | P2 | 两份 | DirectInput 每次键盘轮询仍逐键映射、逐键锁检查 | 每次调用的重复工作确认；耗时未实测 |
| 6 | P2 | DX11 | NGX 尺寸为零不回退；有效输出尺寸没有传给部分输出保护 | 直接调用生产头文件的 CPU 检查确认 |
| 7 | P2 | DX11 | NR 历史按 context 归属，缺少 NGX feature 身份 / 代际 | 归属与释放路径确认；需要对应游戏触发条件 |

P1：应优先解决的崩溃、挂起或 GPU 资源生命周期风险。P2：特定输入下的错误行为或持续多余开销。本轮没有发现可以断言“每个游戏都会立即崩溃”的 P0 问题。

## 1. DX12 重复执行已关闭命令列表仍存在资源生命周期缺口

位置：[capture12.cpp:508](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/src/nr/capture12.cpp:508)、[capture12.cpp:540](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/src/nr/capture12.cpp:540)、[capture12.cpp:148](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/src/nr/capture12.cpp:148)。

`PrepareSubmission` 和 `Submitted` 都只查 `pending`，并在没有 pending 时直接返回。首次提交后 job 从 pending 移到 submitted；首次 fence 完成后 `Collect` 释放借用输入、model recording、resolve target 和部分工作资源。命令列表的 private-data cookie 只负责取消未提交记录，没有保存一个已提交列表的完整执行资源集。

触发条件是游戏在没有 Reset / 重新录制的情况下，再次执行同一份包含注入 NR 命令的已关闭列表。它可以发生在首次执行完成之后；此时既没有新 pending，也不会补发 Signal。列表仍包含旧 GPU 地址 / 描述符 / 模型命令，而这些资源可能已经被回收、重写或用于另一帧。结果可能是图像错乱、GPU device removed 或挂起。

这与普通的一次录制、一次提交路径不同；普通路径不能据此判定有 bug。微软说明命令列表可以重复执行，应用仍须保证相关 GPU 生命周期：[命令队列与命令列表设计](https://github.com/MicrosoftDocs/win32/blob/docs/desktop-src/direct3d12/design-philosophy-of-command-queues-and-command-lists.md)、[ExecuteCommandLists](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-executecommandlists)。

参考实际核对：[Opt 的退休队列](H:/AgentWorkPlace/DLSS5/_ref/OptiScaler_DLSSNR/OptiScaler/shaders/dlssnr/DlssNr_Dx12.cpp:648) 将 feature / resource 放进 `NrRetired`，初始 `framesLeft=32`，在 `TickNrRetired` 中递减后释放。它采用保守延迟，不能据此推导出“首次 fence 完成就覆盖了未来所有 replay”。32 次延迟本身也不能严格保证任意晚发生的 replay 安全。

建议：不要恢复之前被退回的 pre-hook busy 修法。需要按列表的录制代际保留注入资源与槽位，在 post-hook 中对识别到的 replay 更新完成证明；Reset / 列表销毁后，等待该代际最后一次实际提交完成再退休。跨队列还必须保持真实的前驱依赖，不能只补一个 Signal 就宣称全部安全。额外工作应仅限被注入的列表；没有注入记录时保留快速返回。必须同时设计保留上限和安全旁路，避免长寿命列表占满当前四槽后让普通帧持续 busy。

## 2. DX11 恢复鼠标 capture 时仍持有 nativeMutex

位置：[input.cpp:91](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX11/src/input.cpp:91)、[input.cpp:160](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX11/src/input.cpp:160)、[panel.cpp:321](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX11/src/panel.cpp:321)、[panel.cpp:1284](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX11/src/panel.cpp:1284)。

`RestoreNativeLocked` 在 nativeMutex 内执行 `ReleaseCapture` / `SetCapture`。捕获变化会同步进入窗口回调；`ReleaseCapture` 也会产生 `WM_CAPTURECHANGED`，即使是窗口自己释放。官方契约：[WM_CAPTURECHANGED](https://learn.microsoft.com/en-us/windows/win32/inputdev/wm-capturechanged)。

存在以下反向锁顺序：GUI 线程持 nativeMutex，恢复 capture，窗口回调尝试取得 GraphicsMutex；渲染线程持 GraphicsMutex，面板输入转换尝试取得 nativeMutex。两线程重叠时可以互相等待。面板关闭、焦点变化或恢复游戏 capture 是相关触发场景，平常不发生 capture 恢复时不一定遇到。

DX12 已在 [input.cpp:89](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/src/input.cpp:89) 使用 `CaptureRestore` / `NativeLock`：先记录恢复动作，退出锁后才执行 SetCapture / ReleaseCapture。DX11 缺少这项修复。建议同步该机制；增加 recursive_mutex 不能解决跨线程的反向等待。正常帧不需要增加轮询或 GPU 工作。

## 3. DX11 输入 detour 安装未保护其他执行线程

位置：[input.cpp:240](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX11/src/input.cpp:240)。

输入层 `Attach` 使用裸 Detours transaction，只调用 `DetourUpdateThread(GetCurrentThread())`。当前线程的伪句柄不会保护其他线程；其他线程若正执行目标入口或 trampoline，提交时不会调整它们的指令位置，可能执行混合的新旧代码。输入 hook 在游戏已建立交换链和面板时安装，不能假定那时只有一个线程。该路径也被 DirectInput hook 使用。

这是 Detours 明确描述的风险：[DetourUpdateThread](https://github.com/microsoft/Detours/wiki/DetourUpdateThread)。DX12 输入层和两份工程的其他主要 hook 已使用 [detour_hook.hpp](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX11/src/detour_hook.hpp:19) 的线程登记机制。

建议：DX11 输入层统一使用现有 transaction / 线程保护；同时考虑将可以一起安装的入口合并成少量事务，避免每个入口重复枚举 / 暂停线程。安装是一次性成本，不应把暂停线程放入常规键盘轮询路径。

## 4. DX11 已有游戏 NGX guides，原生深度捕获仍消耗 CPU / GPU

位置：[depth_hooks.cpp:11](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX11/src/nr/depth_hooks.cpp:11)、[depth11.cpp:96](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX11/src/nr/depth11.cpp:96)、[diagnostics.cpp:463](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX11/src/nr/diagnostics.cpp:463)、[diagnostics.cpp:526](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX11/src/nr/diagnostics.cpp:526)。

原生深度 hook 的 Active 只判断 NativeDX11 和 ProcessingActive，没有判断当前 context 是否已经由游戏 NGX 提供 color / depth / MV。NR 开启且 NativeDX11 保持默认启用时，即使 NR 使用游戏真实 guides，每次 draw 仍可进入全局深度锁、查表和统计；换绑、clear 或 Finish 时仍可分配 / 复制候选深度。

`externallyFed` 只在 Present 的 EndFrame 阶段阻止最终选择与备份，之前的 draw 统计和 CopyResource 已发生。这些工作在稳定的游戏 NGX 路径中没有被消费，属于明确的重复工作，尤其影响 draw 很多的 DX11 游戏。

建议：按 context 缓存输入来源和 fallback 的实际需求。健康 NGX 路径稳定后停止无需求的候选统计 / 备份；NGX 输入消失或原生预览确实需要时恢复，并正确处理当前深度绑定和历史失效。不能直接删除 NativeDX11 fallback：骑砍未捕获 NGX 的历史问题仍未解决。不要把来源判断换成每 draw 再加一个大锁。

## 5. 两份工程的 DirectInput 键盘轮询有逐键重复工作

位置：[DX12 input_directinput.cpp:55](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/src/input_directinput.cpp:55)、[DX11 input_directinput.cpp:55](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX11/src/input_directinput.cpp:55)、[input_keyboard.cpp:69](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/src/input_keyboard.cpp:69)。

每次 256 字节键盘 `GetDeviceState` 调用都会读取 capabilities、重新匹配已知设备入口，然后遍历全部 256 个扫描码。面板关闭、没有抑制状态时，即使某键已经是零，仍调用 `VirtualKey` 和 `PanelKeyboardPollBlocked`；通常每次做 255 次 MapVirtualKey 和大量独立 mutex 获取。这个开销与 NR 是否开启无关，只取决于游戏是否使用这些输入 API。

建议：已经为零的键清理 suppression 后直接跳过；设备 kind / trampoline 在受生命周期保护的设备记录中缓存；扫描码映射随键盘布局缓存并在布局改变时更新；批量取得必要的键盘所有权状态，减少逐键锁。Win32 `GetKeyboardState` 的同类循环也可一起处理。

必须保留 Ins 热键归属、面板关闭后等待物理 UP，以及此前已经发给游戏的按键释放。不能简单把“面板关闭”变成全盘不处理，否则会恢复粘键问题。Opt 的 [DirectInput 实现](H:/AgentWorkPlace/DLSS5/_ref/OptiScaler_DLSSNR/OptiScaler/menu/input/input_system_directinput.cpp:122) 在设备记录中保存 kind，实际核对后可作为缓存归属的参考。

## 6. DX11 NGX 尺寸契约遗漏两项 DX12 已有的保护

位置：[DX11 guide_contract.hpp:12](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX11/src/nr/guide_contract.hpp:12)、[DX12 guide_contract.hpp:12](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/src/nr/guide_contract.hpp:12)、[DX11 capture.cpp:50](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX11/src/nr/capture.cpp:50)、[DX11 pass.cpp:362](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX11/src/nr/pass.cpp:362)。

**第一项：NGX Get 成功但返回零。** `Read` 的默认参数只覆盖 Get 失败，不覆盖成功返回 0。DX12 会用 feature 创建尺寸，再用 depth 可用范围回退；DX11 直接把 wanted=0 交给 bounded，返回 E_INVALIDARG。符合该参数行为的游戏会丢失本来可用的输入，NR 持续 waiting / 捕获失败。

**第二项：有效输出区域没有传递。** DX11 没把 metadata.outW / outH 写进 guides.width[0] / height[0]。已有的 partial-output guard 因而看见零，无法拦截“原点为零、有效区域小于纹理分配”的输入，后续按整个分配范围执行 NR / CopyResource，可能处理未定义区域或覆盖不应处理的部分。原点非零的情况仍能被既有检查挡住。

本轮 CPU 验证直接包含两份工程的生产 `guide_contract.hpp`，无替代算法、无 GPU / 窗口调用。/W4 /WX 编译通过，输出为：

```text
zero evaluate dimensions: DX12=0 DX11=80070057
partial output metadata: DX12=1280x720 DX11=0x0
Confirmed both DX11 contract discrepancies. CPU only; no GPU or window calls.
```

建议同步 DX12 的零尺寸回退和声明输出尺寸传递；超出分配或部分输出继续保守拒绝。无需新增 GPU dispatch / 资源复制。这不是已证实的骑砍根因：历史骑砍问题还包括未观察到实际 NGX create/evaluate，不能用此检查假称修好了骑砍。

## 7. DX11 NR 历史没有跟随游戏 NGX feature 身份

位置：[capture.cpp:110](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX11/src/nr/capture.cpp:110)、[capture.cpp:173](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX11/src/nr/capture.cpp:173)、[diagnostics.cpp:38](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX11/src/nr/diagnostics.cpp:38)、[diagnostics.cpp:462](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX11/src/nr/diagnostics.cpp:462)。

Evaluate11 知道 NGX handle，但它没有传入 Capture11 / Input11。Registry 以 ID3D11DeviceContext* 为键，每个 context 只有一套 model / 历史。Release11 仅删除捕获 metadata，也没有使对应 NR stream 退休或重置。

触发条件：同一 context 轮流处理多个 SR feature / 视图，或者 release 后创建同尺寸的新 feature，且 evaluate 没有给出 Reset。NR 可能沿用旧视图或旧 feature 的历史，出现错误的时间累积；不能仅凭分辨率相同判断历史可以共享。正常单 feature、正确给 Reset 的游戏不一定受影响。

DX12 已使用 device + stream(handle) 分开管理，并在 ReleaseModelStream 中退休。DX11 建议传递 feature 身份和创建代际，区分游戏 NGX 流与原生 fallback；若暂时只支持一个流，也应在身份变化时明确失效历史，而不是静默共享。实现应限制流数量并按真实 fence 退休，避免为理论多视图支持无限保留上下文。

## 内存、构建、兼容补丁与清理检查

- 未发现足以认定正常单上下文路径“每帧无限增长”的新泄漏。DX12 pending + submitted 上限为 4，copy pool 每设备上限 8，模型流上限 4；白点和计时池也有上限。DX11 桥接保留两个执行槽，busy 返回避免 CPU 等待。资源保留不能仅凭工作集没有立刻下降判为泄漏；第 1 项是回收过早问题。
- Signal 失败或 GPU completion 不可知时继续保留资源，是安全隔离行为；不能为清理内存而强行 Release 仍可能执行的对象。进程级固定单例与 pinned DLL 也不等于逐帧泄漏。
- 参考与旧 Lite 未修改。死亡搁浅额外私有输入 / 原 UAV resolve 由启动时缓存的 ds.exe 策略限定；GTA 小交换链策略同样限定目标。未发现本轮把死亡搁浅额外复制无条件推广到全部 DX12 游戏。
- 延迟加载 D3D12、交换链按设备类型过滤、RTSS / On12 正确队列保留等既有兼容改动仍有用途，不宜因“某游戏的补丁”而直接删除。根据现有代码与此前验收，没有在本轮发现必须回退这些修复的证据。
- 两份代码没有检索到此前 GPU 频率 / 负载监测的 NVML / NvAPI 路径或独立 benchmark / 图片 dump 入口。仍存在的 GPU timestamps、白点读回、Resources / Debug preview 是现有产品功能，按需求执行，不能把名称为 diagnostics 的文件整体删除。
- 曝光勾选没有新增 GPU pass，仍沿用此前 CPU 验证过的公式与纹理优先级；本轮未修改 shader、图像算法或 UI。
- DX11 仍有清理遗留的空 if / 作用域，以及 hooks.cpp 的 `hooksAdded` 写入后不使用。它们可以做源码清理，但没有证据表明删掉这些空块会带来实际帧率收益，优先级低于上述问题。
- DX12 每个输入入口分别进行线程保护事务，有启动阶段重复枚举 / 暂停的成本。适合与第 3 项一起审慎批处理；当前代码不足以证明它就是此前首次点击卡顿的根因。
- 两份代码分叉已经出现修复遗漏：cursor、线程保护、guide metadata 三处都有具体例子。框架的模块划分总体合理；目前更大的维护风险是共享契约没有同步，而不是 C++ 文件数多。优先对齐输入 / 尺寸 / 生命周期契约，不建议为了“精简”立即进行大范围抽象或删除安全保留。

## 覆盖与验证边界

审查覆盖两份 src 下的 C++ / 头文件 / HLSL / 代理 asm 和导出表，以及 CMakeLists、presets、Build.ps1、默认 INI。相同代码采用对照 / 差异核对，独有路径单独检查。记录清单为 DX12 67 个文件、DX11 59 个文件，共 126 个文件；第三方 vendored 依赖未逐行审查，也未反编译 NVIDIA 模型。重点链路如下：

| 范围 | 检查内容 |
| --- | --- |
| 启动 / 代理 / 模块加载 | DLL 初始化、延迟依赖、系统 DXGI 转发、原生工厂来源、失败旁路 |
| Hook / COM 包装 | API 签名、transaction、线程登记、交换链类型与引用、释放 / resize |
| 输入 / 面板 | 锁顺序、GUI 回调、raw / legacy / DirectInput、焦点和 cursor 恢复 |
| DX12 捕获 | NGX SR / RR / _C、参数尺寸、命令状态保存恢复、pending / submitted / Reset |
| DX11 捕获 / 桥接 | NGX 身份、深度候选、生成 MV、共享纹理、双槽、GPU fence 交接与失败路径 |
| NR / 测光 / 预览 | feature 创建 / 退休、资源和描述符归属、历史失效、meter / timing 需求与池复用 |
| 构建 / 配置 | shader 构建期编译、发布文件、异步 INI 保存、备份与诊断不进入正式构建 |

本轮审查前，同基线的 DX11 / DX12 Release、Debug 四次构建均成功，见 [曝光更新验证记录](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/docs/EXPOSURE-INPUTS-2026-10-14.md)。本轮另外执行四份 Ninja dry-run，均为 `no work to do`；这说明构建器没有判定生产输入待重编，**不把 dry-run 当成重新编译或游戏测试**。

本轮唯一新增执行检查为上面的 CPU 尺寸契约检查。没有运行游戏、GPU、窗口或性能跑分，没有新增真实游戏日志 / dump。本报告不能量化每项开销占多少毫秒，也不能证明所有设备 / 游戏无异常。

源码 SHA256 清单：[source-inventory.json](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/diagnostics/2026-10-14/full-audit/source-inventory.json)。CPU 源码、编译日志、执行结果及检查二进制统一归档在同目录 verification-evidence.zip，未加入 CMake / Build.ps1，未在 build 中留下测试工程。

## 建议处理顺序

先同步 DX11 已经存在于 DX12 的光标恢复与输入 transaction 修复，再同步尺寸保护。这三类改动范围小，不应增加常规帧 GPU 工作。随后按实际 NGX 来源关掉多余的原生深度工作、减少键盘轮询重复处理。最后单独处理 DX12 replay 保留及 DX11 feature 历史归属，明确限额、失败旁路和退休契约后再实施，避免再次把普通路径改成持续 busy。

历史游戏修复 / 未解决项仍以 [详细修复报告](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/docs/REPAIR-REPORT-2026-10-12-13.md) 为准。本报告没有宣称骑砍捕获问题、模型切换后长期 GPU 时间变化或全部游戏性能已验收。
