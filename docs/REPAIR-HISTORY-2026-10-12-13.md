# 批量部署问题与修复报告（工作记录）

范围：2026-10-12 大量 DX11/DX12 游戏部署以来的问题。用户要求全部问题修好并确认后，提供详细最终报告，并清理排查遗留。本文件当前是证据索引与进度记录，不能当作最终验收报告。

## 问题台账

| 问题 | 已有证据与处理 | 验收状态 |
| --- | --- | --- |
| 死亡搁浅启动退出 | 用户逐轮隔离确认纯 DXGI 转发正常、加载主体即退出；延迟全部依赖后正常，收窄到仅延迟 D3D12 后仍正常。正式加入 D3D12/compiler delay-load，恢复正常 Start 后可进菜单及面板。 | 启动已通过用户实测；NR 和退出另列，不能称整个游戏已修好。 |
| 死亡搁浅 NR 一直 waiting | 日志与实际命令状态确认新建开放列表缺少 Reset 基线；补 CreateCommandList 初始录制追踪，CreateCommandList1 仍要求 Reset，不取消状态恢复保护。 | 后续能够执行 NR，仍有画面问题。 |
| 死亡搁浅 NR 马赛克闪烁 | 依次隔离到输出应用链路；私有 guide、alpha 与同族 depth 实验均未解决。改为独立原图读取、compute 直接写原游戏 UAV 后消失。 | 用户明确确认“闪烁和半幅缺失都消失了”；定向版已通过，正式代码收敛/诊断移除待完成。 |
| 死亡搁浅退出卡死 | 用户报告开启 NR 后退出卡死；WER 有 AppHang 信息，原 dump 已不存在，没有可声称已分析的 DS 卡死调用栈。20:00 这轮按要求先关 NR 再退出，用户确认未卡死。 | 本轮流程通过；尚未覆盖保持 NR 开启直接退出。 |
| 战锤 SM2 启动崩溃 | 实际 dump 经 StackWalk64 展开到 RTSS Present → D3D11On12 → d3d11 无效队列 vtable；仅 Loader 监测漏首次 Present 内 D3D11 装载。补首次 native Present 前安装兼容 hook，并在原 On12 调用期间强引用正确队列。 | 用户确认“已经可以玩了”；不等同于所有 NR 场景均已验收。保留已确认版本。 |
| Alan Wake 2 已开 DLSS 但未识别 | 游戏开启光线重建；补 NGX RR feature 13 与 DLSSD 资源键回退，并覆盖 EvaluateFeature_C。 | 用户确认“alan 的可以了”。 |
| 骑马砍杀 DX11 已开 DLSS 但未识别 | 部署一直是 DX11；补 C 入口后仍不识别。诊断捕到相关导出 lookup，未捕到 Create/Evaluate 执行。没有本地 SR DLL 不代表没有 DLSS；曾补入 SR DLL 已移除。 | 未解决。不得改归为 DX12 或声称游戏没开 DLSS。 |
| 对马岛曝光 Trim 无法调整，Display input | 当前非 sceneLinear 路径固定 divisor=1，白点 Trim 不参与；ref 实际 display passthrough shader 也绕过白点。现有日志不足以验证该游戏 flags/格式判别是否正确。 | 原因解释部分已确认；是否误判尚待证据，未修改图像算法。 |
| 地平线 6 Ins 无反应 | 20:01:44 PID 17800 日志确认面板路径正常；对照 ref 补单键轮询、共享消息/轮询释放边沿和前台根窗口判断。 | 最新用户确认现在能打开面板；NR waiting 单独继续修。 |
| 地平线 6 NR waiting | 20:49:58 PID 34044 日志到达 NR 捕获后的状态保护，反复报 DX12 game PSO unavailable; NR skipped。修正已知空 PSO 被误拒，保留未知状态保护。 | 最新用户明确确认“地平线可以了”，面板与 NR 已通过；保持 42AADAEC 版本。 |
| Jedi 多次切换后 Completed NR GPU 增长 | 用户确认 Opt 也发生；修计时读取同时核对 Context/Layer，防旧模型计时串入。日志并非累计加和；对照不足以证明模型泄漏或 GPU 功耗是唯一原因。 | 不将计时显示修正写成持续性能增长根因已解决。历史监测已清理。 |
| Jedi 启动首次点击卡顿/疑似失焦 | 修原 WndProc 持渲染锁转发、非键盘消息无必要锁等待、焦点缓存刷新。用户后来指出 Opt 同样出现；这些源码风险确实存在，但没有完整根因验证。 | 修正保留，不能写成游戏启动卡顿已完全确认消失。 |
| 龙信 2 启动报错 | 更新正式版后仍报 DD2.exe+0x90F550E；22:03 PID 16128 的 enabled=0 实测仍报相同错误。补查 Opt 官方游戏兼容 Wiki，明确要求 REFramework，目标缺少该前置。已安装官方版本并恢复插件开关。 | 待用户确认正常启动与 NR；前置缺失已确认，尚不能把它写成崩溃根因已实测解决。 |

