# 2026-10-13 启动与 DLSS 捕获问题

## 已有证据

| 游戏 | 用户实测与日志 | 当前结论 |
| --- | --- | --- |
| 死亡搁浅导演剪辑版 | 原版和 General.Enabled=0 均退出；移开代理正常；代理存在而主体缺失正常；只加载主体、不调用 Start 仍退出，日志确认 LoadLibrary 成功；只改主体文件名仍退出；延迟三个依赖后正常；只延迟 D3D12、保留 DirectInput/compiler 正常加载也正常；正式 Start 恢复后用户确认菜单/面板正常，实际场景 NR 一直 waiting | 启动修正已获用户确认；日志的后续阻断点为 `DX12 waiting for a tracked command list Reset`，并非未捕获 DLSS。Reset 未捕获的具体原因尚待诊断 |
| Space Marine 2 | Windows 记录 AppHangTransient；NR 默认关闭；用户回复选项 1，按“关闭插件总开关后可进菜单”理解，日志确认 enabled=0；只禁用 On12 hook 仍崩溃，日志存在 E_NOINTERFACE 的 Unsupported swapchain | On12 隔离未解决。查到 DX12 分支对非 DX12 交换链也包装的明确边界错误；不能仅凭错误日志断言它是退出原因 |
| Alan Wake 2 | 用户确认 DLSS 光线重建开启；原 observer 仅登记 feature 1；部署 RR/C 捕获修正后用户确认可以识别 DLSS | RR 捕获修正获用户确认，不再更新该游戏 |
| 骑马砍杀 2 | 用户确认实际场景仍无 DLSS Active，且为 DX11；EvaluateFeature_C 修正部署后仍不行，日志依旧有原生深度等待 | 已核验部署的是 DX11 主体；C 入口修正尚不足以解决该游戏。暂未确定是 lookup、Create 登记还是 Evaluate 漏捕获 |

## 正式修改

- DX12 `CMakeLists.txt`：主体链接 `delayimp`，延迟 `d3d12.dll`、`D3DCOMPILER_47.dll`。参考 `_ref/OptiScaler_DLSSNR/OptiScaler/OptiScaler.vcxproj` 的 Release `DelayLoadDLLs`。不延迟 DirectInput，也不改代理启动机制。
- DX12 `src/nr/ngx_observer.cpp`：接受 SR feature 1 和 RR feature 13；RR 资源允许 common key 缺失/为空时回退到 `DLSSD.Output`、`DLSSD.Depth`、`DLSSD.MotionVectors`。仍经过原 guide、状态及资源验证；不捕获 NR feature 18。
- DX11/DX12 observer：覆盖 `EvaluateFeature_C`，普通与 C 入口分别保存原函数、使用各自 thunk。C 参数保持 const，原回调直接交还对应原函数；共享重入保护避免 C 入口转调普通入口时重复捕获。
- 未改图像算法、shader、UI、ledger、配置保存及 GPU 计时。未自动启动游戏。

## 检查与部署

两套项目独立目录的 Release 全量编译通过，正式 Build.ps1 构建也通过，无 warning/error。新 DX12 主体 PE 普通依赖中不再列出 D3D12/compiler，二者列在 delay-load 依赖中。

build-only CPU 检查实际包含 observer 源文件，验证 C/普通入口独立原函数及真实不同签名的进度回调、原返回值、const 参数、嵌套去重、feature 过滤、释放与 bypass；DX12 另验 RR/null common key 的 alias 回退。两套检查通过，未创建 GPU 设备/窗口、未加载 NVIDIA runtime。不纳入项目 CMake 或正式导出。

首次从新目录配置时，MSVC Debug ABI 探测报 C1902（PDB manager mismatch）；指定 Release 的 try-compile 后，全量编译成功。这是本次构建环境的探测问题，没有证据表明它是已部署 Release 的游戏退出原因。

正式主体 SHA256：

```
DX12 DF7D9B562AC477A9EFAC0993E14479AA94F793B41F09BA8F85B3E73923311C22
DX11 28D841BAE6FFD983387759FA353410DDF0BF03FCD42924274232EA8A839F2E5B
```

仅更新死亡搁浅、Alan Wake 2、骑马砍杀。死亡搁浅恢复原正式代理与原 INI（General=1、NR=0）；Alan Wake 2/骑马砍杀现有 INI、NR 模型 SHA256 不变。其他游戏未更新。

