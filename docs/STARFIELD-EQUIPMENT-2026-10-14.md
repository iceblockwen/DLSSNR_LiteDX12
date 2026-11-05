# 星空装备界面切回后 NR waiting

**本方案已被用户验证无效，且反馈一直 waiting；已撤回。** 后续只给星空按参考处理当前纹理尺寸，[最新处理报告](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/docs/STARFIELD-TEXTURE-2026-10-14.md)。下方保留失败方案与其验证边界。

2026-10-14。用户确认进入装备界面再切回即可触发。最新日志会话为 02:56:04，明确记录 allocation 3840×2160、declared 2560×1440、origin 0,0，随后因 E_NOTIMPL 跳过 NR。此前失败没有这些数字。

## 已确认与尚未确认

已确认当前输出分配与插件传入的声明不一致。ngx_observer 原来只在 CreateFeature 缓存 OutWidth/OutHeight，Evaluate 继续用缓存判定颜色区域和高分辨率 MV 范围。没有读取当前 Evaluate 参数中的输出声明，因此无法处理声明在视图切换时更新的情况。

参考核对：OptiScaler 的 DlssNr_Dx12.cpp 以当前 output texture GetDesc 得到工作尺寸；本工程有额外的部分输出保护。DLSSNR_Lite 的旧捕获同样依赖创建声明。此次选择修正每帧声明来源，保留安全保护，没有直接将分配尺寸认定为有效区域。

**旧日志没有当前 Evaluate 的 OutWidth/OutHeight，不能证明修复后星空一定会提供 3840×2160。** 如果当前参数仍为 2560×1440，或缺失且只能回退到旧声明，保护仍会拒绝；这时仍需研究游戏真实输出范围。未将用户触发条件等同于已证明的具体引擎内部行为。

## 改动

仅修改 DX12 的 guide_contract.hpp 和 ngx_observer.cpp：每帧优先使用当前 Evaluate 的非零 OutWidth/OutHeight，缺失或为零回退到 feature 创建声明。颜色区域和高分辨率 MV 使用同一组输出尺寸；低分辨率 MV 仍使用 render 尺寸。保留偏移、部分输出和过大声明保护。

没有修改 shader、模型、UI、INI、hook 安装或 ledger。每次处理增加两次参数 Get，无新增 GPU pass、复制、读回、资源缓存或等待。已有拒绝尺寸日志保留，无新增成功帧监测。

## 验证与部署

Release / Debug 构建通过，无 compiler warning / error。生产 guide contract 的 CPU 检查覆盖同一 feature 在 1440p 与 4K 声明间切换及切回、创建缓存不变、缺失 / 零值回退、高 / 低 MV、过大输出和非零偏移仍保留。没有运行游戏或 NVIDIA 模型；实际装备界面恢复仍待用户确认。

仅星空更新主体 DLL，代理、INI 和模型按部署前 SHA256 保留；其他 19 个游戏仍为此前审查修复版，等待这项行为确认后再推广。

主体 SHA256：`34BA792BCC9C0DAB921CB27FB901C019F5F65193E99DC9F9CEB45A100936D1A8`。

[部署核验](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/diagnostics/2026-10-14/starfield-equipment/deployment.json)。复现日志、CPU 程序及构建证据归档于同目录 verification-evidence.zip，未加入正式构建。部署前主体单独归档。
