# 星空验收、诊断清理与全部游戏部署

日期：2026-10-14。用户已确认星空修复有效：“ok 可以了”。清理完成，全部 20 个登记游戏均部署并核验通过。

## 正式代码与清理结果

保留创建前保存 NGX 尺寸与标志的通用修复。星空装备界面切换时，原创建调用返回后共享参数可能已改写，旧代码在这个时点读取造成错误缓存；创建前快照解决了本轮交替 waiting / 恢复问题。[完整证据与修复说明](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/docs/STARFIELD-CREATE-METADATA-2026-10-14.md)。

检查 DX12 / DX11 的 src、CMakeLists.txt 和 Build.ps1，未发现本轮 NGX trace、create-before / after、临时测试入口、StarfieldTextureOutput 尺寸特例或 NvAPI GPU 负载 / 频率采集。临时诊断和 CPU 检查先前已隔离并归档，没有进入正式构建。

本次额外收起龙信 2 排查遗留的 15 个散落文件：REFramework 参考源、诊断日志、旧主体 / 代理 DLL 与旧配置，分别放入原 diagnostics 案例目录的 verification-evidence.zip。每个归档条目校验 SHA256 后才移除散落副本；官方 REFramework 下载 ZIP 及部署 JSON 保留。游戏中的正式 REFramework 未移除。

保留实际产品使用的资源预览、测光读回、面板 GPU 计时和失败状态报告。DX11 的 diagnostics.cpp 承担捕获、桥接、面板状态与原生回退，并非临时测试文件，因此保留。build/windows-release 与 windows-debug 是正常编译缓存，没有新增测试目录。

验收后的生产源码及正式 DX12 DLL 与本轮已测试版本 hash 一致，本次清理没有再改渲染行为、每帧路径或 GPU 工作。沿用已经通过的 Release / Debug 构建和旧版失败 / 修复版通过的正式代码 CPU 检查，不以重复构建制造另一份未经游戏验收的二进制。

## 部署版本

| 工程 | 数量 | 正式主体 SHA256 |
| --- | --- | --- |
| DX12 | 16 | 4CB7542E5A3C8B44C3C5DB8CD01655A7D94A8CBAC385336588688911E082DE6B |
| DX11 | 4 | A6987DE533ED5594D5CD7A72076EEBD2BF9734DE701365ECA3A735787D92C286 |

DX12 代理仍为 11076F50635B213BD3C04DBC5F07B24D4D43622A4A3F9CB1F71059A4F8CFC3CD；DX11 代理仍为 1164C025F6EAA1A8C84E38C58E5EA89FC4AF6D189BB29C2C8D3212588C35DCF0。

部署使用两份工程各自 tools/deploy.ps1 的当前清单，覆盖对应代理和主体；已有 INI、NR 模型、原生 NGX DLL、dinput8.dll 逐项按部署前 SHA256 核验。DX11 此轮没有新的源码修复，重新部署对应正式版；没有把 DX12 DLL 放入 DX11 游戏。

| 游戏 / 清单名 | API | 结果 |
| --- | --- | --- |
| Jedi Survivor / jedi | DX12 | 已部署并核验 |
| GTA V Enhanced / gta | DX12 | 已部署并核验 |
| 卧龙 / wolong | DX12 | 已部署并核验 |
| 怪物猎人荒野 / mhwilds | DX12 | 已部署并核验 |
| 浪人崛起 / ronin | DX12 | 已部署并核验 |
| 看门狗军团 / wdlegion | DX12 | 已部署并核验 |
| 极限竞速地平线 6 / fh6 | DX12 | 已部署并核验 |
| 天国拯救 II / kcd2 | DX12 | 已部署并核验 |
| 星空 / starfield | DX12 | 已部署并核验，修复已获用户验收 |
| Alan Wake 2 / alanwake2 | DX12 | 已部署并核验 |
| 神秘海域合集 / uncharted | DX12 | 已部署并核验 |
| 龙之信条 2 / dd2 | DX12 | 已部署并核验 |
| 对马岛 / tsushima | DX12 | 已部署并核验 |
| 仁王 3 / nioh3 | DX12 | 已部署并核验 |
| 死亡搁浅 / dsdc | DX12 | 已部署并核验 |
| 战锤 Space Marine 2 / sm2 | DX12 | 已部署并核验 |
| 博德之门 3 / bg3 | DX11 | 已部署并核验 |
| Rise of Eros / roe | DX11 | 已部署并核验 |
| 幽灵行动荒野 / wildlands | DX11 | 已部署并核验 |
| 骑马与砍杀 II / bannerlord | DX11 | 已部署并核验 |

[全部目标路径与逐文件核验](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/diagnostics/2026-10-14/confirmed-cleanup/all-games-deployment.json)；[清理与归档核验](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/diagnostics/2026-10-14/confirmed-cleanup/cleanup.json)。部署脚本和各游戏输出保存在同目录 verification-evidence.zip。

本次未启动游戏。星空有效性来自用户实际验收，其他游戏本次确认的是文件部署正确。之前报告中骑砍 NGX 捕获及完整审查第 1—3 项的未解决状态不因本次部署而改变。
