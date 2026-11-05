2026-10-14 最新：用户已确认星空创建前快照修复有效；临时诊断清理完成，全部 20 个游戏已部署并核验，16 个 DX12 为 4CB7542E…，4 个 DX11 为 A6987DE5…。[最新清理与全量部署报告](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/docs/CONFIRMED-CLEANUP-2026-10-14.md)；[星空根因与修复报告](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/docs/STARFIELD-CREATE-METADATA-2026-10-14.md)。

2026-10-14 后续更新：完整审查第 4—7 项已修复，四份构建及五组 CPU 检查通过，全部 20 个登记游戏已部署；审查第 1—3 项尚未修复。星空日志指向部分输出保护，已增加失败尺寸详情，但故障根因尚未确认。[审查修复、验证与部署报告](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/docs/AUDIT-FIXES-2026-10-14.md)。下方版本状态作为历史记录保留。

2026-10-14 更新：两份工程新增曝光输入独立开关，保持原公式；已重新构建并部署全部 20 个登记游戏。[新增开关、验证和最新部署报告](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/docs/EXPOSURE-INPUTS-2026-10-14.md)。下方 SHA256 和日期是 10 月 13 日版本的历史记录。

# DLSSNR DX11 / DX12 修复、清理与全量部署报告

日期：2026-10-13。覆盖 10 月 12 日批量部署以来的兼容问题与此前模型计时、输入、DX11 工程检查。本次收尾已完成代码清理、Release / Debug 构建、20 个登记游戏的 Release 部署及逐文件校验。**全部部署完成不代表所有历史问题都已解决：骑马砍杀的游戏 DLSS 捕获仍未解决。**

性能、稳定优先：保留用户实测通过的修复；游戏特殊路径限制作用范围；移除测试入口和排查采样，不修改模型数学、图像 shader 或面板布局。当前文件给出最终状态；[详细排查时间线](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/docs/REPAIR-HISTORY-2026-10-12-13.md)保留曾经失败、回退、待验证的历史记录，其中旧的“当前”“待部署”不代表本报告状态。

## 1. 问题与验收状态

| 问题 | 处理结果 | 已有验证与边界 |
| --- | --- | --- |
| 死亡搁浅加载插件即退出 | D3D12 / compiler 延迟加载，正式代理恢复正常 Start | 用户逐项隔离确认仅延迟 D3D12 后可以启动；正式面板可打开。 |
| 死亡搁浅 NR waiting | 跟踪 CreateCommandList 初始开放录制；保留 Reset / epoch / render pass 状态保护 | 后续日志实际创建并执行 NR。 |
| 死亡搁浅马赛克、depth/MV 缺半幅及闪烁 | 私有颜色、depth、MV 输入；compute 直接 resolve 到原游戏 UAV；转换 guide 引用随 recording 持有 | 用户明确确认“闪烁和半幅缺失都消失了”。本次把有效路径纳入正式源码并删除采样。组合修复有效，不据此认定 CopyResource API 本身有缺陷。 |
| 死亡搁浅退出卡死 | 修正列表身份匹配等生命周期问题；不再以接口地址误匹配提交 | 用户后来确认先关闭 NR 后退出不再卡死；保持 NR 开启直接退出未获得独立验收，原 AppHang dump 不存在，不能声称分析过其卡死调用栈。 |
| 战锤 SM2 启动崩溃 | RTSS 第一次 native Present 前安装 On12 hook；原 On12 调用期间强引用正确队列 | 实际 dump 栈指向 RTSS → On12 → d3d11 无效队列 vtable。用户确认“已经可以玩了”。本次保留行为并减少稳态重复查询。 |
| Alan Wake 2 打开 DLSS RR 后未识别 | 接入 RR feature 13、DLSSD 资源键回退和 EvaluateFeature_C | 用户确认可以。普通 SR feature 1 路径仍保留。 |
| 骑马砍杀 DX11 打开 DLSS 未识别 | 已覆盖 DX11 C evaluate 入口，移除错误补入的 SR DLL；本次更新正式清理版 | 仍未解决。诊断只捕到相关导出 lookup，没捕到 Create/Evaluate 实际执行；不能说用户没开 DLSS或游戏没有 DLSS。 |
| 对马岛 Trim 不可调，显示 Display input | 保留当前 display-domain 路径 | 当前 shader 的 divisor=1，Trim 不参与；ref 的 display passthrough 同样绕过白点。该游戏是否被误判为 display domain 尚无足够证据，不盲改算法。 |
| 地平线 6 Ins 没反应 | 每次实际 Present 检查单个菜单键，与窗口消息合并按下/释放边沿；焦点使用前台根窗口 | 用户已确认面板能打开。没有全键扫描或新增轮询线程。 |
| 地平线 6 NR waiting | 接纳有效开放录制中的“已知空 PSO”，恢复时不调用 SetPipelineState(nullptr) | 用户明确确认地平线可以了；未知状态、闭合列表、错误 epoch、bundle / render pass 保护保留。 |
| 龙信 2 启动报 DD2.exe+0x90F550E | 根据 Opt 官方兼容页补齐 REFramework；插件总开关恢复正常 | 升级插件和关闭 hook 都未解决；装官方前置后用户回复“可以了”。没有有效本轮异常 dump，不把静态 RVA 当成异常代码或完整根因栈。 |
| Jedi 多次切换后 Completed NR GPU 持续增长 | 防止旧 Context / Layer 的计时串入当前显示；相同尺寸保留工作纹理 | 用户确认 Opt 有相同现象。显示一致性和切换分配风险已修；持续增长的根因并未证明解决，不能归结成模型泄漏或功率限制。 |
| Jedi 首次鼠标点击卡顿 / 疑似失焦 | 转发游戏 WndProc 前释放插件渲染锁；先过滤无关消息；刷新焦点缓存 | 修正明确源码阻塞风险。用户表示 Opt 也存在；首次点击现象的完整根因仍未确认。 |

