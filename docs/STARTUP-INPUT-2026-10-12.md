# 启动首次点击卡顿与面板焦点检查

后续状态：焦点/锁范围修正保留；本页描述的临时 `Panel input` 状态日志和锁等待计时已按用户要求移除。见 [清理记录](DIAGNOSTICS-CLEANUP-2026-10-12.md)。以下为原修正和诊断记录。

用户反馈：进入主菜单后，尚未按过 Ins，首次鼠标点击时整机游戏画面卡一下；此后 Ins 才能打开面板。尚未通过日志确认焦点确实被哪个窗口抢走。

## 源码发现与修正

1. `Panel::Impl::WindowProc` 在调用游戏原来的 WndProc 时仍持有 `Graphics().mutex`。游戏处理激活、捕获或点击时可能等待渲染线程，而插件的渲染入口同样需要这把锁，存在交叉等待风险。现在复制原 WndProc，在调用前解锁；解锁后不再访问 `self` 或 `route`。窗口销毁后的路由清理单独重新加锁。参考 `_ref/OptiScaler_DLSSNR/OptiScaler/menu/input/input_system_messages.cpp` 中 `OptiInputWndProc`（约 710–752 行），其输入处理锁同样在调用原 WndProc 之前结束。原 Lite 存在相同的持锁转发问题，本轮未修改原 Lite 或参考代码。
2. Peek/GetMessage 的 `RouteKeyboard` 原先对每条消息先抢渲染锁，然后才判断是否键盘消息。现在非键盘消息直接返回，不在消息队列层等待渲染锁；它们仍正常进入原来的 DispatchMessage/WndProc 路径。
3. `inputFocused` 最初在安装 subclass 和输入钩子之前查询，安装期间发生的焦点变化可能没有更新到缓存。现在安装完成后、Ins 切换面板时、可见面板绘制前重新查询 GUI 线程的实际焦点。捕获仍需要面板可见、目标窗口确有焦点且位于前台。

本轮只修改 `src/panel.cpp`。没有调用 SetForegroundWindow/SetFocus 强制激活游戏，也没有修改 NR、图像 shader、模型切换或队列回收。

## 验证与限制

Release 构建与链接通过，无 MSVC warning/error。源码修改前备份：`build/startup-input-backup-20261012/panel.cpp`；构建日志：`build/startup-input-build-release.log`。主体 DLL SHA256：`915BD16F569A9D4FBDC0630F85647FB2327D41B5108DEBCA7DAEE2E3567D936C`。

新增 `Panel input` 日志只在挂接窗口、焦点变化、Ins 切换以及选定输入消息等待渲染锁至少 100 ms 时写入；记录 focus、foreground、visible、captured、lock_wait。没有在鼠标移动或每帧写日志。

修正的是确认存在的锁范围与焦点缓存问题，尚未证实它们就是用户此次首次点击卡顿的唯一根因。WndProc 内部仍需要短暂获取渲染锁处理插件状态；模型初始化、resize 等长操作仍可能使该锁的获取延迟，本轮新增日志用于区分。窗口消息实际是否到达、是否发生游戏自身窗口模式切换，需要用户重启游戏验证；此工程不自动运行游戏或 GPU/窗口测试。

验证步骤：重启进入主菜单，先尝试 Ins，再关闭面板并点击游戏；另一次启动先点击游戏再按 Ins，对照是否仍有卡顿。若仍复现，检查 `Panel input` 的 focus/foreground 和 `lock_wait`。

## 部署

2026-10-12 00:34:01 +08:00 已部署到 Jedi 原目标目录。两个 DLL SHA256 与 Release 产物一致；游戏现有 INI 与模型部署前后哈希一致。记录：`build/deployment-jedi-20261012-startup-input.json`。未启动游戏。