Space Marine 2 当前为 build-only `sm2-no-on12` 诊断主体（`F15F2709D6EA8294895CC4D73B24C89461A548D3CD6C5CFCFD68D95058270057`），恢复原 INI，总开关为 1、NR 为 0。该诊断版不属于 dist/Release。

核验：`build/affected-games-20261013-verified.json`。死亡搁浅原主体保存为 `build/dsdc-original-body.dll`；诊断期间遗留的中性名 DLL 在游戏验证完成后再清理。

## ref 与兼容开关

Opt 的 `misc/Quirks.h` 将 ds.exe 列为 DisableDxgiSpoofing、DisableFSR2Inputs、DisableFSR3Inputs。前者指硬件信息伪装，不等于关闭 DXGI 代理/所有 hook；本插件未实现这类伪装和 FSR 输入，不能把该条当作本次装载故障的直接修复。

现有 On12 hook 为通用路径，队列替换有 RTSS module、设备及队列差异条件，非 GTA 专属；hook installed 日志本身不证明发生过队列替换。GtaSkipSmallSwapchains 是另一个只对 GTA5_Enhanced.exe 且开关开启时生效的分支。

## 后续交换链边界修正及有限诊断

DX12 `hooks.cpp` 先从创建参数 QueryInterface 出 ID3D12CommandQueue，失败时保留原交换链且不查询/替换它。`Swapchain` 构造函数直接保存已经验证的类型。SwapChain4 同样可以属于 DX11，不能将支持 SwapChain4 当作 DX12 的判据。对照 ref `hooks/DxgiFactory_WrappedCalls.cpp` 的队列判别分支和本工程 DX11 的设备判别；不添加 DX11 接入路线。

正式 Release 编译通过；build-only CPU 检查实际包含 hooks.cpp，验证 null/非 DX12 设备不触碰原交换链、DX12 队列进入接口检查、引用计数平衡。没有创建 GPU/窗口。正式 DX12 主体 SHA256：`1DFDED619D240EFAEDB1FC14692095E9ABAEB07E4103A671EEF85763EB421E51`。

当前针对三款游戏的部署（只更换主体，原 INI、模型、代理 SHA256 不变）：

| 游戏 | 当前主体 | 用途 |
| --- | --- | --- |
| Space Marine 2 | 正式 DX12 `1DFDED619D240EFAEDB1FC14692095E9ABAEB07E4103A671EEF85763EB421E51` | 恢复正常 On12，包含延迟依赖加载与交换链边界修正，等待启动反馈 |
| 死亡搁浅 | 临时 `7EFEEAFE05CCA75D7DD255B1D11E6E8672C4D068E2DD4313347EFA9C017FFDEF` | 同正式功能，额外记录有限 Reset/丢失状态诊断；未绕过绑定保护 |
| 骑马砍杀 | 临时 DX11 `4B6EC3028E6A304BC84A47DDB2556D47E4BA54F778B9E08AEEF0C3A7064C9607` | 同正式功能，额外记录有限 NGX lookup/create/首个 evaluate；未猜测未登记 feature 类型 |

临时诊断由 `build/prepare-game-capture-diagnostics.ps1` 生成，源代码及对象仅在 build 中，不属于 CMake、dist/Release 或正式导出。上限为死亡搁浅 6 次 Reset + 4 次缺失状态、骑马砍杀 48 次相关 lookup + 24 次 create/首个 evaluate，不逐帧写日志。诊断骑马砍杀 observer 的 CPU 回归检查同样通过。临时版定位后应恢复正式主体。

本轮备份与核验：`build/before-capture-diagnostics-20261013/`、`build/active-game-diagnostics-20261013.json`。此前“当前部署”段落保留历史记录，当前状态以本节为准。成功编译不等于完整游戏稳定性验证。

## 对照 ref 后的当前部署与证据修正

死亡搁浅诊断确认：Reset hook 有执行，但被拒绝的列表为新地址，已有同 epoch 的绑定状态而没有 Reset 基线。正式命令状态捕获补上 CreateCommandList 返回时的初始开放录制；CreateCommandList1 的关闭列表仍须 Reset，NR 关闭、内部暂停捕获及旧 epoch 均不放宽。未改 ledger 或 shader。

Space Marine 2 的两次 minidump 均为 d3d11.dll+0xAD36E 的空指针读取。RTSSHooks64 存在于进程中，原始栈内有其地址，但这些候选地址不等于已经展开的调用栈，也不证明 RTSS 是根因。对照 ref 的 factory caller 判别，正式主体与代理补上 D3D/系统 DXGI/RTSS 内部 factory 调用保持原生对象的边界。游戏稳定性尚待用户复测。

