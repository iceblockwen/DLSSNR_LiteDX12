# 审查问题修复与星空日志排查

最终更新：用户已确认星空创建快照修复有效；诊断清理及全部 20 个游戏部署完成，16 个 DX12 统一为 4CB7542E…，4 个 DX11 为 A6987DE5…，文件及配置 / 模型校验通过。[最新清理与部署报告](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/docs/CONFIRMED-CLEANUP-2026-10-14.md)。以下状态是历史记录。

最新星空状态：原 Create 返回后参数改写造成创建尺寸缓存错误，已改为调用前快照；实测日志、旧版失败 / 新版通过 CPU 复现及 Release / Debug 构建确认。仅星空已部署正式主体 4CB7542E…，替换诊断版，待实际验收。[最新修复报告](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/docs/STARFIELD-CREATE-METADATA-2026-10-14.md)。以下星空尝试及诊断准备是历史记录。

最新星空状态：两次尺寸尝试已撤回，正式源码恢复；用户指出装备界面内独立渲染，已仅星空部署保持基线行为的隔离 feature 诊断，待关联数据。[最新排查](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/docs/STARFIELD-STREAMS-2026-10-14.md)。下方相关尝试为历史记录。

星空最新：上一轮当前 Evaluate 声明方案经用户反馈无效、已撤回；改为仅星空零原点输出采用参考的当前纹理尺寸处理，已单项部署，实际恢复待确认。[最新报告](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/docs/STARFIELD-TEXTURE-2026-10-14.md)。

后续：用户复现装备界面切回后 waiting，新日志明确为 3840×2160 分配 / 2560×1440 声明。已修正 DX12 每帧声明优先级，仅部署星空，游戏实际恢复待确认。[后续尺寸修复报告](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/docs/STARFIELD-EQUIPMENT-2026-10-14.md)。下方全量部署 hash 是此前审查修复版记录。

日期：2026-10-14。对应 [完整审查报告](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/docs/FULL-AUDIT-2026-10-14.md) 第 4—7 项。DX12 / DX11 已构建、部署至全部 20 个登记目标，并核验 SHA256。本轮没有启动游戏或执行 GPU / 窗口测试。

## 已完成的修复

### DX11 按实际输入来源暂停原生深度工作

Capture11 收到完整的游戏 color / depth / MV 后，暂停该 context 的原生候选深度跟踪。draw、bind、clear、finish、execute hook 都先检查该 context 的需求；暂停期间不进入候选统计锁，也不录制候选深度备份。

需求采用线程本地缓存的共享 atomic policy。正常情况下只读 epoch 和启用状态，不对每次 draw 做 map 查找。创建 / 删除 context 时使缓存代际失效，缓存只保留小型 policy，不持有 context 或 GPU 资源。

游戏输入缺失、feature 释放后恢复原生回退，重新读取真实深度绑定和写入状态，清空暂停前的候选，防止拿旧深度冒充新帧。恢复的第一帧可能需要重新收集深度；没有有效候选时保留原画面。没有 NGX 输入的 context 继续原来的捕获路线，骑砍回退未删除；其他独立 context（包括未获得 NGX 输入的 deferred context）继续保守跟踪，未将一个 context 的来源推广到全设备。

### 两份工程减少键盘轮询重复工作

- DirectInput 设备的 kind 和 native 入口在有最终 Release 保护的设备记录中缓存，不再每次轮询重复 GetCapabilities 和匹配 vtable。设备最终 Release 后清除记录，同地址重用重新识别。
- 扫描码到虚拟键的映射按调用线程的键盘布局缓存。布局改变才重新计算；零状态扫描码不做映射。
- 键盘阻断按一次状态快照处理，最多一次取得 keyboard mutex，代替每键独立获取。Win32 GetKeyboardState 也使用同一快照。
- 即使这次 DI 轮询没有 DOWN 键，仍完成等待物理 UP 的清理，避免减少工作后把旧抑制状态留到下一次新按键。Ins 归属、菜单消费的按下和欠游戏的释放规则保留。

### DX11 尺寸契约与 DX12 对齐

NGX Get 成功返回 0 时，依次使用创建尺寸、可用 depth 范围回退。将创建时声明的输出 width / height 完整传到 executor，既有 partial-output guard 因而能拦截原点为零的部分输出；声明超出分配也继续拒绝，不做截断后伪装成全幅。

这项修复不添加 GPU pass、复制或同步；它补齐参数解释和保护。本轮没有新增部分输出渲染支持。

### DX11 按 feature 创建代际重置历史

NGX feature 创建时分配唯一 stream generation，普通 evaluate 和 EvaluateFeature_C 都传递它。不同入口的 handle 地址相同、同地址释放后重建，也不会被误认为同一历史来源。

每个 context 继续只保留一套 NR executor。输入的 stream 或来源在游戏 NGX / native fallback 之间切换时，走既有 Reset 路径，重置模型与白点历史。身份在成功提交并完成有效 NR 录制后才确认；busy / 未提交不会提前吞掉来源变化。

ReleaseFeature 在退出 NGX metadata 锁后通知对应 generation 的活动 context，退休 executor 和相关历史，并恢复深度回退。桥接及其有序队列继续保留；已提交 recording 持有的 GPU owner 不被强制释放，模型仍按既有 GPU ticket 退休。

