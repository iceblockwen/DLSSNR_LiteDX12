# DLSSNR_LiteDX12

Windows x64 的 **Direct3D 12** DLSS Neural Rendering 插件（个人使用）。

沿游戏自己的 NGX DLSS／DLAA／光线重建调用之后插入 NR：观察游戏的 `NVSDK_NGX_D3D12_EvaluateFeature` 或 `EvaluateFeature_C`，把该次 evaluate 的 Color／Depth／MotionVectors 交给 NR 模型，把结果合成回原图。面板移植自 `DLSSNR_Lite`。

## 构建

安装 Visual Studio 的“使用 C++ 的桌面开发”工作负载（MSVC + Windows SDK + CMake 工具）。

```powershell
./Build.ps1                    # Release
./Build.ps1 -Configuration Debug
```

输出到 `dist/<配置>/`。本工程不含测试。

## 使用

把 `dist/Release/` 里的三个文件放到游戏目录：

- `dxgi.dll`（代理，转发系统 DXGI 并启动主体）
- `nvngx.dll_litedx12.dll`（主体；名字含 `nvngx.dll` 是为了通过模型 snippet 的 caller 检查）
- `DLSSNR_LiteDX12.ini`

NR 另需兼容的 `nvngx_dlssnr.dll`（模型）。游戏自己不调用 NGX 时还需要一个 NGX 核心（`_nvngx.dll`，或注册表 `HKLM\System\CurrentControlSet\Services\nvlddmkm\NGXCore` 指向的目录）。

Insert 打开面板；NR 默认关闭。日志文件 `DLSSNR_LiteDX12.log` 每次进程启动重写（不再跨会话累积），位于当前生效的配置目录；游戏目录不可写时回退到配置的用户档案目录。由 `[Diagnostics] Level` 控制：`0` 不建文件、不拼接字符串；`1` 记录启动、失败和状态变化；`2` 额外输出详细诊断与调试器行。旧的 `[General] Log=0/1` 在缺少 `Level` 时仍然生效。

`Model parameters → Model preset`：Default / 0 由模型决定，A / 1 为原模型路线，B / 2 为新版优化模型。310.9.16.0 的 Default 选择 B；310.8.0.0 没有 B，会回退到其原模型。此项与 `Style` 分开；切换 preset 会重建模型历史。

## 范围

- **只做 Direct3D 12。** 不包含 DX11 原生捕获、DX11→DX12 桥接、光流 MV 估计、深度查找、白点读回测量，也不含任何测试工程。
- 输入只来自游戏自身的 NGX SR（feature 1）或光线重建（feature 13）evaluate；没有无 DLSS 的合成路径。光线重建允许资源名回退到 `DLSSD.Output`／`DLSSD.Depth`／`DLSSD.MotionVectors`。
- 参考实现见工作区 `_ref/OptiScaler_DLSSNR`，面板与管线沿用 `DLSSNR_Lite`。


## Separate HDR10 build

Standard: `./Build.ps1` produces `dist/Release`. HDR10 variant:
`./Build.ps1 -HDR` produces `dist/Release-HDR`. These use separate CMake build
folders and the same runtime filename for compatibility with the thin proxy.
Install one variant at a time. The standard build does not compile the HDR source
or shader and preprocessing removes HDR members, configuration reads, UI and
Present/resize/color-space branches entirely. No runtime HDR switch exists.

Deploy with `./tools/deploy.ps1 -Target <name>`. Remnant 2 and Manor Lords are
listed as HDR targets, so the script selects `dist/Release-HDR` for them
automatically; every other target gets `dist/Release`. `-HDR` forces the HDR
variant for any target and `-Standard` forces the standard one. Running the
script with no `-Target` prints, per game, which variant it should hold and
which one it currently holds.

The HDR build automatically attempts final-image SDR to HDR10 conversion. Windows
Use HDR must be enabled; disable Windows Auto HDR and RTX HDR for this game.
Requires an SDR RGB10A2 flip-discard chain, blocking Present and one direct queue.
Unsupported paths are skipped and logged. Native non-SDR color space requests
bypass conversion. Frame generation compatibility remains unverified.

HDR mapping uses the inverse BT.2446 Method A algorithm:
luma/chroma reconstruction in BT.2020 with SdrReferenceNits (default 100) and
PeakNits. Domain, radicand and black guards are local safety additions, not
additional artistic highlight curves. A small gap at the rounded lower-knee
boundary is held to prevent a non-monotonic inverse ramp.

InputEncoding: 0 sRGB, 1 Gamma 2.2, 2 Gamma 2.4 (default; zero-black BT.1886
approximation). Game backbuffer encoding must be selected correctly. The graph
uses neutral encoded RGB 0..1, with actual nits readouts at 0.5 and 1.0. Shared
scalar C++/HLSL equations keep the graph aligned with the shader; colored pixels
also undergo the algorithm's chroma/gamut treatment. Hidden panels skip curve/UI
work; HDR conversion continues.

INI keys: InputEncoding=0/1/2, PeakNits=1000, SdrReferenceNits=100. The old custom
curve and its controls are removed; legacy PaperWhiteNits/HighlightStart/HighlightPower/
HighlightStrength/Enabled keys are ignored, as are the removed Method, InputGrayLinear
and OutputGrayNits keys. Slider changes apply live and save on release. Only HDR builds
compile these modules. This still processes UI with the final SDR image and
cannot recover scene highlights discarded before Present. Game validation is
performed by the user.