两款游戏已部署正式 DX12 主体 `118D56B5C868F770598E5266DA7F1EC14D2E5BEFED048A19DF09ECFADA5048EF` 和代理 `11076F50635B213BD3C04DBC5F07B24D4D43622A4A3F9CB1F71059A4F8CFC3CD`，原 INI、NR 模型未变。Release 构建以及 build-only 的初始列表、SDK vtable 槽、factory 来源与交换链边界 CPU 检查通过；未运行游戏或 GPU 测试。

骑马砍杀诊断只证明 NGX 导出 lookup 被捕获，未记录到 Create/Evaluate 的执行。不能据此断定游戏未使用 DLSS，更不能据游戏目录没有 nvngx_dlss.dll 断定 SR 依赖缺失。曾临时补入 workspace 的 SR DLL，用户指出原环境已有 DLSS 后，已核对 SHA256 并移除该新增文件。已恢复正式 DX11 主体 `28D841BAE6FFD983387759FA353410DDF0BF03FCD42924274232EA8A839F2E5B`，原代理、INI、NR 模型未改；捕获问题尚未解决。

对马岛显示 Display input：本工程依据已捕获的 NGX create flags 与颜色资源格式判别 sceneLinear。非 sceneLinear 路径固定 divisor=1，shader 编码与合成绕过白点，Trim 因而无效。ref 的 CPU ResolveWhitePoint 注释声称保留手动 slider，但实际 shader 的 passthrough encode 直接复制、resolve 的 normScale 同样固定为 1；不能仅凭该注释声称 ref 的曝光 slider 在此路径有效。当前游戏日志未包含实际 flags/颜色格式，尚不能确认对马岛判别是否正确。此轮未修改其图像处理或部署。

本轮核验：`build/ref-followup-20261013-verified.json`；备份：`build/before-ref-followup-20261013/`。上述当前状态取代前文的临时部署状态。

## 最新：战锤启动实测通过，死亡搁浅闪烁仍在定位

战锤 01:55:47 PID 34468 dump 经离线 StackWalk64 展开：d3d11 的 D3D11CreateDeviceForD3D12 / D3D11On12CreateDevice ← RTSSHooks64 ← 本插件原生 Present 转发 ← 游戏。d3d11.dll+0xAD36E 读取 RTSS 传入队列的无效 vtable。本轮对应进程原日志没有 On12 hook installed，说明仅依赖 Loader 发现可能错过首次 Present 内的 D3D11 装载。

对照 ref `hooks/D3D11_Hooks.cpp` 的 RTSS 队列修正，正式 hooks.cpp 在有 RTSS 且已捕获 DX12 队列时，于原生 Present/Present1 之前确保系统 D3D11 的 On12 hook 已安装。替换队列通过本次调用的 ComPtr 持有，避免离开全局队列锁后失去所有权。正式 Release 与实际 hooks.cpp 的 CPU 转发、延迟发现及引用计数检查通过。没有新 GPU 工作或 GPU 等待。

战锤当前主体 `1917F24EA55E1901ACFAF520976733C0FC6BB523BB41354633BD62F791FF4D7A`、代理 `11076F50635B213BD3C04DBC5F07B24D4D43622A4A3F9CB1F71059A4F8CFC3CD`；日志出现实际 On12 queue replaced。用户本轮确认“已经可以玩了”，这是启动/进入游戏实测结果，不能据此声称全部 NR 行为验证通过。之后不再更换战锤 DLL。

死亡搁浅用户报告此前 118D56B5 版本开 NR 马赛克、疯狂闪烁和退出卡死；先回退到 1DFDED61/677976A7 并保留 NR 关闭。Windows 本轮报告为 MoAppHang / AppHangXProcB1，留下 Report.wer，但对应 WER dump 已被系统删除，不能凭报告宣称已定位卡死堆栈。失败二进制、日志及 WER 信息保存在 `build/dsdc-flashing-20261013/`。

ledger 原先直接按 graphics-list 接口地址匹配 Capture 与 Execute；现改为 canonical IUnknown，Reset 同样匹配，cookie 析构直接使用保存的身份，避免 QueryInterface 正在析构的 native object。CPU 检查实际 capture12.cpp，覆盖不同 graphics 接口共享身份、pre/post-submit、Reset、cookie 销毁及 Signal 失败不退休。这证明一般接口匹配问题已修，尚未证明死亡搁浅实际使用了不同接口地址。没有改变槽数、重复执行列表处理或 fence 退休规则。