## 最新批量诊断

死亡搁浅的 Resources 预览显示 fence 完成后的捕获副本；NR debug 显示模型使用的 guide，且发生在模型 Evaluate 之后。两者并非同一读法/时间点。最新证据不证明游戏原始输入有问题，也不证明模型本体故障。

`build/dsdc-batch-audit/`：只采第 1、16、64、128 次模型评估的中心八像素，记录 depth/MV 调用前后、游戏颜色、模型颜色输入、原始模型输出和 resolve 输出；记录实际参数、尺寸/格式、有限 Execute 重复提交检查。样本完成后才 CPU Map，不添加 GPU Wait，不打开 debug layer。样本很小，只能发现其覆盖位置的问题；阴性结果不能排除整幅图的缺陷。采样自身含有限 copy/barrier，可能影响采样帧调度。沿用当前 alpha=1 对照，不把这个假设列为根因修复。

`build/fh6-startup-audit/`：正常源码对象加有限启动日志，最多 8 条 factory、16 条包装路径、3 条 Present、4 次面板接入；不新增键盘轮询/每帧日志/GPU 等待。当前参考实现有 UWP 接入，本工程面板仅 Win32；这是一项代码支持范围差异，尚未证明就是本轮地平线故障原因。

两版 /W4 /WX 编译链接通过。实际批量诊断 capture 源码的 CPU COM 检查通过：pending ledger 已退休仍能识别 cookie 重放，NR 关闭期间 Reset 可失效旧 cookie，预算到期停止，引用计数平衡；不运行 GPU/窗口测试。游戏实测仍由用户完成。

已定向部署并再次独立复核：死亡搁浅主体 `99F1665900F550A7AA1C7BCA63D8FB42EB476FBA3370FFD6ED0C40F80A746B80`，地平线主体 `F6EA4D37D61BEE76CAEAB3ED250752F7B7AF278DE1652847A19E1C60A0086701`。前者重复提交检测预算从已有 NR 捕获开始，避免加载/主菜单提前耗尽；CPU 检查另验未捕获时不耗预算。两款代理/INI/模型保持原哈希，未启动游戏，未刷新其他游戏或正式 dist。部署记录 `build/batch-audits-deployed-20261013.json`。两版都是待用户运行的诊断，不能作为根因修复结果。

## 20:00/20:01 批量测试后，按 ref 继续对照

死亡搁浅 PID 9208：四个中心八像素样本均没有 NaN/Inf；每一帧 depth/MV 调用前后 hash 完全一致，没有样本被模型改写的证据。第 16/64/128 帧 guide 样本相同，颜色样本有变化；小区域结果不能证明全图 guide 正确。1024 Execute 批次观察窗口完成，没有记录到 executions-without-Reset>1；观察窗口较早结束，不能排除窗口外重放。四个资源键的 Get(unsigned long long*) 返回 BAD00010，属于该诊断读法未成功，不等于设置的资源指针为零/缺失。原始输出中心采样为有限且接近输入的颜色，这也不能排除其他图块的马赛克。用户确认仍闪、先关 NR 后退出不卡，地平线仍无反应。