## 2. 死亡搁浅正式收敛：为何保留这些代码

实测有效版本 BE13B13E 同时改变了输入原图保存和最终写回路径。ref 实际使用 hdrCopy 保存未编辑原图，compute resolve 直接写游戏 target。本工程此前使用独立 packed output，再复制回游戏输出。私有 guide 单独修改未解决，因此最终保留已实测有效的组合，不再拆成猜测性实验。

正式实现：启动仅判断一次 `ds.exe`，缓存兼容策略；NR 开启时将三种输入复制到既有可复用捕获槽，保持原图只读；输出直接绑定游戏自身 UAV。其他游戏继续现有直接输入 / 中间输出路线，不承担死亡搁浅的额外输入复制。NR 关闭时不启用这条特殊模型路径。

build-only 的 thread_local route scope 已移除，改为 `PassFrame::resolveTarget` 强引用。进入 pass 前检查纹理形态、设备、尺寸、格式、UAV 写入标志，以及不与任何输入资源相同；capture 要求原输出处于 UAV。保持已验证的 UAV → SRV → UAV 完成屏障，返回后不对原输出进行自复制。捕获槽、recording 持有输出；真实 fence 完成或确认未提交后才释放，在复用、准备失败、Discard 中也清空。

转换后的 guide 由 recording 强引用，避免尺寸 / 格式变化替换 scratch 时释放仍在 GPU 使用的旧 guide；复用条件增加格式核对。引用在 ticket 完成后释放，不增加资源分配或 GPU 等待。模型数学、颜色 shader、R32 depth 转换、MV 编码 / scale 和模型参数保持原有正式实现。

主要位置：[src/runtime.cpp](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/src/runtime.cpp:74)；[src/nr/frame_input.hpp](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/src/nr/frame_input.hpp:42)；[src/nr/capture12.cpp](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/src/nr/capture12.cpp:370)；[src/nr/pass.cpp](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/src/nr/pass.cpp:364)；[src/nr/pass.cpp](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/src/nr/pass.cpp:146)。

性能影响：死亡搁浅有三路输入复制及对应副本显存；去掉一张 packed resolve scratch 和最终输出复制。净 GPU 耗时没有量化对照，不声称零成本或一定更快。删除定向诊断中的七扫描线采样、四帧 readback、一次完整图像复制 / BMP 导出、约 147 MiB 当轮图像 readback、临时导出线程和额外成功日志。这些诊断的资源和调度成本不再出现在正式版本。

## 3. RTSS / 游戏兼容修正的作用范围

战锤的修复与 GTA 小交换链规避是两件事。On12 队列兼容适用于实际加载 RTSS、设备与当前交换链设备匹配的调用，替换队列时持有 ComPtr 至原调用结束；不是给每款游戏新增一套渲染流程。首次 Present 前安装仍保留，避免 RTSS 自己加载 D3D11 后立即调用而漏 hook。