这个方案避免为多 feature 增加多套常驻模型。多个 feature 交替使用同一 context 时会分别重置，不能宣称它提供了每个 feature 独立的长期历史；它解决的是不同来源静默混用。

## 星空持续 waiting：已确认线索，尚未确认根因

读取文件：`G:\XboxGames\Starfield\Content\DLSSNR_LiteDX12.log`。最新启动为 2026-10-14 01:53:19，文件最后写入为 02:09:14。最新一轮先出现正常的 feature 创建等待，之后多次出现：

```text
NR: Partial NR color output is not supported (applied 0, HRESULT 0x80004001, NGX 0x00000000)
```

现有日志能确认后续输入被部分输出保护拒绝；没有这轮 device removed、模型 evaluate 失败或 busy 记录。不能据此证明这些错误在任何其他位置都没有发生，也不能仅凭该日志解释具体是哪一条尺寸数据有问题。

此拒绝标记为 frameSkipped，不会永久封锁 ModelActive。后续仍会尝试，若输入一直不满足全幅契约就一直跳过。日志中的“Waiting for captured NR input”也会在用户重新开启 NR 时出现，不能把它单独读成“游戏没开 DLSS”。

参考实际核对：Opt 的 `DlssNr_Dx12.cpp` 以当前 output texture 的 GetDesc 得出工作输出尺寸；本工程另外保留 NGX 创建时的输出声明并执行部分区域保护。当前日志未记录两者具体值，因此至少仍有两个可能：游戏实际使用部分输出 / 带 padding 的分配；创建时声明与当前纹理尺寸不再一致。未证明是哪一个，不盲目把声明尺寸换成分配尺寸，防止再次处理无效区域或覆盖整幅。

本次 DX12 新增普通失败日志详情：allocation、declared 和 origin 六个整数。只在拒绝首次出现或尺寸变化时记录，没有成功帧采样、GPU 读回、计数器监控或截图导出。保护保持，输入继续可重试。**星空功能故障尚未宣称修复**；下一次自然发生 waiting 时的日志将提供区分这两种情况所需的数字。本轮旧日志已归档。

## 验证

| 检查 | 结果与边界 |
| --- | --- |
| DX11 / DX12 Release、Debug | 四份最终构建成功，最终日志无 compiler warning / error。DX12 增加星空详情后重新构建 Release 与 Debug |
| 生产 keyboard / DirectInput CPU 检查 | 两工程通过：批量与逐键结果一致，欠 UP、菜单抑制、Ins、设备重用、布局失效。各一万次稳定轮询不新增映射或 GetCapabilities |
| 生产 guide contract CPU 检查 | 两工程零尺寸、分配回退、偏移越界、部分 / 超大输出声明、MV flags 行为一致 |
| 生产 depth policy + 假 COM context | 通过：按 context 暂停，一万次稳定需求检查不查询 context，恢复时重新读取绑定 / 状态，销毁 / 重用不沿用旧 policy |
| 生产 NGX capture + 假入口 | 通过：普通 / _C stream 传递、同地址重建、跨入口 handle 地址相同、精确 generation 释放。没有运行 NVIDIA 模型 |
| 真实游戏与 GPU 性能 | 未运行，未量化帧率 / GPU 毫秒；CPU 操作次数不等于游戏帧率收益 |

Debug 首次在沙盒中失败为 MSVC C1902，工具文件版本一致；使用独立 PDB endpoint 仍失败，在正常构建环境下两工程成功。没有修改构建配置规避 Debug，也没有关闭用户程序或更换 VS 工具链。失败与成功日志均保留在证据归档。

## 部署和清理

16 个 DX12、4 个 DX11 登记游戏全部核验，实际只更新变更的主体 DLL；代理 hash 不变，没有重复写入。每个游戏的已有 INI、NR 模型、原生 DLSS 和已识别 REF 配置 / DLL 与这次部署前 hash 一致。

Release 主体 SHA256：

```text
DX12 32FFFC22C89F4AD86F4492CA6FF4CCA67365FB2AF053980EF5B1EE6F63298E63
DX11 A6987DE533ED5594D5CD7A72076EEBD2BF9734DE701365ECA3A735787D92C286
```

全量清单与保护文件：[all-games-deployment.json](H:/AgentWorkPlace/DLSS5/DLSSNR_LiteDX12/diagnostics/2026-10-14/audit-fixes/all-games-deployment.json)。部署前主体 / 代理和源码保存在同目录 ZIP。CPU 程序、源文件、辅助脚本与日志归入 verification-evidence.zip；未加入 CMake、Build.ps1 或正式 build 目录。

源代码变更：DX12 7 个文件（键盘 4 个、星空失败详情 3 个），DX11 12 个文件（键盘 4 个、捕获 / 历史 / 深度 / 尺寸 8 个）。默认 INI、shader、模型二进制、代理、图像算法和面板 UI 未改。build 仍只有标准 windows-release / windows-debug。

原审查第 1—3 项的 DX12 replay、DX11 capture 恢复锁顺序、输入 detour 线程保护不在此次圈选范围，尚未修复。本轮也不宣称骑砍 NGX 捕获或此前模型切换后 GPU 时间长期变化已解决。
