# 星空装备界面切换导致 NR waiting：创建参数读取时机修复

日期：2026-10-14。状态：根因已有本轮游戏日志和正式代码 CPU 复现；用户已确认修复有效。后续清理完成，全部 20 个游戏已部署各自正式版并核验。[清理与全量部署报告](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/docs/CONFIRMED-CLEANUP-2026-10-14.md)。本报告替代之前两种已撤回的尺寸尝试。

## 已证实的原因

用户本轮观察：“第一次进入装备界面，出来 NR 失效，再进去出来 NR 有效”。03:41:05 启动、03:42:30 结束的诊断日志中，五次 DLSS 创建的调用前参数全部是输入 2560×1440、目标输出 3840×2160；外层 NGX 创建返回后的参数却交替正常、反向。

原 observer 在原 CreateFeature 返回后才调用 Describe，读取同一份共享参数的 Width / Height / OutWidth / OutHeight。因此它保存的不是调用者原先提交的创建尺寸，而是创建返回时可能已经改写的尺寸。错误缓存随后一直用于该 feature 的 Evaluate，正常的 4K 输出被部分输出保护拒绝。换成下一个参数正常的 feature 后，缓存重新正确，NR 就恢复。

| 外层入口 3 的 feature Id | Create tick | 调用前 输入 → 输出 | 返回后 输入 → 输出 / 原缓存 | 后续实际输出 | 日志结果 |
| --- | --- | --- | --- | --- | --- |
| 1 | 358288031 | 2560×1440 → 3840×2160 | 2560×1440 → 3840×2160 | 3840×2160 | NR ready / applied |
| 2 | 358320781 | 2560×1440 → 3840×2160 | 3840×2160 → 2560×1440 | 3840×2160 | partial output，E_NOTIMPL，waiting |
| 3 | 358328515 | 2560×1440 → 3840×2160 | 2560×1440 → 3840×2160 | 3840×2160 | NR ready / applied |
| 4 | 358335546 | 2560×1440 → 3840×2160 | 3840×2160 → 2560×1440 | 3840×2160 | partial output，E_NOTIMPL，waiting |
| 5 | 358341453 | 2560×1440 → 3840×2160 | 2560×1440 → 3840×2160 | 3840×2160 | NR ready / applied |

参数地址始终是 0000025E34526F60；各次 feature 地址与 Id 不同。内层入口 0 和外层入口 3 均观察到创建，但外层返回时的反向尺寸进入了实际评估所用缓存。此次证据没有显示同地址 / Id 重用是触发原因。普通评估中也能看到 Width=3840、OutWidth=2560，同时真实输出仍是 3840；这再次说明直接采用 Evaluate 时的同名尺寸键会读错语义。

日志证明了参数在原创建调用期间发生变化；尚未定位具体是哪一个内部函数写入，不能把最优设置回调作为已经确认的调用栈结论。NVIDIA SDK 的 [Create helper](https://github.com/NVIDIA/DLSS/blob/main/include/nvsdk_ngx_helpers_d3d.h) 和 [GetOptimalSettings helper](https://github.com/NVIDIA/DLSS/blob/main/include/nvsdk_ngx_helpers.h) 的确分别将同名尺寸键用于相反方向。这个 SDK 约定解释了为什么必须保存创建调用前的契约。

## ref 核对与最终代码

本地 OptiScaler_DLSSNR 的 upscalers/dlss/DLSSFeature_Dx12.cpp 在进入代理 CreateFeature 前执行 ProcessInitParams；inputs/NVNGX_DLSS_Dx12.cpp 区分 SR / RR 的内部创建与其他 feature 原生转发。此前已核对 IFeature::SetInitParameters 的尺寸保存以及 SDK helper。ref 的 Starfield Quirks 不含本问题的输出全幅尺寸特例。

本次仅修改 [ngx_observer.cpp](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/src/nr/ngx_observer.cpp:98)：

1. 对 SR / RR，在原 CreateFeature 调用前读取并保存 Metadata，包括四个尺寸、创建标志及 SR / RR 类型。
2. 原创建成功且返回有效 handle 后，登记这份快照。
3. 读取异常仍调用原创建；失败、空 handle、空参数、不相关 feature 均不登记。原返回值保留。

这是通用的创建调用契约修复，没有游戏名称分支。Evaluate、shader、NR 模型、队列 / fence / ledger、资源复制和输出区域保护都没有改动。之前两项猜测方案仍已撤回：不优先使用 Evaluate 的 OutWidth，也不强制星空按纹理全幅处理。

## 性能与稳定性

成功创建仍然是每个被捕获入口五次参数 Get，仅移动到调用前；没有增加每帧参数读取、GPU 工作、复制或同步。失败创建可能提前发生这五次 CPU 查询，属于 feature 创建路径。参数读取不再持有 observer mutex，原函数也不在该锁内调用；登记时沿用原来的锁和 map。

本次临时 NGX trace 没有进入正式源码、CMake 或 Build.ps1。星空已换回正式 DLL，后续运行不再记录 create-before / after 或成功路径心跳。诊断程序、源、日志、旧 DLL 统一留在 diagnostics 的 ZIP 证据归档，正式 build 没有新增测试目录。

## 验证与部署

- 同一 CPU 用例直接包含修复前 observer 和修复后正式 observer，模拟原函数改写尺寸、内层入口 0 / 外层入口 3 转发。旧版在创建尺寸检查上失败，修复版通过；不是另写一份模拟缓存实现。
- 检查 SR / RR、创建标志、普通 / `_C` Evaluate 使用创建快照、没有每帧重新读取创建尺寸、Release 清理，以及创建失败 / 空 handle / 空参数 / 其他 feature / 元数据读取异常仍保留原调用的行为。无 GPU、窗口或游戏启动。
- CPU 用例 /W4 /WX 编译通过；Release 与 Debug 构建通过。
- 星空已用正式 Release 主体替换诊断主体，SHA256：`4CB7542E5A3C8B44C3C5DB8CD01655A7D94A8CBAC385336588688911E082DE6B`。代理、INI、NR 模型、原生 DLSS 文件逐项核验保持原 hash。
- 用户验收后，16 个 DX12 游戏全部更新到本修复版，4 个 DX11 游戏重新部署其既有正式版。全量部署详情见上述清理报告。

[部署核验](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/diagnostics/2026-10-14/starfield-create/deployment.json)；[验证摘要](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/diagnostics/2026-10-14/starfield-create/verification.json)。完整日志、源、CPU 用例和构建证据在同目录 verification-evidence.zip。

实际验收：用户已回复“ok 可以了”，确认本次星空修复有效。随后按用户要求完成清理及全部游戏部署。