本次 `SetCurrentQueue` 在队列未变化时直接返回，不再每帧 GetDevice 与重写 COM 引用。On12 安装成功后 Present 不再反复查找 D3D11 / GetProcAddress / 安装入口。为了让缓存 detour 地址一直有效，仅安装时固定目标系统 D3D11 模块生命周期；不是每帧加载。RTSS 是否加载的轻量查询和线程同步仍保留，以支持晚加载，不声称 CPU 开销完全为零。位置：[src/hooks.cpp](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/src/hooks.cpp:38)、[src/hooks.cpp](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/src/hooks.cpp:205)、[src/hooks.cpp](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/src/hooks.cpp:224)。

GTA 小交换链排除只在 `GTA5_Enhanced.exe` 且 INI 启用时生效，不把其他游戏的小交换链一概排除。死亡搁浅的复制策略也只对 `ds.exe` 生效。龙信 2 依靠独立 REFramework 前置，不新增 DD2 名称分支、文件打开 spoof、SkipFirstFrames 或其他无证据的补丁。

## 4. DX11 先前修复与本次清理

先前检查中 6 项实际风险及 feature 错误路径已按授权修复，本次全部保留：

| 修复 | 行为与性能 / 稳定收益 |
| --- | --- |
| 窗口锁 | 游戏 WndProc 在插件锁外调用；无关消息先过滤，避免消息线程等待模型工作，保留焦点刷新。 |
| 失败初始化参数所有权 | 申请 capability 参数前要求 DestroyParameters；失败重试只销毁本插件持有的参数，销毁失败不继续申请；不 Shutdown 游戏的 NGX。 |
| 风格切换 | 分辨率 / 格式不变时保留 Context 大型工作纹理，只替换 Layer；旧录制独立持有旧 Layer 至完成。 |
| 已有 NGX 输入时的原生 depth | 帧尾仅收尾统计与候选年龄，省掉多余备份复制；保留必要的 draw / bind 追踪及清除前备份，用于输入源切换 / NGX 缺帧。 |
| 正常 busy 跳帧 | 录制前 Bridge 返回 S_FALSE 时只失效历史，保留光流纹理与噪声；录制后错误仍完整 Reset。 |
| shader 编译 | 本插件 14 个 shader entry 离线 fxc 编译，保持算法和优化 / IEEE 选项；官方 ImGui DX11 后端首次编译不在这项移除范围。 |
| feature 退役 | Pending / Signal 失败不提前释放；确认 Discarded 或完成后释放一次；Release 失败仍保留简短错误日志。 |

本次移除 24 个源文件中 48 个 `LITE_TEST_HOOKS` / `LITE_INPUT_TESTS` 条件块，共 320 行（包含条件指令和空白），保留正式构建原本选择的 else 代码。移除测试导出、假模型 / 参数注入、测试设备挂接、计数、抓图、面板测试选择、测试等待接口。随后清掉测试接口失去消费者后留下的 ArchitectureStatus 大快照 / 更新锁、无等待者的 idle 通知、恒为 0 的 TabFlags、External 测试输入枚举、无调用者的公开 Drain 包装。Bridge 内部析构 Drain、超时隔离保护、配置写入去重与 worker 同步全部保留。

DX11 不包含 DX12 游戏捕获、GTA 或死亡搁浅专项路径。内部 D3D12 NR 模型、共享资源、队列、fence、生成 MV、深度和预览是 DX11 功能的必要后端，不能因命名中包含 12 / diagnostics 删除。当前桥接是 D3D11 / D3D12 共享纹理与 fence，不是接入 DX12 游戏的框架。

源码复核记录：`diagnostics/2026-10-13/cleanup/final-source-audit.json`；全部变更 diff 在 `cleanup-run-evidence.zip/final-source.diff`。历史检查详情：[CODE-REVIEW-2026-10-12.md](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX11/CODE-REVIEW-2026-10-12.md)。

## 5. 模型计时、输入与保留的正常功能

Completed NR GPU 使用当前 Context 和 Layer 同时匹配的已完成时间戳；没有把旧风格计时累加到新风格。GetTimestampFrequency 只是时间戳换算，不是 GPU 核心频率监测。正常面板计时、曝光 / 白点按功能需求读回、Resources 预览、失败状态和 fence 保护保留。