实际对照位置：ref `menu/input/input_system.cpp` 的 PollInputFallbackLocked、`input_system_messages.cpp` 的共享按键边沿、`input_system_window.cpp` 的前台根窗口判别；NR 的 `shaders/dlssnr/DlssNr_Dx12.cpp` TypedGuideFormat/CreateGuideClone/ReadableGuide、guide subrect/MV scale、state envelope 与 32-evaluate 资源退役；`dlssnr/forwarder/dlssnr_forwarder.cpp` create/evaluate 参数及 ABI。没有把仅存在于注释的行为当作实现事实。

正式输入修正：`src/menu_hotkey.hpp`、`src/panel_impl.hpp`、`src/panel.cpp`。在当前窗口路由对应的 active panel Present 上读取一次真实 Ins 高位状态，绕开游戏输入屏蔽；原消息与轮询共用 down/release 状态，按住和重复读取不重复切换。非前台根窗口不触发，失焦保留释放抑制；使用与 ref 一致的根窗口焦点语义，覆盖 render HWND 是子窗口的场景。未创建定时器/线程，不轮询全部键。独立 CPU 检查覆盖消息/轮询重复、快速消息按键、长按、失焦后陈旧释放；不等同于真实游戏输入验证。Release /W4 /WX 构建通过。

死亡搁浅另有确定的实现差异：ref 将 R32G8X24_TYPELESS 同族 CopyResource 为 R32_FLOAT_X8X24_TYPELESS，保留 depth/stencil 的 64-bit 格式载体；本工程此前 compute 转为 R32_FLOAT。原 compute 转换不能仅凭格式差异定为错误；新路线用于把实际接入收敛到 ref。只在 build-only NR pass 中对该格式且不翻转 depth 的情况使用同族复制，保留原像素、subrect、MV 及颜色 shader。每份 recording 持有它实际使用的 guide，避免替换 Context scratch 后旧 recording 失去资源引用；不改变 ledger 回收/队列等待。不把这个差异提前写成已证实闪烁根因。当前 alpha=1 假设未消除闪烁；暂沿用该测试 shader 以单独改变 guide 路径，最终须撤销未证实的实验性改动。

已定向部署：地平线正式主体 `659A75A7C7D8FE8DBF39FCF765F84B40BE7D329127E714625FC9A11BD0E13CE2`；死亡搁浅参考 guide 接入版 `6D90D6B4D84F17682C3BEDDC1E6CF18B69904FC6171874F39C358CE5A5FF1B5F`。记录 `build/ref-alignment-deployed-20261013.json`，备份/本轮日志 `build/before-ref-alignment-20261013/`。原代理/INI/原生 SR/NR 模型均核对不变；战锤等未更新。死亡搁浅保留有限采样；地平线回到无启动诊断的正式输入修复版。尚待两款实测。

## 20:48/20:52 本轮反馈与定向部署

用户确认：死亡搁浅仍闪，NR debug depth 缺上半部分、MV 缺下半部分，水平分界有马赛克闪烁；Resources 预览稳定。地平线现在能打开面板，但 NR waiting。此空间模式不能用此前中心八像素阴性样本排除。当前参考同族 depth 路线没有解决问题；其 format 21 的旧读回在 footprint 筛选处提前返回，并未成功采到 depth，不将这个空结果写成 depth 正常。

preset 核对 ref `dlssnr/DlssNr_Menu.cpp` Model 区和 Config 默认：Default/0 由模型选择；它是创建时渲染提示，不是游戏 SR/RR 的 preset 等级，也不同于 Style。1/2/3 的具体内部行为在本地参考中未说明，不提供臆测的质量/速度排序。选择器属于可选控制；维持 0 足够，写入 0 仍用于恢复默认，避免旧值残留。本轮没有修改 UI。