死亡搁浅最新为 build-only 诊断主体 `B0554B12CFA8E69BF1B1D11088D40F72AD9587BFBBDFF58CC3241D17DF7B9543`、代理 `11076F50635B213BD3C04DBC5F07B24D4D43622A4A3F9CB1F71059A4F8CFC3CD`。包含当前正式兼容修正，额外有限记录：6 次 NGX 输入（含三种资源）、8 次 capture、16 次 NR Record、12 次 prepare/Signal、8 次 pending discard、6 次 busy。达到上限后停止相应日志和诊断资源查询；不添加 GPU 频率/负载监测、GPU 等待或 shader/UI 变更。诊断源和链接输出仅在 build，不加入正式 CMake/dist。

原 INI（NR 仍关闭）、模型和游戏 SR DLL 的哈希保持原样。仅死亡搁浅两个插件 DLL 更新，其他游戏未刷新。部署核验 `build/dsdc-ledger-diagnostic-deployed-20261013.json`，可回退到 `build/before-dsdc-ledger-diagnostic-20261013/`。等待用户场景中短暂开启 NR 后的日志；闪烁及退出卡死尚未确认修复。本节取代前文“当前部署”。

### 12:39 场景日志与显示隔离

用户完成测试，仍马赛克闪烁，关 NR 恢复。有限记录显示第一轮 Create 成功，随后 15 个 Record 均 edited/ready=1、NGX/model=1；12 个队列 Signal 均成功，fence 从 0 向后推进，记录时 GetDeviceRemovedReason 均 S_OK。记录范围内没有 busy/discard。Capture 与 Execute 的 graphics 接口地址实际相同；它们与 canonical IUnknown 相差 0x140，但这不证明旧接口地址匹配会漏掉本游戏的提交。当前 canonical 修正不能认定已解决本次闪烁。

本场景颜色为 R10G10B10A2_UNORM (24)、3840×2160，深度 R32G8X24_TYPELESS (19)、2560×1440，MV R16G16_FLOAT (34)、2560×1440；create/evaluate 的 flags 均 0x2B，MV scale=-1280,+720，depth inverted，preExposure=1，无 exposure texture。尺寸与传递参数已得到日志实证，但成功返回不验证 GPU 结果图像。日志和当时 INI 保存于 `build/dsdc-scene-20261013/`。

用户继续实测：保持 Neural rendering 开启，取消 Apply NR result 后不再闪烁。该开关保留模型执行、状态恢复和合成 Copy，只让 resolve 输出原图，因此故障重点缩小到 NR 输出/合成，而非根据闪烁就猜测队列忙或游戏恢复状态坏。等待 Model input / Raw model output 显示对照；退出卡死本轮未得到明确新结论。

用户随后确认 Model input 正常、Raw model output 闪烁。这将坏图像定位到模型输出（或其输入契约），最终 Compose/白点恢复不是形成本次马赛克的必需条件。

对照 ref shader 的 feature 18 opaque colour input 注释，发现本工程 linear HDR proxy 强制 alpha=1，而 display 分支继承原游戏的 alpha；ref 的 display passthrough 分支也保留 alpha，因此不能称它为 ref 已有的完整修复。本轮制作只将 display NR proxy alpha 固定为 1 的 build-only 对照版，RGB、最终游戏 alpha、模型参数、guides、生命周期均不改，也没有增加 dispatch、GPU wait 或统计。这个输入契约差异是待验证假设，尚未测得游戏原 alpha 或证明它就是根因。

fxc /O3 与 pass.cpp /W4 /WX 编译链接通过；DLL 中核对改后 shader 唯一存在、原 shader 不存在，避免静态库旧对象混入。只更新死亡搁浅主体到 `DFBD1E77553598405E38E60D918749378AE4059B82F2A102A2581A89BB164606`；代理、用户当前 INI（其自行调整的 NR/Apply/Debug 值不覆盖）、模型和 SR DLL 哈希保持原样。源码/dist 未改，其他游戏不动。核验 `build/dsdc-opaque-proxy-deployed-20261013.json`，回退 `build/before-dsdc-opaque-proxy-20261013/`，等待同一场景 Raw model output / 常规画面结果。仍未宣布死亡搁浅修复。
