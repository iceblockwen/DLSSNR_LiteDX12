# 星空装备内渲染：撤回特例并追踪 feature 归属

后续已取得完整证据：创建前正确的 2K 输入 / 4K 输出在原 Create 返回后交替反向，原 observer 读取时机错误导致缓存错误。已修复为调用前快照，用户确认有效，诊断清理及全部 20 个游戏部署完成。[最终根因、修复与验证报告](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/docs/STARFIELD-CREATE-METADATA-2026-10-14.md)。下方是诊断准备时的历史状态。

2026-10-14。用户补充：装备界面有界面内的独立角色渲染。此前“当前 Evaluate 的 OutWidth 优先”和“星空强制按输出纹理全幅处理”都没有足够证据解释多视图恢复；前者用户反馈一直 waiting，后者未获验收且用户明确反对依据不足的游戏特例。**两项都已从正式源码撤回。**

## ref 实际有什么

OptiScaler 的 misc/Quirks.h 有 Starfield 兼容项，涉及 FSR 输入、DXGI spoofing 和自动曝光；没有本次自加的 Starfield 颜色输出尺寸特例。DlssNr_Dx12.cpp 的当前纹理尺寸是通用 NR 行为，不能将它说成 ref 已证明星空所有输出都是全幅。自加的兼容策略及 AGENTS 中对应条目已删除。

## 2560×1440 不能直接叫输出分辨率

NVIDIA SDK 在两个调用阶段复用同名键：[GetOptimalSettings helper](https://github.com/NVIDIA/DLSS/blob/main/include/nvsdk_ngx_helpers.h) 将 Width/Height 设为用户输出分辨率，读取 OutWidth/OutHeight 为建议的渲染输入分辨率；[Create helper](https://github.com/NVIDIA/DLSS/blob/main/include/nvsdk_ngx_helpers_d3d.h) 则把 Width/Height 设为输入、OutWidth/OutHeight 设为目标输出。Evaluate helper 主要更新资源和子区域，没有保证重新写入创建尺寸。

本地 ref 的 NVNGX_Parameter.cpp 最优设置回调同样在 Quality 下计算 Width/1.5，并写入 OutWidth；IFeature::SetInitParameters 对 display/render 使用 max/min 处理。因而 OutWidth 在 Evaluate 时为 2560，并不自动表示 4K DLSS 质量的输出变成了 1440p。此前 Evaluate 读取尝试忽略了这个语义区别。

**尚未证明星空这轮 2560 的具体来路。** 旧日志只有尺寸拒绝，没有 feature 地址 / Id、创建前后参数或角色预览调用关联，无法确认它是最优设置查询残留、预览 feature 的合法声明，还是插件缓存匹配问题。

## 目前代码暴露的排查点

1. Metadata 以入口 slot + handle 地址匹配，模型缓存按 device + handle 地址。ref 的 DLSS feature 管理使用 handle 内 Id。不能仅凭地址管理的差异证明星空已发生地址 / Id 重用；需要实际记录。
2. Create 的 Metadata 在原函数返回之后读取参数。若原函数或其回调修改参数，插件可能读到调用之后的值；目前尚未证明星空发生该情况。
3. 每次 Capture 都会更新全局 LastPass，面板也读取最后一次评估的状态。副视图跳过可覆盖主视图显示状态，但 partial frameSkipped 本身不会触发全局 blocked。必须区分“显示 waiting”和“主视图再也没有有效评估”。
4. 原 ledger 同列表重复 capture 和全局槽上限仍按既有行为保留，本轮没有再改生命周期或提交规则。

## 已完成的回退与诊断准备

星空首先恢复到两次尝试前的审查修复版主体，SHA256 为 `32FFFC22C89F4AD86F4492CA6FF4CCA67365FB2AF053980EF5B1EE6F63298E63`。正式 dist/Release 保持这个版本；正式源码没有 StarfieldTextureOutput、强制全幅或当前 OutWidth 优先。正式 Release / Debug 再次构建通过。

随后仅星空部署隔离诊断主体：以基线对象文件链接，仅替换带观察记录的 ngx_observer 对象；诊断源、编译脚本和程序全部位于 diagnostics，不添加 CMake 开关，也不修改正式 observer。渲染尺寸、Capture 调用、资源归属、shader 和原函数结果保持原行为。代理、INI、模型按 hash 保留，其他 19 个游戏未更新。

诊断包含：Create 前后 Width/OutWidth 等值、feature 地址 / SDK Id、入口 slot、参数地址；Evaluate 原调用前后的输入颜色 / 输出 / depth / MV 的资源尺寸、地址、格式；本次缓存、NR 状态和按 feature 的观察次数；Release 事件。创建阶段不解引用旧参数中的资源，避免复用参数残留指针。普通资源轮换不反复写日志，状态随五秒心跳观察，维度 / feature 变化立即记录，最多 256 个事件。只有 CPU 参数观察，没有 GPU 读回、图片采样、复制或同步；临时 CPU 开销不代表正式插件开销。

诊断以 /W4 /WX 编译、链接通过，导出与延迟 D3D12 / compiler 依赖保留；假参数 CPU 检查通过，覆盖两种尺寸语义、前后观察、feature 分离、同地址不同 Id、去重和上限。未运行游戏或模型，不把模拟结果当作星空根因结论。

[诊断构建与 SHA256](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/diagnostics/2026-10-14/starfield-streams/trace-build.json)；[回退核验](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/diagnostics/2026-10-14/starfield-streams/rollback.json)；[诊断部署核验](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/diagnostics/2026-10-14/starfield-streams/trace-deployment.json)。源文件、程序、脚本和日志归档在同目录 verification-evidence.zip；正式 build 没有新增测试目录。

## 下一步所需证据

一轮“正常场景开启 NR → 进入装备 → 返回正常场景”，每段约 10 秒，随后退出。它可以一次区分主 / 副 feature、参数修改阶段、身份匹配和主视图恢复，不再先按游戏名称绕过保护。实际数据回来前不再提交猜测性的行为修复。最终问题确认解决后恢复正式 DLL，并删除诊断路径的用途，仅保留归档证据。