模型 preset 是创建时的渲染提示，与游戏 SR / RR preset 和 Style 分开；Default / 0 交由模型决定，保持 0 即可。选择器是可选控制，本地 ref 没有给出 1 / 2 / 3 的可靠质量 / 速度排序，不凭名称推断。没有把切换 preset 或 Style 的功能作为测试代码删除。

临时 GPU 负载、频率、功耗、温度、显存预算、成功 create/release 累计统计及焦点锁等待日志此前已清掉；本次再次搜索源代码、核查最终 DLL，没有恢复 NVML / nvidia-smi 或跑分工具。用户已删除的 NRBenchDX12 没有重建。Opt 同样增长是对照现象，既不能证明插件完全无关，也不能证明 NVIDIA 模型泄漏；以往非同期功率上限观察不能当作这轮根因。

## 6. 构建与静态核验

- 两工程 Release 构建、链接与安装成功；两工程 Debug fresh 全量构建成功；完成日志中没有 MSVC warning / error。Debug 最初在沙箱中遇到 C1902 PDB 管理器错误，在正常 Windows 工具环境重试成功，没有为此修改源码或编译选项。
- 逐文件比较清理前快照与当前源码：本次 DX12 改动 8 个源 / 头文件，DX11 改动 26 个；源码中无此次测试宏、TLS 诊断 route、扫描线采样 / 图像导出 / 监测残留。所有 HLSL 文件逐字相同，shader 算法未改。
- 最终主体均为 x64；两份主体都仅导出 Start、LiteAttachFactory、LiteNrCreate、LiteNrEvaluate、LiteNrRelease；无测试导出或采样字符串。DX12 d3d12.dll / D3DCOMPILER_47.dll 位于 delay imports。
- 人工核对两条输出路线、UAV / SRV 屏障、资源别名限制、输出强引用及实际 fence 退役；保持已有命令状态 / 提交保护，没有新 CPU fence wait、线程或热路径图像读回。
- 本次没有启动游戏、GPU / 窗口检查或性能跑分。此前 CPU 检查用于空 PSO、创建列表状态、feature 退役、同步失败保护等，其程序与日志已归档并退出构建；这些检查不能替代实际 GPU 画面验证。

| 工程 | Release 主体 SHA256 | Release 代理 SHA256 |
| --- | --- | --- |
| DX12 | `169CAE370EEA5E1A87E8C65957A945FB665F7FEDBB9D17A24839CAE5C877D101` | `11076F50635B213BD3C04DBC5F07B24D4D43622A4A3F9CB1F71059A4F8CFC3CD` |
| DX11 | `24EEBC13A2CD3A695E259712E210355ED1F514E905D8C1B28137D7126CBD4407` | `1164C025F6EAA1A8C84E38C58E5EA89FC4AF6D189BB29C2C8D3212588C35DCF0` |

统一安装源为各工程 `dist/Release`。Debug 仅留作正常开发构建，不向游戏部署。最终清理版的实测来源是之前用户验收的对应定向修复，**这次归并后的新 DLL 未让 20 款游戏逐一重新运行**，不能把 hash 核验写成全游戏画面 / 性能验收。

## 7. 全部游戏部署结果

20 个登记目录全部更新并独立核验：DX12 16 个、DX11 4 个。部署前备份现有代理、主体、INI；部署后逐项核对两个 DLL 与最终 Release SHA256 相同，原 INI、NR 模型、原生 SR / RR / FG DLL、存在的 dinput8.dll 与 REF 配置保持原 hash。未覆盖个人参数，未替换已有模型，未启动游戏或结束用户进程。