地平线：`src/nr/command_guard.cpp` 修正已知空 PSO 被拒的问题。epoch、成功 Reset/open creation、valid、renderPass 与嵌套检查照旧；只有这些验证通过后才接纳空 PSO。恢复时不调用 SetPipelineState(nullptr)。这与 ref `D3D12_Hooks.cpp` CanRestoreRootSignature/RestoreRoot 的处理方向一致：NR 没有全局非空 PSO 前置条件，PSO 恢复可缺席。已知空 PSO 下游戏后续有效 draw/dispatch 必须自行绑定 PSO；不伪造一个游戏 PSO，不用 ClearState 清除未被 NR 改动的图形状态。实际包含 command_guard.cpp 的 CPU COM 检查以 /W4 /WX 通过：空 PSO 接纳且不调用空 setter；真实 PSO 恢复、未知/bundle、render pass、闭合列表未 Reset、过期 epoch 仍按原规则处理，引用平衡。Release 正式构建通过。这是源码逻辑修正，实际 FH6 NR 验收尚待用户运行，不能提前写成全部解决。

死亡搁浅：新 `build/dsdc-private-guides/` 定向版本只对 guide 走现有 capture CopyResource 路径，不依赖 Resources 面板是否显示。颜色仍可直接读取；depth/MV 都用捕获副本，与用户观察稳定的 Resources 路径对齐。撤回未奏效的 alpha=1 shader 和 format 21 同族 depth 实验，回到正式颜色 shader、R32_FLOAT depth 转换；保留 recording 的实际 scratch guide 引用与格式核查。正式 capture/pass 未推广这条复制路线，尚无根因证实。额外 guide 复制会消耗显存带宽，不能声称性能无影响，最终方案必须在图像确认后收敛。

有限采样扩大为每个资源七条中心扫描线：首行、1/4 高度、半幅边界前一行/本行/后一行、3/4 高度、末行；仍仅第 1/16/64/128 次评估，分别记录 model 前后 depth/MV、颜色输入、raw 和 resolve。每个样本 buffer 3584 字节，footprint offset 按 512 对齐，每条八像素；资源都用受支持的独立颜色/R32/RG16/half4 格式读取，不做 depth-stencil 原资源的非法部分复制。GPU 采样有限 copy/barrier 会影响四个样本帧的调度，阴性结果仍不能证明整图无异常。不增加等待、debug layer、GPU 负载/频率监测；移除不支持的资源 Get(unsigned long long*) 读取，实际指针由资源描述日志记录。定向版 /W4 /WX 编译链接通过。

部署核验：DS 主体 `43D8569AB85B156AD5D504BF07F572A23F39463233FF929A123DB323CA7E5BB1`；FH6 正式主体 `42AADAEC048314CBCB24C385C441F8039817DC6063A0629E2250AAF67A3E53CD`。备份 `build/before-private-guides-pso-20261013/`；记录 `build/private-guides-pso-deployed-20261013.json`。每款当前 INI、代理、原生 SR 和 NR 模型逐项核对不变，没有启动游戏，战锤及其他部署未更新。下一轮两款各实际场景开 NR 约十秒即可完成自动采样，无需逐项调整 debug。

## 21:28 私有 guide 测试结果与进一步完整图像捕获

地平线 21:11:40 PID 29720：进入 NR feature 创建路径，未再报 game PSO unavailable；用户明确确认地平线可以了。其正式主体仍 `42AADAEC048314CBCB24C385C441F8039817DC6063A0629E2250AAF67A3E53CD`，不刷新该已通过版本。

