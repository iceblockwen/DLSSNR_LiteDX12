# 星空：撤回无效参数改动，限定当前输出纹理兼容

**本方案依据不足，用户指出装备界面存在独立渲染；已撤回源码和部署。** 不能把参考通用纹理尺寸处理当作星空全幅保证。[最新 feature 关联排查](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/docs/STARFIELD-STREAMS-2026-10-14.md)。下方为撤回方案的历史记录。

日期：2026-10-14。用户反馈上一版更新后一直 waiting。新日志会话 03:04:48 仍反复显示 allocation 3840×2160、declared 2560×1440、origin 0,0，拒绝码 E_NOTIMPL。上一轮“读取当前 Evaluate 输出声明”没有消除差异，**不能将该故障归结为创建缓存、再假定 Evaluate 一定提供 4K 声明**。CPU 检查只证明给定参数的处理正确，无法证明游戏实际提供该参数；上一轮行为修复未通过用户验证。

## 参考与处理

重新实际读取 `_ref/OptiScaler_DLSSNR/OptiScaler/shaders/dlssnr/DlssNr_Dx12.cpp`：约 1529 行使用 target.GetDesc 的 Width/Height 作为颜色工作尺寸；约 1560 行另用 RenderSubrectWidth/Height 限定 guide 有效区域。参考未把创建 OutWidth/OutHeight 当作该颜色 pass 必须满足的全幅条件。这是参考通用行为，不是参考中存在单独 Starfield 代码的证据。

本次撤回 ngx_observer 对当前 OutWidth/OutHeight 的两次读取，保留原先创建声明路线作为一般游戏默认。增加仅在主程序文件名为 Starfield.exe 时启用的缓存兼容策略：零原点、创建声明不超出实际纹理时，以本次 NGX Output 纹理尺寸作为颜色 / 高分辨率 MV 工作尺寸。低分辨率 MV 继续用 render 尺寸，depth 继续用本帧 Render.Subrect 与原来的零值回退，不扩大 depth 的有效区域。

非零颜色偏移、超大声明保留原契约，PassExecutor 既有越界和区域拒绝没有删除。没有把这一兼容规则推广到其他游戏。这个处理依赖星空这条零原点 Output 对应完整颜色图像的假设，依据是已观察的 NGX 输出尺寸及参考的工作方式；日志本身不能证明纹理像素的有效覆盖。因此仍需用户确认实际 NR 画面和界面切回结果。

## 性能与改动范围

4 个 C++ 文件：runtime.cpp/.hpp、guide_contract.hpp、ngx_observer.cpp；AGENTS.md 记录限定范围。程序名只在 Initialize 解析一次；捕获时读取缓存 atomic bool，使用已经取得的资源尺寸。上一轮新增的两个 NGX Get 已移除。没有增加 GPU pass、资源复制、读回、同步、每帧路径查询或监测。shader、模型算法、UI、INI、代理和 ledger 未改。

这不等于断言所有场景 GPU 毫秒不变：恢复 4K NR 会执行实际模型工作；没有运行 GPU 或游戏基准。

## 验证与部署

Release、Debug 构建通过，最终日志无 compiler warning / error。生产 guide contract 的 CPU 检查覆盖：星空纹理尺寸选择、同 feature 纹理变小再变大、其他游戏保持声明、非零偏移和超大声明不被吞掉、render / depth / 低 MV 范围保留。测试仅 CPU，没有执行 NVIDIA 模型或窗口。

仅部署星空主体 DLL，逐文件 hash 核验；代理、所有现有 INI 与 nvngx_dlss*.dll 保留。其他 19 个登记游戏仍为审查修复版，未收到上一轮或本轮星空限定更新。

主体 SHA256：`2AE9DB6AE4A6962DAE38EDAAC50AC59DFCFC493B62A56615ABEBA40488C008B7`。

[部署核验](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/diagnostics/2026-10-14/starfield-texture/deployment.json)。该目录的 source-before.zip、runtime-before.zip 保存更新前状态，verification-evidence.zip 保存复现日志、构建与 CPU 检查证据。测试源文件和程序未加入正式 build。

**状态：已部署，实际正常场景与“装备界面 → 返回”仍待用户验收。尚未声称星空已修好。**