| API | 游戏 | 实际目录 | 本次复制核验 |
| --- | --- | --- | --- |
| DX12 | 绝地：幸存者 | `G:\EAGames\Jedi Survivor\SwGame\Binaries\Win64` | 已核验 |
| DX12 | GTA V Enhanced | `G:\SteamLibrary\steamapps\common\Grand Theft Auto V Enhanced` | 已核验 |
| DX12 | 卧龙 | `G:\XboxGames\Wo Long- Fallen Dynasty\Content` | 已核验 |
| DX12 | 怪物猎人：荒野 | `F:\SteamLibrary\steamapps\common\MonsterHunterWilds` | 已核验 |
| DX12 | 浪人崛起 | `G:\SteamLibrary\steamapps\common\Ronin` | 已核验 |
| DX12 | 看门狗：军团 | `G:\UBISoft\Watch Dogs Legion\bin` | 已核验 |
| DX12 | 极限竞速：地平线 6 | `G:\XboxGames\Forza Horizon 6\Content` | 已核验 |
| DX12 | 天国：拯救 II | `G:\XboxGames\Kingdom Come- Deliverance II\Content` | 已核验 |
| DX12 | 星空 | `G:\XboxGames\Starfield\Content` | 已核验 |
| DX12 | 心灵杀手 2 | `G:\EpicGames\AlanWake2` | 已核验 |
| DX12 | 神秘海域：盗贼传奇合集 | `G:\SteamLibrary\steamapps\common\Uncharted Legacy of Thieves Collection` | 已核验 |
| DX12 | 龙之信条 2 | `F:\SteamLibrary\steamapps\common\Dragons Dogma 2` | 已核验 |
| DX12 | 对马岛 | `F:\SteamLibrary\steamapps\common\Ghost of Tsushima DIRECTOR'S CUT` | 已核验 |
| DX12 | 仁王 3 | `F:\SteamLibrary\steamapps\common\Nioh3` | 已核验 |
| DX12 | 死亡搁浅导演剪辑版 | `F:\XboxGames\DSDC\Content` | 已核验 |
| DX12 | 战锤：星际战士 2 | `F:\XboxGames\Warhammer 40,000- Space Marine 2\Content\client_pc\root\bin\pc` | 已核验 |
| DX11 | 博德之门 3 | `F:\SteamLibrary\steamapps\common\Baldurs Gate 3\bin` | 已核验 |
| DX11 | Rise of Eros | `H:\Games\RiseOfEros` | 已核验 |
| DX11 | 幽灵行动：荒野 | `G:\UBISoft\Tom Clancy's Ghost Recon Wildlands` | 已核验 |
| DX11 | 骑马与砍杀 II | `F:\SteamLibrary\steamapps\common\Mount & Blade II Bannerlord\bin\Win64_Shipping_Client` | 已核验 |

逐文件证据：[all-games-deployment.json](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/diagnostics/2026-10-13/cleanup/all-games-deployment.json)。其中 before / expected / retained / verifiedAt 分别记录旧 DLL、最终 DLL、保留文件和完成时间；不是手工填写的“应该部署”。本次全部游戏使用正式主体，不再保留定向诊断版本作为活动插件。

龙信 2 的 REFramework nightly 01432（commit d052e8fd3fd41d0a1a33394fcf6e6c19be39b1ee）保留，dinput8.dll SHA256 `70A2A3C24030D8BC3695554E7F929FD1B0B4B757FB5BA2C9D34F42D7155005AC`。菜单 Delete，本插件 Ins；默认不展示 REF 菜单。非 VR 只部署官方 DLL，没有增加 Lua / C# / VR 可选组件。用户整理后的 `E:\Mod\REMod\REFramework\v26_10` 保持不动。官方包校验与独立源码配置核对证据在 `diagnostics/2026-10-13/dd2-reframework/`。

## 8. 文件清理与归档

| 清理对象 | 处理结果 |
| --- | --- |
| DX12 build 临时目录与散文件 | 376 个文件，原始约 187.2 MiB，压缩归档约 58.3 MiB；逐文件 SHA256 验证后删除原位置。 |
| DX11 build 临时目录与散文件 | 101 个文件，原始约 18.8 MiB，压缩归档约 5.3 MiB；逐文件 SHA256 验证后删除原位置。 |
| build 最终布局 | 两工程均只留 windows-release、windows-debug；不留临时 cpp / rsp / cmd / DLL、CPU 工具、重复构建或回退目录。 |
| 死亡搁浅游戏目录 | 两份 body 临时 DLL 与 load-only、neutral-loadonly 两份日志，共 4 项，归档校验后移除。正式 dxgi / 主体 / INI / 模型保留。 |
| 本次清理 / 部署工具与日志 | 执行工具、变更 diff、编译与逐游戏复制日志统一收进 cleanup-run-evidence.zip，不进入 src、CMake 或游戏目录。 |
| 需要保留的证据 | 清理前源码 ZIP、旧 build 证据 ZIP、部署前游戏 DLL / INI ZIP、结构化清单，统一放 diagnostics；旧报告中的 build 路径可按相对路径在对应 ZIP 找到。 |