死亡搁浅 21:28:48 PID 33400：用户明确反馈仍闪、仍缺半幅。私有 guide 路线未消除故障，不将它推广为最终修复。四帧八类资源七扫描线共 224 条样本均采齐且 nonfinite=0；depth/MV 的 56 对 before/after 同位置 hash 全部一致，采样位置并未缺上/下半幅。完整性仍只对这些位置成立，不能推断全图或屏幕正确。1024 Execute 观察窗口未记录重放。日志/结构化样本保存于 `build/dsdc-private-guides-evidence/`。

继续实际核对 ref `shaders/dlssnr/DlssNr_Dx12.cpp` DispatchPass 和 resolve 分支：ref 用自身 hdrCopy 读取未编辑画面，compute shader 直接写原游戏 target UAV；本工程是写独立 packed output 后 CopyResource 回原输出。CopyResource 本身符合尺寸/格式不能直接判错，但确实是可隔离的接入差异。独立 `build/dsdc-direct-resolve/` 比较该写回方法：保留上轮同样的 private guides、正式颜色算法与模型参数；再复制原始颜色供不可变读取，resolve/debug 直接绑定原游戏输出 UAV，避免同一资源同时作为原图 SRV 和输出 UAV；移除不再用的 packed resolve scratch。原输出要求 UAV 状态与写入标志、同尺寸/格式，完成后仍为 UAV，capture 不再对同一个原输出做 result CopyResource。scope 是 build-only 同线程传递，不修改正式 ABI/框架，也不把未证实的路径推广全游戏。

新版本仍四次七扫描线采样，另在第 64 次评估完整复制一帧 depth、MV、raw output 和实际 game resolve 输出。受支持的独立资源都按完整 CopyableFootprints 读回，不部分复制原 depth-stencil；预算与尺寸限制固定。只在既有 GPU ticket 完成后，独立 CPU 工作线程进行 Map/解码/保存 BMP，避免在 render 线程同步转图/写盘，不创建 GPU/窗口测试或新 GPU Wait。输出 `build/dsdc-direct-resolve/images/`；depth 为显示用 contrast 映射、MV 用实际 scale，模型数据不被修改。这一次完整捕获在当前尺寸约需 147 MiB readback 内存及一次全图复制开销，采样结束并导出后释放；本诊断性能不是最终插件性能，不能宣称无成本。单帧图像用于定位空间缺失，不能单凭一帧判断时间闪烁。所有相关线程/helper/header/采样仍在最终确认后的清理范围。

编译与验证：定向 pass/capture /W4 /WX 编译链接通过。CPU 检查包含真实导出与 route scope 代码，验证嵌套/异常退出恢复、footprint 非零 offset、padded row pitch、BMP 顶向行/BGR 顺序、half 解码、Map/Unmap 和引用平衡；没有运行 GPU 或窗口。手工核对原色只读副本与原输出独立、原输出 descriptor/状态、GPU 生命周期由 job 借用资源与 fence 持有。实际 GPU 仍待用户运行，不以 CPU 检查代替画面验收。

仅死亡搁浅已定向更新到 `BE13B13E30004AA56DD23FA5B727C2E5414D2CC294EC422AE448EC369AD19652`；记录 `build/dsdc-direct-resolve-deployed-20261013.json`，备份 `build/before-dsdc-direct-resolve-20261013/`。原配置（用户已关闭 NR）、代理/模型均核对保持；未启动游戏。需要进入场景开 NR 约十秒即可自动导出，无需调试视图逐项切换；仍未宣称闪烁修好。

## 21:46 死亡搁浅通过；新增龙信 2 启动问题

