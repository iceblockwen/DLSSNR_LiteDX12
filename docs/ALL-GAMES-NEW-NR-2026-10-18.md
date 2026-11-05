# 全部登记游戏换用新版 NR 模型

日期：2026-10-18。按用户要求，把 20 个登记目标的 `nvngx_dlssnr.dll` 全部换到 **310.9.16.0**，preset 一律不动。20 / 20 完成（16 款 DX12、4 款 DX11），未启动游戏。

## 模型与范围

- 来源：`H:\AgentWorkPlace\DLSS5\DLSSNR_Model\nvngx_dlssnr_2026_10\nvngx_dlssnr.dll`。
- 版本 310.9.16.0，266424944 字节；NVIDIA 数字签名 Valid。
- SHA256：2BC19BC09A7B89DD44F73E2AC0108FCC586E2154BDD0830F242F4F040E28789F。
- 共 22 份模型：20 个游戏根目录，另加怪猎、龙信 2 已存在的 `_storage_` 副本。实际替换 21 份；GTA 在此之前已是该模型，记为保持。
- 113 个保护文件前后 hash 相同，包含插件主体、代理、全部 INI、原生 DLSS SR / RR / FG 以及存在的 REFramework DLL / 配置。
- **未修改任何 INI**：19 个游戏目录配置保持 `Preset=0`（新模型 Default 即 B）；GTA 的生效配置是重定向 profile `%LOCALAPPDATA%\DLSSNR_LiteDX12\Profiles\GTA5_Enhanced-851792630aab9068\DLSSNR_LiteDX12.ini`，其中 `Preset=1`（A）原样保留。用户按计划进游戏自行试 preset。
- 插件与代理没有重新部署：DX12 主体 075D7CBD…、DX11 主体 B93997CE… 与 10 月 17 日正式构建一致，20 个目录核对相同。
- 旧模型按 SHA256 去重，只备份一份（165840496 字节）。换模型前先备份，复制后逐份校验版本、SHA256 与签名；失败自动按清单回滚，本次未触发。
- 本次只替换模型文件并做静态核验，没有启动游戏或 GPU 测试。

## 目标清单

| API | 名称 | 目录 | 模型副本 | 结果 |
| --- | --- | --- | ---: | --- |
| DX12 | jedi | G:\EAGames\Jedi Survivor\SwGame\Binaries\Win64 | 1 | replaced |
| DX12 | gta | G:\SteamLibrary\steamapps\common\Grand Theft Auto V Enhanced | 1 | already-new |
| DX12 | wolong | G:\XboxGames\Wo Long- Fallen Dynasty\Content | 1 | replaced |
| DX12 | mhwilds | F:\SteamLibrary\steamapps\common\MonsterHunterWilds | 2 | replaced |
| DX12 | ronin | G:\SteamLibrary\steamapps\common\Ronin | 1 | replaced |
| DX12 | wdlegion | G:\UBISoft\Watch Dogs Legion\bin | 1 | replaced |
| DX12 | fh6 | G:\XboxGames\Forza Horizon 6\Content | 1 | replaced |
| DX12 | kcd2 | G:\XboxGames\Kingdom Come- Deliverance II\Content | 1 | replaced |
| DX12 | starfield | G:\XboxGames\Starfield\Content | 1 | replaced |
| DX12 | alanwake2 | G:\EpicGames\AlanWake2 | 1 | replaced |
| DX12 | uncharted | G:\SteamLibrary\steamapps\common\Uncharted Legacy of Thieves Collection | 1 | replaced |
| DX12 | dd2 | F:\SteamLibrary\steamapps\common\Dragons Dogma 2 | 2 | replaced |
| DX12 | tsushima | F:\SteamLibrary\steamapps\common\Ghost of Tsushima DIRECTOR'S CUT | 1 | replaced |
| DX12 | nioh3 | F:\SteamLibrary\steamapps\common\Nioh3 | 1 | replaced |
| DX12 | dsdc | F:\XboxGames\DSDC\Content | 1 | replaced |
| DX12 | sm2 | F:\XboxGames\Warhammer 40,000- Space Marine 2\Content\client_pc\root\bin\pc | 1 | replaced |
| DX11 | bg3 | F:\SteamLibrary\steamapps\common\Baldurs Gate 3\bin | 1 | replaced |
| DX11 | roe | H:\Games\RiseOfEros | 1 | replaced |
| DX11 | wildlands | G:\UBISoft\Tom Clancy's Ghost Recon Wildlands | 1 | replaced |
| DX11 | bannerlord | F:\SteamLibrary\steamapps\common\Mount & Blade II Bannerlord\bin\Win64_Shipping_Client | 1 | replaced |

## 证据与回退

逐文件旧 / 新 hash、备份路径和保护文件结果保存在 `diagnostics/2026-10-18/all-games-new-nr-v2/deployment.json`；执行日志为同目录 `deploy.log`，脚本为同目录 `deploy-new-models.ps1`。
换下的旧模型备份在 `backup-objects/E16BCF15…FC8E.bin`（即 310.8.0.0 原文件）。回退时按清单逐项复制该备份并核验 hash 即可；310.8.0.0 正式源文件仍保留在 `DLSSNR_Model\nvngx_dlssnr_2026_9\`。

## 与 10 月 17 日全量部署的差异

10 月 17 日那次同时部署了代理与插件主体；本次插件已是同一构建，因此只动模型。GTA 在 10 月 18 日回退清单里记为已回到 310.8.0.0，实际在本次部署前已是 310.9.16.0（文件时间戳 2026-10-17 10:28:26），说明回退之后另有一次拷贝，本次按“已是新版”保持。
