# 新版 NR preset 与怪猎荒野启动排查

日期：2026-10-17。当前阶段：两工程 A/B 菜单已完成；怪猎手动开启旧模型、保护版随启动开启 NR 均由用户确认不闪退且效果正常。新日志确认启动时缺少计算根签名的录制被保护跳过，随后继续创建 NR。之后先升级怪猎至 310.9.16.0，用户反馈 A 效果更合意但更掉帧；再按用户要求，全部 20 款登记游戏完成正式插件及新版模型部署并校验。Default 默认选择 B，REFramework 与既有配置保持原文件。全量结果见 [全部游戏部署报告](ALL-GAMES-NR-2026-10-17.md)。

## 1. 新模型与 preset 的实际对应

本地已有两份 NVIDIA 模型，数字签名均 Valid，签名主体为 NVIDIA：

| 文件 | 版本 | 字节数 | SHA256 |
| --- | --- | ---: | --- |
| 工作区根目录 nvngx_dlssnr.dll | 310.8.0.0 | 165840496 | E16BCF15E16E13F527491CDF7845B2FE6521A738D8F7C9C721866A8496E1FC8E |
| DLSSNR_Model/nvngx_dlssnr_2026_10/nvngx_dlssnr.dll | 310.9.16.0 | 266424944 | 2BC19BC09A7B89DD44F73E2AC0108FCC586E2154BDD0830F242F4F040E28789F |

新版 PE 中有两项模型描述符。静态解析与反汇编得到：

| 传给 DLSSNR.Hint.Render.Preset 的值 | 选择 |
| ---: | --- |
| 0 | DLL 默认：310.9.16.0 回退到 preset 2 |
| 1 / A | CC_Control_History_Blend_Quantize_With_Teacher_honest_tench_2026_07_04_22_30_weights |
| 2 / B | hnet-crazy-cuckoo-small-fp8 |
| 其他未提供值 | 新 DLL 的查找器回退到 preset 2 |

证据地址以新版 DLL 的首选基址 0x180000000 为准：描述符位于 0x1800DBE30 和 0x1800DC0C0，步长 0x290，preset 字段偏移 8；查找函数 0x180023C20，默认回退分支 0x180023C47 / 0x180023C73 明确使用 2。两 DLL 的 55 个导出名相同。这里只证明导出入口和参数选择关系，不把相同导出名等同于所有运行行为已经实测兼容。

原插件已经在创建 feature 前传递该参数，包括 0；SameTuning 已包含 preset，变化会退休旧 Layer 并重建历史。因此旧面板的 Preset 1 / Preset 2 就是对应的数值入口；没有 A/B 字样不代表新版 B 无法启用，Default 本身已可选择新版 B。

本轮两工程将菜单明确改成 Default (model choice) / A / B；删除未提供模型的 Preset 3 菜单项，旧配置的 3 或非法值归一到 Default。Style 的 Default / Natural / Cinematic 不变。默认 INI 保持 Preset=0，不强行将所有旧游戏配置改为 2。A 的描述符名称与原模型路线对应，但未据此宣称两 DLL 中 A 的权重逐字节相同，也未测量 B 的画质或 GPU 提速。

## 2. 怪猎证据

目录：F:\SteamLibrary\steamapps\common\MonsterHunterWilds。

- 10:37 / 10:38 的日志记录新版 310.9.16.0；10:40 的失败轮次已经回退到 310.8.0.0。因此不能把全部失败仅归因于新模型。
- 10:40:20 的 reframework_crash.dmp：异常 0xC0000005，nvwgf2umx.dll + 0x6DB90F，空指针相关读取。驱动版本 32.0.16.1742。
- 按 x64 unwind 数据展开的实际栈：驱动 +0x6DB90F → 驱动 +0x7E33F7 → 插件 +0x3899D → 游戏 +0x4C027。插件地址位于转发 SetComputeRoot32BitConstant 的原调用处，尚未进入该次调用后的 Track。驱动访问用于根常量绑定的内部状态时遇到空指针。
- 原始栈中还留有插件 +0x3ED7C 等值。它们不是这次异常的有效调用帧，不能把其附近的描述符堆 Release 当成根因。此前进度中提出的释放对象方向已撤回。
- 用原生产 DLL 的反汇编和异常展开定位入口；私有重链 map 辅助解释符号，未部署重链 DLL。dump 时间戳与备份中的原主体匹配。
- dump 显示的模块路径包含 _storage_。其主体与游戏根目录、原 dist 主体的 SHA256 均为 4CB7542E…，未发现这个副本落后的证据。本轮定向部署同步这两个副本。2026-10-18 核对 REFramework.cpp 后补充：REF 会创建并复制 DLL 至该目录，同时调用模块路径伪装；不能仅凭 dump 路径证明映射文件的真实磁盘来源。
- 现有 REF 日志是之后 10:40:59 启动的另一轮；不能把它的末尾状态混当成 10:40:20 崩溃轮的完整日志。

用户确认第一次区分测试结果：NR 关闭启动正常，进入实际场景后手动开启 NR 也正常。这轮主体仅更新 preset 菜单和非法 preset 值处理，模型仍为 310.8.0.0。新日志启动为 13:26:33，原 dump 没有更新。该现象说明启动阶段相关，但一次成功不证明唯一根因，也不证明修改菜单就修复了闪退。