死亡搁浅 21:46:47 PID 15400，主体 BE13B13E：四张完整 BMP 在 21:47:23/24 成功导出，全图解码 nonfinite-pixels=0；实际查看 depth contrast、raw model output、game resolve 图像，均为完整场景，没有可见整半幅缺失。用户随后明确确认“闪烁和半幅缺失都消失了”。时间稳定性由用户实测确认，单张导出图不能独自证明。这一轮变更同时包含不可变原图副本与直接 native UAV resolve，故不能仅以它证明 CopyResource API/驱动本身有缺陷；已确认的是按 ref 对齐后的输出接入行为解决本场景故障。定向 DLL 暂保留已通过版本；其修正需要收进正式代码，再移除有限采样/图像导出等诊断，不能把这份 build-only 版本当作最终清理完成版。完整运行日志保存 `build/dsdc-direct-resolve/verified-user-run.log`。

龙信 2：用户随后报告启动就报错。检查目标 `F:\SteamLibrary\steamapps\common\Dragons Dogma 2`，原主体 F3FB5E3D90D2758BB06982BC22F8BE3B11F79A01D4D0DBF3F6AAD4A657597F51、代理 677976A79ACC12C6E929329E41158401A126214268CB780909802A7692674A33，正是先前只登记而未替换的早期部署。DLL PE 导入表确认旧主体直接依赖 d3d12.dll/D3DCOMPILER_47.dll，当前正式版为 delay imports。游戏日志只有 10-12 23:53 和 10-13 00:34 的两次启动，不能作为当前这轮报错的新日志。CrashReport 目录无文件，没有 DD2 CrashDumps；只读 Windows Application 事件找到 10-12 23:18 的 RADAR_PRE_LEAK_64，未找到可解释本次启动错误的 exception/dump。RADAR 事件不能证明本插件泄漏或启动崩溃根因。

实际参考 DD2 条目 `misc/Quirks.h`：DisableDxgiSpoofing/DisableHudfix、非 NVIDIA 的 compute signature 恢复。本工程没有对应 spoofing/HUD fix 功能；不盲目加无效 DD2 专项开关，不把这些条目当成已诊断根因。先将未更新的部署补到当前正式兼容版，包含已经在 DS/SM2 排查中确认的 delay-load、首次 Present 前 On12 hook 与队列强引用、factory 调用边界等修正。等待用户提供具体错误文字，继续以实测确认，不声称这一更新已经修好龙信 2。

龙信 2 已用已有 `tools/deploy.ps1 -Target dd2` 定向部署：主体 42AADAEC048314CBCB24C385C441F8039817DC6063A0629E2250AAF67A3E53CD，代理 11076F50635B213BD3C04DBC5F07B24D4D43622A4A3F9CB1F71059A4F8CFC3CD。原配置、nvngx_dlss/dlssd/dlssg/dlssnr 均保持原 hash。备份与部署记录 `diagnostics/2026-10-13/dd2-before-compatibility/`；不再为此新建诊断 DLL/临时部署脚本。没有启动游戏，没有刷新已通过的死亡搁浅、地平线或战锤。

build 盘点：当时顶层 29 个目录、175 个散文件；windows-release 的正式构建约 23.2 MiB，DS 完整图像与定向版约 71.2 MiB，startup-audit-clean 重复构建约 14.3 MiB，其他多为独立诊断源码/对象/工具/DLL/shader、部署记录及回退副本。这些并非全部是正式构建所需。最终清理仍必须覆盖旧诊断目录、散文件和重复构建，必要证据归档到 diagnostics，避免仅删除最新一轮文件而遗漏前日积累。新增龙信 2 尚未验收，因此还不能记为“所有问题已确认、清理完成”。

## 22:02 龙信 2 新版启动仍报错，分离 hook 层

用户提供错误文字 `an unhandled exception occurred 0x7ff6eba00000 + 0x90f550e`；这是模块基址加偏移，不是异常类型/代码。新日志 21:58:04 PID 21104 确认当前正式 DLL 已启动，日志仍止于文件发现，NR 配置本身为关闭，没有 NR 模型初始化记录。读取 DD2.exe 的 PE section 表确认 RVA 0x90F550E 落在 `.text`；静态地址不能替代异常上下文和调用栈，也不能证明游戏自身或插件是根因。CrashReport 目录仍空，近三小时 Windows Application 1000/1001/1002 未找到 DD2 有效异常记录。

