# 全部游戏回退旧版 NR 模型

后续状态：2026-10-18 当天稍后已按要求把全部 20 个目标的模型换回 310.9.16.0，preset 未改，见 [全量换新模型记录](ALL-GAMES-NEW-NR-2026-10-18.md)。下文为当日的回退记录。

日期：2026-10-18。按用户要求，将全部登记游戏的 nvngx_dlssnr.dll 回退至 310.8.0.0。20 / 20 款完成，含 16 款 DX12、4 款 DX11；未启动游戏。

## 模型与范围

- 来源：H:\AgentWorkPlace\DLSS5\DLSSNR_Model\nvngx_dlssnr_2026_9\nvngx_dlssnr.dll。
- 版本 310.8.0.0，165840496 字节；NVIDIA 数字签名 Valid。
- SHA256：E16BCF15E16E13F527491CDF7845B2FE6521A738D8F7C9C721866A8496E1FC8E。
- 已核验 22 份模型，包含怪猎、龙信 2 现有 _storage_ 副本；每份均校验版本和 SHA256。
- 113 个保护文件前后 hash 相同，包括现有插件主体、代理、INI、原生 DLSS SR / RR / FG 以及存在的 REF DLL / 配置。保留已部署的稳定性修正及现有设置。
- 未创建新的 _storage_ 目录；只同步其中已存在的模型副本。核对 REFramework.cpp 后确认：REF 会创建该目录，并在初始化时将游戏根目录 DLL 复制进去、覆盖同名文件。REF 还调用模块路径伪装，因此 dump 中显示 _storage_ 路径不能单独证明映射文件的真实磁盘来源；此前据此直接认定“实际加载副本”的表述过强。此次同步保持根目录与现有副本一致。
- 模型替换前备份差异文件，按 SHA256 去重；失败回退逻辑未触发，未结束用户进程。

## 目标清单

| API | 名称 | 目录 | 模型副本数 | 结果 |
| --- | --- | --- | ---: | --- |
| DX12 | jedi | G:\EAGames\Jedi Survivor\SwGame\Binaries\Win64 | 1 | verified |
| DX12 | gta | G:\SteamLibrary\steamapps\common\Grand Theft Auto V Enhanced | 1 | verified |
| DX12 | wolong | G:\XboxGames\Wo Long- Fallen Dynasty\Content | 1 | verified |
| DX12 | mhwilds | F:\SteamLibrary\steamapps\common\MonsterHunterWilds | 2 | verified |
| DX12 | ronin | G:\SteamLibrary\steamapps\common\Ronin | 1 | verified |
| DX12 | wdlegion | G:\UBISoft\Watch Dogs Legion\bin | 1 | verified |
| DX12 | fh6 | G:\XboxGames\Forza Horizon 6\Content | 1 | verified |
| DX12 | kcd2 | G:\XboxGames\Kingdom Come- Deliverance II\Content | 1 | verified |
| DX12 | starfield | G:\XboxGames\Starfield\Content | 1 | verified |
| DX12 | alanwake2 | G:\EpicGames\AlanWake2 | 1 | verified |
| DX12 | uncharted | G:\SteamLibrary\steamapps\common\Uncharted Legacy of Thieves Collection | 1 | verified |
| DX12 | dd2 | F:\SteamLibrary\steamapps\common\Dragons Dogma 2 | 2 | verified |
| DX12 | tsushima | F:\SteamLibrary\steamapps\common\Ghost of Tsushima DIRECTOR'S CUT | 1 | verified |
| DX12 | nioh3 | F:\SteamLibrary\steamapps\common\Nioh3 | 1 | verified |
| DX12 | dsdc | F:\XboxGames\DSDC\Content | 1 | verified |
| DX12 | sm2 | F:\XboxGames\Warhammer 40,000- Space Marine 2\Content\client_pc\root\bin\pc | 1 | verified |
| DX11 | bg3 | F:\SteamLibrary\steamapps\common\Baldurs Gate 3\bin | 1 | verified |
| DX11 | roe | H:\Games\RiseOfEros | 1 | verified |
| DX11 | wildlands | G:\UBISoft\Tom Clancy's Ghost Recon Wildlands | 1 | verified |
| DX11 | bannerlord | F:\SteamLibrary\steamapps\common\Mount & Blade II Bannerlord\bin\Win64_Shipping_Client | 1 | verified |

## 证据

逐文件旧 / 新 hash、备份路径和保护文件结果保存在 `diagnostics/2026-10-18/all-games-old-nr/deployment.json`；执行日志为同目录 `deploy.log`。换下的模型备份在同目录 `backup-objects/`，清单记录了对应关系。

这次只执行模型文件回退和静态核验，没有重新构建插件，没有启动游戏或进行 GPU 测试。10 月 17 日的新版部署记录保留为历史。本次 310.8.0.0 回退之后已完成 310.9.16.0 全量换新，当前列表游戏模型以 [换新记录](ALL-GAMES-NEW-NR-2026-10-18.md) 为准。