## 3. 补上的绑定完整性保护

本地参考 `_ref/OptiScaler_DLSSNR/OptiScaler/shaders/dlssnr/DlssNr_Dx12.cpp` 的 Dispatch 在需要恢复根签名时检查 CanRestoreRootSignature；没有捕获就跳过。D3D12_Hooks 中只记录非空签名。参考项目也在 [v0.1.1.5 发布说明](https://github.com/Dagherbou/OptiScaler_DLSSNR/releases/tag/v0.1.1.5-dlssnr) 中说明，怪猎等依赖保留绑定状态的引擎曾因 NR 遗留计算绑定而崩溃。该说明是调查依据，不能直接证明本地崩溃与它完全相同。

我们原有 CommandStateScope 检查 epoch、Reset、valid 和 render pass，却允许 compute root signature 为空的快照进入 NR，随后会恢复空签名。现在在任何模型命令和 SetPredication 之前增加检查：没有捕获到非空 compute root signature 就跳过这次 NR，状态为 `DX12 waiting for a captured compute root signature`。后续录制有完整捕获时继续正常处理，不锁死本轮模型，也不强制用户重新开关。

这是保守的完整性保护，不是固定延迟、游戏名称补丁或忽略驱动异常。对不完整捕获的录制，NR 会等待；后续捕获完整时继续。用户已确认保护版随启动开启 NR 不闪退、效果正常。它不能补回未观察到的根签名，不能保证所有其他命令状态风险都已经消除。

验收轮日志启动时间 13:38:57、PID 29508，模型仍为 310.8.0.0。日志先出现 `DX12 waiting for a captured compute root signature`，随后出现 NR layer created，说明本轮确实触发了新增保护并继续处理，不是永久等待。原崩溃 dump 仍停留在 10:40:20。该轮实测支持这项修正有效，但没有保存崩溃时完整的绑定快照，不能证明其为所有启动失败的唯一原因；新版模型 B 的实际运行仍未在本轮验收。

性能影响为 NR 快照路径上的一次已有指针检查；没有额外 GPU pass、纹理复制、读回、fence wait、线程、每帧日志或睡眠。没有测量实际 GPU 毫秒或帧率，不声称性能提升。

## 4. 验证与部署

| 验证 | 结果 |
| --- | --- |
| DX11 / DX12 Release、Debug | 全部成功；DX12 加保护后两配置重新构建成功，日志无 MSVC warning / error |
| 模型静态解析 | 两描述符数值与默认回退核对通过；55 个导出名一致 |
| 生产 command_guard.cpp 的 CPU 假 COM 检查 | 只有 Reset、只有图形签名、空计算签名、新 epoch 未重建捕获均在修改列表前拒绝 |
| 完整签名下连续 10000 次 scope | 计算签名和常量恢复，引用计数不增长；cookie 释放清理注册表 |
| 相同 CPU 用例对照旧逻辑 | 在 Reset-only 不应放行的断言处失败；新逻辑通过 |
| 真实游戏与模型执行 | 只由用户运行。用户确认旧模型手动开启正常，保护版随启动开启 NR 不闪退、效果正常；新日志确认保护命中后继续创建 NR |

CPU 程序、源码、私有 map、日志和 dump 均位于 diagnostics，不接入 CMake / Build.ps1 / dist，也不包含真实设备创建或 GPU 检查。参考工程与 DLSSNR_Lite 未修改。

| 产物 | SHA256 |
| --- | --- |
| DX12 Release 主体，含保护 | C887BA92A296D37BE9D9DB767DA423507B17C852780DCC6F55785EC3386FE043 |
| DX11 Release 主体，仅 preset 菜单 | B93997CEB726DEC92D8F867B1842E2587AECF8251CC37F8C8C3C65C59DF848E5 |
| DX12 代理，未变 | 11076F50635B213BD3C04DBC5F07B24D4D43622A4A3F9CB1F71059A4F8CFC3CD |

最初保护版部署只更新怪猎的根目录和 _storage_ 主体，hash 与当时 dist 一致。首次临时将 NR 启动开关设为 0，用户手动打开后配置已恢复为 1；保护版部署保留该配置及全部其他设置。当时旧模型、REF、代理 hash 经部署前后核对不变。原主体 / INI 有独立备份。此后已按用户要求推广至全部 DX12 登记游戏，见全量部署报告。

## 5. 用户确认旧模型稳定后的新版模型部署

用户要求部署新 NR 模型后，仅更新怪猎根目录及 `_storage_/nvngx_dlssnr.dll` 两份模型至 310.9.16.0。两份部署后均为 266424944 字节，SHA256 与第 1 节新版一致；源文件 NVIDIA 数字签名 Valid。旧模型的两个副本 SHA256 相同，因此只保存一份核验过的回退文件，避免重复占用空间。

保持 Preset=0，由新版 DLL 默认选择 B；NR 随启动开启保持 Enabled=1。本次模型部署前后核验插件主体的两个副本、INI、代理、REF 的 SHA256 均未变。没有启动游戏，未把文件校验当作新模型实际运行验收。

部署清单与回退文件位于 `diagnostics/2026-10-17/preset-mhwilds/model-310.9.16/`，记录在 `deployment.json`。本次未修改根目录的全局旧模型源文件和其他游戏模型。
