# DLSSNR_LiteDX12 开发规则

本工程是 `DLSSNR_Lite` 的 **Direct3D 12 only** 分支，个人使用。

## 原则

- 先读参考再动手：`_ref/OptiScaler_DLSSNR`（NR 模型、参数名、命令列表状态）与 `DLSSNR_Lite`（代理、包装、NGX 捕获、面板）。不凭名称或记忆声称已参考。
- **不修改／不清理**参考工程与 `DLSSNR_Lite`；本工程从它复制而来，两边独立演进。
- 新依赖、接入路线或行为取舍先说明方案与影响再实施。
- 真实游戏只由用户运行；此工程不含测试，也不在构建时执行任何 GPU／窗口检查。

## 当前范围

- 仅 DX12：DXGI Factory／SwapChain 包装（面板与队列捕获）、Loader 监测、NGX D3D12 捕获、命令列表绑定快照／恢复、NR Pass 与合成 shader、ImGui 面板。
- 已删除：DX11 原生捕获、DX11→DX12 桥接、光流估计、深度查找、独立跑分与测试入口。
- 兼容策略由 INI 显式开启并在启动时缓存：`SkipSmallSwapchains`（小于 100 的交换链不包装）、`DirectResolve`（复制三种输入并把结果直接写回捕获颜色 UAV）。不要按进程名判断游戏，也不要把 `DirectResolve` 推广到颜色不是 UAV 的游戏。
- 正常面板计时、按需求曝光读回、资源预览属于产品功能；临时采样/图片导出/成功路径监测不得进入正式构建。历史诊断证据统一归档 diagnostics。
- NR 输入只来自游戏自身的 NGX SR／RR evaluate（feature 1／13），覆盖普通与 `_C` 评估入口。

## 约定

- 面板窗口标题、INI、日志、运行时 DLL 文件名统一使用 `DLSSNR_LiteDX12` / `nvngx.dll_litedx12.dll`。
- 代理 `dxgi.dll` 只转发系统导出并加载主体，不持有图形／UI／模型对象。
- 主体导出名 `Start` 是代理与主体之间的私有 ABI，可保留。工厂由主体自己 detour 系统 `CreateDXGIFactory*` 包装，代理只转发。

## 代码风格

- 大括号一律独占一行（Allman），函数、命名空间、`extern "C"` 块、`struct` 都适用。
- `if` / `else` / `for` / `while` / `do` / `switch` 必须带大括号并换行；不写 `if (x) return;` 这类单行复合语句。
- 空函数体也要分行写：
  ```cpp
  void WINAPI EtwRundown()
  {
  }
  ```
- 缩进 4 个空格；`namespace` 内容不缩进，`namespace` 与 `extern "C"` 的左括号按上面第一条处理。
- 这些是排版要求，不得借排版顺手改动逻辑；改排版的提交应当能通过"代码段与原版逐字节一致"的核对。