`build/windows-release` / `windows-debug` 内仍有 CMake 缓存、Ninja 文件、obj / lib、嵌入 shader、DLL 和 Debug PDB，这是可重复 / 增量编译的正常内容。它们不是游戏要加载的文件；发布安装源在 `dist/Release`。没有把第三方库、原项目、ref、模型或用户 Mod 仓库当成测试垃圾删除。

清理前源代码和游戏 DLL 可回溯；本次没有把“彻底清空 build”当作性能优化。477 个诊断 / 回退文件已经退出工作构建目录，正常构建只引用正式 src 和生成的 shader。

## 9. 未解决与仍需覆盖的实际问题

1. **骑马砍杀游戏 DLSS 捕获仍缺失。** C 入口覆盖不足以证明覆盖其真实执行入口，需要进一步确认实际 NGX 调用模块 / 函数 / 路径；缺少本地 nvngx_dlss.dll 不构成判断依据。本次清理没有伪装其 Active 状态，也没有继续补入 SR DLL。
2. **Jedi / Opt 多次模型切换后同模型耗时增长根因未证实。** 已避免旧计时串入、无意义大纹理重建及监测成本；没有证据保证这种持续增长消失。
3. **已关闭命令列表在退役后再次 Execute 的边缘风险仍在。** 精确 fence 判定的旧改法导致严重 busy / 等待回归，按用户要求未恢复；本次不改 ledger 的提交 / 回收策略。有限重放观察阴性不能证明游戏永远不重放；未来应单独验证 post-hook 再 Signal 并延长保留，或参考延迟回收方案，不能把等待回归重新带回清理版本。
4. **死亡搁浅保持 NR 开启直接退出、对马岛输入域判别、Jedi 初次点击问题**没有完整独立验收，报告不写成全部彻底解决。正常用户场景已通过的部分见第 1 节。
5. Signal / 设备失败时隔离 GPU 仍可能使用的资源是故障保护；不能为消除看起来像“保留”的代码而提前释放。ReleaseFeature 失败也不能凭忽略返回值写成成功，无有效 dump 的 RADAR 记录不证明本插件泄漏。

## 10. 失败方案、纠正与框架判断

死亡搁浅逐项关 NR 应用、查看 Model input / Raw output、depth / MV 等曾让用户多次操作，定位效率不够；后续改成一次运行自动采样并核对 ref 输出链路。alpha=1、同族 depth、私有 guide 单项尝试都未解决并已撤回；中心少量像素或 nonfinite=0 不能排除半幅空间损坏。完整图像和最终用户确认支持有效组合修复，不能反过来把那些失败方案记为成功。

战锤先前禁用 On12 / 仅依赖 Loader 发现的处理没解决，实际 dump 才确认 RTSS 首次 Present 时机与队列问题。当前保留这个时序修复。龙信 2 最初只查本地 Quirks / Kernel 注释，漏掉官方 Wiki 的 REFramework 安装前置；升级主体与关闭 hook 无效，补前置后才通过。报告保留这些判断失误，不把安装前置包装成插件内部算法修复。

框架现在的职责边界是：代理做系统转发和启动；hooks 做入口 / 队列 / 列表观察；capture 持有帧资源与提交 ticket；pass 管理模型 Layer、工作纹理与每次 recording；面板消费状态与按需预览。DX11 通过自己的共享资源桥接后端运行模型，DX12 观察游戏本身的 SR / RR 输入。把直接输出放进明确的 owned frame contract，比 TLS 临时侧信道清楚；保留 fence 生命周期，比为“精简”删同步更稳。

仍有固定槽 / stream 上限、核心共享状态与互斥、单运行时设备等设计约束。它们是必须结合游戏验证的边界，不以代码行数多少宣称最优。当前收尾选择保留已确认行为、缩小游戏特殊路径、减少确定的多余工作；没有承诺所有游戏性能无损或所有历史故障消失。

参考来源：本地 `_ref/OptiScaler_DLSSNR/OptiScaler/shaders/dlssnr/DlssNr_Dx12.cpp`（hdrCopy / target resolve）、`hooks/D3D11_Hooks.cpp`（RTSS 队列修正）、`hooks/D3D12_Hooks.cpp`（状态恢复）；[Opt 龙信 2 兼容页](https://github.com/optiscaler/OptiScaler/wiki/Dragons-Dogma-2)；[官方 REFramework 对应 release](https://github.com/praydog/REFramework-nightly/releases/tag/nightly-01432-d052e8fd3fd41d0a1a33394fcf6e6c19be39b1ee)。没有修改参考工程。