补核 ref `hooks/Kernel_Hooks.cpp` 的 DD2 注释：其 CreateFileW/GetFileAttributesW spoof 原先把 `nvngx.dll_dlssnr.dll` 错当成游戏 NGX，重定向到驱动签名 DLL；通过排除 `nvngx.dll_` 修正。本工程没有这些文件打开/属性 spoof hook，这段修正不适用，不按注释盲加排除分支。ref SkipFirstFrames 位于 NGX Evaluate 替代渲染逻辑，本轮 NR 尚未初始化，不能凭历史 changelog 推断跳过帧就能解决启动错误。

当前只在龙信 2 INI 将 General.Enabled 从 1 改为 0；主体与代理保持原 hash。Start 仍加载主体、正常读取配置与初始化日志，但不调用 InstallCapture/DiscoverModelRuntime，配置写入 worker 也因 disabled 不启动；Usable 为 false，使代理 factory attachment 也绕开。该轮没有面板/NR，用于区分 hook 与主体加载/转发层，尚待用户启动结果。原 INI、失败日志与状态记录保存在 `diagnostics/2026-10-13/dd2-before-compatibility/before-no-hooks.ini`、`failed-formal-run.log`、`no-hooks-check.json`。未新建诊断 DLL/源码或启动游戏。后续必须恢复 General.Enabled=1，不能将禁用状态作为修复验收或最终部署。

## 22:16 龙信 2 补齐官方兼容前置；复制至用户 Mod 仓库

用户确认 disabled 版本仍报相同错误；22:03:37 PID 16128 的实际日志为 enabled=0，没有安装 hook 和发现模型日志。这将本轮范围缩到加载/转发/初始化之外的兼容条件，不能再把 NR 计算/输出或本轮 hook 状态恢复当作原因，也尚不能证明主体的普通依赖加载完全无关。

