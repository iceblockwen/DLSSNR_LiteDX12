# 曝光输入开关更新记录

日期：2026-10-14。DX11、DX12 同步完成。保持原来的 Game exposure 模式、正反比例、Trim 和曝光纹理优先级，新增两个独立勾选：Use pre-exposure、Use exposure scale。取消勾选的标量输入代入 1。原有 Read game's exposure texture 继续控制纹理读取。

## 公式与选择

定义 P 为选中的 pre-exposure（关闭为 1），S 为选中的 exposure scale（关闭或未提供为 1），T 为有效且已启用的游戏曝光纹理值，R 为 Trim。

| 路径 | 默认比例的白点除数 | 反比例的白点除数 |
| --- | --- | --- |
| 有效纹理已启用 | P × R / T | T × R / P |
| 标量路径 | P × R / S | S × R / P |
| 仅纹理勾选且纹理有效 | R / T | T × R |
| 三项全部关闭 | R | R |

输入颜色按上述白点除数归一化。纹理保持原来的优先级，不与 scalar exposure scale 相乘；关闭纹理选择标量路径。纹理不可用时保持原来的回退处理。只要同时取消两个新开关，回退也不会混入游戏标量。显示域输入继续旁路，实际除数为 1；Manual / Measured 模式维持原来的处理。

默认纹理路径仍在 GPU 使用当前纹理；反比例仍使用已有的完成后曝光读回。没有增加 dispatch、资源分配、GPU 同步或读回。开关不参与模型 SameTuning 比较，因此修改开关不会因此重建模型上下文。

## 配置兼容

新增 `WhiteUsePreExposure` 和 `WhiteUseExposureScale`，走原来的配置加载和持久化流程。新模板两项为 1。既有 INI 缺少新键时：pre-exposure 默认开启；exposure scale 默认与旧 WhiteUseTexture 一致，保留旧配置“关闭纹理时只用 pre-exposure”的行为。用户修改后两个新开关独立保存。

部署保留每个游戏的既有 INI，没有强行改动用户设置。

## 验证与部署

- DX11、DX12 的 Release 和 Debug 共四次构建成功，构建日志未发现 warning / error。
- CPU 校验直接链接两份新 Release 的生产 WhitePoint 函数，断言开启，编译采用 /W4 /WX：验证标量开关与正反比例组合、旧配置映射、关闭输入时忽略非法值、开启输入时的等待回退和显示域处理。全部通过，无 GPU 调用。
- 源码与修改前备份比较：每个工程只改面板、white_point.hpp/.cpp、model_settings.cpp、pass.cpp 和默认 INI。图像 shader 未修改。游戏兼容、ledger、队列和资源生命周期修复未修改。
- 16 个 DX12、4 个 DX11 登记目标全部部署，并核验主体和代理 SHA256。既有 INI、NR 模型、原生 DLSS 和已登记的 REF 文件与部署前 hash 一致。
- 本轮未启动游戏，未进行新的画面或 GPU 性能实测；不将编译、CPU 验证和文件校验等同于游戏验收。骑马砍杀未捕获 DLSS 的历史问题仍未解决。

完整部署清单与保护文件：[all-games-deployment.json](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/diagnostics/2026-10-14/exposure-inputs/all-games-deployment.json)。源文件修改前备份、部署前 DLL 备份和本轮验证归档保存在同目录。临时 CPU 程序、编辑脚本、部署核验脚本及日志归入 verification-evidence.zip，未加入正式工程或 build 目录。

此前各游戏修复与清理详情：[修复报告](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/docs/REPAIR-REPORT-2026-10-12-13.md)。本记录更新其构建和部署状态，历史问题的验收边界继续有效。
