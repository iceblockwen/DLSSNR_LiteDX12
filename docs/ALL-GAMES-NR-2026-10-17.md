# 全部登记游戏部署新版 NR

后续状态：2026-10-18 已按用户要求，将全部 20 款游戏模型回退至旧版 310.8.0.0，保留插件与配置。见 [旧模型全量回退记录](ALL-GAMES-OLD-NR-2026-10-18.md)；同日稍后又换回 310.9.16.0，见 [全量换新模型记录](ALL-GAMES-NEW-NR-2026-10-18.md)。下文为 10 月 17 日的历史部署记录。

日期：2026-10-17。20 / 20 款完成，16 款 DX12、4 款 DX11。部署当前正式插件和 NR 310.9.16.0，逐项核验 SHA256；未启动任何游戏。

## 版本与选择

- 新模型：310.9.16.0，266424944 字节，NVIDIA 数字签名 Valid。
- 模型 SHA256：2BC19BC09A7B89DD44F73E2AC0108FCC586E2154BDD0830F242F4F040E28789F。
- 两工程都有 Default / A / B 菜单；A=1，B=2，新 DLL 的 Default / 0 默认选择 B。
- 新 DLL 内含 A、B 两项模型权重配置。旧 DLL 约 158 MiB，新 DLL 约 254 MiB，增加约 96 MiB。A 对应原模型路线，不能仅凭名称证明与旧 DLL 权重完全相同。插件按所选 preset 执行，不同时运行 A、B。
- 保留全部现有 INI；本次实际读取的 20 个游戏都是 Preset=0，因此这次部署后默认使用 B。NR 开关、曝光、风格等设置保持原值。

## 正式构建

| 项目 | 主体 SHA256 |
| --- | --- |
| DX12 | 075D7CBD23D84B562A7FF37B875A8EAE4DC43C5FB555C1856A608B97476AD992 |
| DX11 | B93997CEB726DEC92D8F867B1842E2587AECF8251CC37F8C8C3C65C59DF848E5 |

两工程 Release 构建成功，日志无 MSVC warning / error。DX12 保留怪猎已实测有效的计算根签名完整性保护。重新构建的 DX12 主体与怪猎验收的 C887BA92…逐节比较：机器码、数据、异常展开信息均相同；只有 COFF / debug-directory 时间戳共 4 字节变化，因此整文件 SHA256 更新。未增加其他运行逻辑。

## 部署清单

| API | 游戏 | 目录 | 结果 |
| --- | --- | --- | --- |
| DX12 | 星球大战绝地：幸存者 | G:\EAGames\Jedi Survivor\SwGame\Binaries\Win64 | verified |
| DX12 | GTA V Enhanced | G:\SteamLibrary\steamapps\common\Grand Theft Auto V Enhanced | verified |
| DX12 | 卧龙 | G:\XboxGames\Wo Long- Fallen Dynasty\Content | verified |
| DX12 | 怪物猎人荒野 | F:\SteamLibrary\steamapps\common\MonsterHunterWilds | verified |
| DX12 | 浪人崛起 | G:\SteamLibrary\steamapps\common\Ronin | verified |
| DX12 | 看门狗军团 | G:\UBISoft\Watch Dogs Legion\bin | verified |
| DX12 | 极限竞速地平线 6 | G:\XboxGames\Forza Horizon 6\Content | verified |
| DX12 | 天国拯救 II | G:\XboxGames\Kingdom Come- Deliverance II\Content | verified |
| DX12 | 星空 | G:\XboxGames\Starfield\Content | verified |
| DX12 | 心灵杀手 2 | G:\EpicGames\AlanWake2 | verified |
| DX12 | 神秘海域合集 | G:\SteamLibrary\steamapps\common\Uncharted Legacy of Thieves Collection | verified |
| DX12 | 龙之信条 2 | F:\SteamLibrary\steamapps\common\Dragons Dogma 2 | verified |
| DX12 | 对马岛 | F:\SteamLibrary\steamapps\common\Ghost of Tsushima DIRECTOR'S CUT | verified |
| DX12 | 仁王 3 | F:\SteamLibrary\steamapps\common\Nioh3 | verified |
| DX12 | 死亡搁浅导演剪辑版 | F:\XboxGames\DSDC\Content | verified |
| DX12 | 战锤 40K 星际战士 2 | F:\XboxGames\Warhammer 40,000- Space Marine 2\Content\client_pc\root\bin\pc | verified |
| DX11 | 博德之门 3 | F:\SteamLibrary\steamapps\common\Baldurs Gate 3\bin | verified |
| DX11 | Rise of Eros | H:\Games\RiseOfEros | verified |
| DX11 | 幽灵行动荒野 | G:\UBISoft\Tom Clancy's Ghost Recon Wildlands | verified |
| DX11 | 骑马与砍杀 II | F:\SteamLibrary\steamapps\common\Mount & Blade II Bannerlord\bin\Win64_Shipping_Client | verified |

## 核验与保留

- 共核验 66 个目标文件：20 个游戏根目录的代理、主体、模型，以及怪猎和龙信 2 已有 _storage_ 内的同名副本。
- 实际写入 39 个文件；其中模型更新 17 份，另 5 份模型已与新源文件相同，因此保留。22 份模型副本最终均与新版源文件 SHA256 一致。
- 64 个保护文件部署前后 SHA256 不变，包含现有 INI、原生 DLSS SR / RR / FG、存在的 REFramework DLL 和相关配置。没有覆盖这些文件。
- 代理已是当前正式版本，因此没有重复写入。游戏运行时检查均未发现需跳过的活动游戏；没有结束用户进程。
- 所有差异文件先备份后复制，复制后再次校验；没有发生失败或回退。备份以内容 SHA256 去重，避免每个游戏重复保存一份相同的大模型。
- 本次文件部署完成不等于全部游戏已经逐一进行画面、稳定性和性能验收。怪猎之前由用户完成实际验证；其他游戏没有自动运行。

## 证据与回退

清单：`diagnostics/2026-10-17/all-games-new-nr/deployment.json`，包含每个文件的旧 / 新 SHA256、备份位置、复制动作及保护文件的前后 hash。日志为同目录 `deploy.log`；源码部署辅助脚本仅在 diagnostics，不接入插件构建。

回退文件在同目录 `backup-objects/`。每个修改目标在清单中对应 Backup 路径和 Before hash，恢复时按目标逐项复制并核验；这些 .bin 是原始 DLL / INI 的备份，不是插件运行依赖。

怪猎旧模型的独立备份也保留在 `diagnostics/2026-10-17/preset-mhwilds/model-310.9.16/`。工作区根目录的旧模型源文件仍保留；本次以新版模型目录作为显式部署源，未改写旧源文件。