继续补查 [Opt 官方龙信 2 兼容页](https://github.com/optiscaler/OptiScaler/wiki/Dragons-Dogma-2)，其 Notes 明确要求 REFramework，标准部署为 dinput8.dll 与 Opt dxgi.dll 并存，建议将 REF 菜单键改为 Delete。目标实际没有 dinput8.dll/REF 配置。此前只看本地 Quirks/Kernel 等源码，没有覆盖官方游戏安装要求，遗漏了这个前置；先前“本插件没有 spoof 所以该修正不适用”只针对那段源码，不足以覆盖游戏的完整兼容要求。停止准备移开主体的下一轮对照，直接按官方安装方案补齐前置；没有实际执行主体移开。

官方 REFramework nightly 01432，commit d052e8fd3fd41d0a1a33394fcf6e6c19be39b1ee。该 release 已使用统一 REFramework.zip，发布说明明确包含 DD2、非 VR 只解出 dinput8.dll。包 SHA256 27376E1DA177DDCDE9C46B3541BDC6FA1AD54A11D0CAB9F7782EFF0481455E94 与 [官方资产清单](https://github.com/praydog/REFramework-nightly/releases/expanded_assets/nightly-01432-d052e8fd3fd41d0a1a33394fcf6e6c19be39b1ee) 一致；DLL 为 x64 PE32+，23084544 字节、SHA256 70A2A3C24030D8BC3695554E7F929FD1B0B4B757FB5BA2C9D34F42D7155005AC。没有执行下载的代码。

配置实际核对同 commit 的 src/REFramework.cpp、mods/REFrameworkConfig.hpp/.cpp 与 Mod.hpp；其菜单键为虚拟键 REFrameworkConfig_MenuKey_V2，配置名 re2_fw_config.txt，完整键名由 mod name 拼接。再核对构建指定 kananlib commit 43cb5353615dcbc5d4945b1b9ab685b11e8b0d37 的 include/utility/Config.hpp 与 src/Config.cpp：逐行 key=value，bool 使用 true/false。最小新增配置为 MenuKey_V2=46 (Delete)、RememberMenuState=true、MenuOpen=false，避免与本插件 Ins 共用、初始化完成后默认不展示 REF 菜单。没有加任何 Lua/C#/VR 可选组件或脚本；该独立框架自身有初始化与 hook 成本，尚无本游戏的性能测量，不声称零开销。

22:16:10 已定向部署游戏 dinput8.dll 与这份最小 REF 配置，原插件 INI 恢复为关闭诊断前的完整 hash F242CD864B26449C0AA9819BFA37630387920FD51301F2BF696F838BD7581544（General.Enabled=1、NR.Enabled=0）；主体仍 42AADAEC、代理仍 11076F50。当前未修改本插件源码或其他已通过游戏。用户另明确要求存入 E:\Mod\REMod\REFramework，已复制官方 REFramework.zip、解出的 dinput8.dll 与 revision 文件并核对 hash；不把诊断源码复制到仓库/游戏。记录与 disabled 失败日志在 diagnostics/2026-10-13/dd2-reframework/deployment.json、disabled-still-failed.log。所有游戏实测仍由用户完成；等待一次正常启动、实际场景 NR 与退出反馈，不提前记为修好。

## 最终清理承诺与边界

触发条件：所有问题修好，并收到用户确认后执行；现在不提前删除需要继续定位/回退的文件。

- 从正式源码及最终 DLL 核查并移除此次新增的临时采样、成功路径诊断、额外参数读取与辅助字段；保留正常面板计时、必要状态/同步保护及简短失败日志。
- 每款游戏替换临时主体为其经过验证的最终版本，核对 hash；清理此次放入游戏目录的中性名/load-only/disabled 等临时 DLL，逐个核实归属，不删除游戏原文件或其他注入器。
- 清理 build 内临时诊断源码、shader/header、独立 CPU 工具、对象/DLL、临时链接与部署脚本和不再需要的回退副本；保留正式构建所需内容。
- 将关键 dump/日志事实、版本与验证限制收敛到最终报告。需要的证据先归档再删除重复采集，不把诊断依赖继续留在正式构建或部署流程中。
- 不恢复已经由用户删除的跑分工程，不重新加入 GPU 负载/频率/温度/功耗监测。

## 最终报告待补齐的内容

逐游戏实际部署清单与最终版本、具体代码位置、每项根因的证据强度、行为修复与诊断改动的区别、验证结果及未覆盖情况、性能/资源生命周期影响、清理前后核验。报告包括失败修法、回退及错误假设造成的影响，不能只列成功修改。

## 历史证据索引

- `docs/STARTUP-CAPTURE-2026-10-13.md`：死亡搁浅、战锤、AW2、骑马砍杀、对马岛的过程与证据修正。
- `docs/DEPLOYMENTS.md` 与 `../DLSSNR_LiteDX11/docs/DEPLOYMENTS.md`：目标目录及历史部署；其中“当前”按时间较新记录覆盖较早记录。
- `docs/SWITCH-TIMING-2026-10-12.md`、`docs/STARTUP-INPUT-2026-10-12.md`、`docs/DIAGNOSTICS-CLEANUP-2026-10-12.md`：之前的性能/输入检查及已完成清理。
- `build/*deployed*.json`、`build/*verified*.json` 与 `build/deployment-*.json`：实际复制核验，不以编译成功代替部署/实测。

留存复核补充：复制时 E:\Mod\REMod\REFramework 根目录的包/DLL/revision 均已核对；之后目录被整理为 v26_10 与 v26_6，根目录安装包已不在。当前 v26_10/dinput8.dll 与游戏 dinput8.dll 逐项 hash 一致（70A2A3C…），未重建/覆盖这次目录整理。官方安装包仍保存在工程 diagnostics/2026-10-13/dd2-reframework/REFramework-official.zip。
