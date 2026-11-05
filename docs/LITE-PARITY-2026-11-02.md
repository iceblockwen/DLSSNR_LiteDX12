# DX12 反向优化：与 Lite 对齐

日期：2026-11-02
范围：`DLSSNR_LiteDX12`（主）、`DLSSNR_Lite`（同步改动）

---

## 背景

`DLSSNR_Lite` 合并工程（DX11 + DX12 单 DLL）在开发过程中对 `DLSSNR_LiteDX12`
做过多处修正与精简。本次把那些修正反向搬回 DX12 工程，并顺带清掉由此产生的
死代码。

**结论先行**：DX12 侧每一项都已经在 Lite 里验证过；本次没有引入新机制，全部是
「删掉 DX12 里多余的那一套」或「改名对齐」。

---

## 一、真 Bug：工厂对象替换（Alan Wake 2 设备丢失）

### 症状

AW2 启动后设备丢失，`0x887A002B`（`DXGI_ERROR_ACCESS_DENIED`）。

### 根因

`AttachFactory` 把游戏拿到的原生 DXGI 工厂**换成了自己的 COM 代理对象**：

```cpp
// 修复前 hooks.cpp
auto* wrapper = new capture::Factory(factory.Get());
void* queried{};
const auto hr = wrapper->QueryInterface(iid, &queried);
wrapper->Release();
if (SUCCEEDED(hr) && queried) { object->Release(); *output = queried; }
```

游戏后续拿这个外来 `IDXGIFactory7` 做自己的 DXGI 调用就失败。

### 为什么是纯多余

`AttachFactory` 里的 detour 段已经把原生工厂的 4 个创建方法（vtable 槽
10 / 15 / 16 / 24）全部钩住，`Create::Call` 内部调 `WrapSwapchain` 包装返回的
交换链。**对象替换是第二套做同一件事的机制**，且是唯一会破坏游戏状态的那套。

### 修复

游戏保留原生工厂对象，一个字节都不替换：

| 位置 | 改动 |
| --- | --- |
| `hooks.cpp` `AttachFactory` | 删 `new capture::Factory` + `QueryInterface` + `object->Release(); *output = queried;`（6 行） |
| `hooks.cpp` `AttachFactory` | 删 `capture::FactoryIID` 识别检查（1 行） |
| `hooks.cpp` | 首参 `REFIID iid` → `REFIID`（匿名，避免 C4100） |
| `wrappers.hpp` | 删 `class Factory final : public IDXGIFactory7`（**86 行**） |
| `wrappers.hpp` | 删 `FactoryIID` 常量（1 行） |
| `wrappers.hpp` | 头注释改为说明「为何不替换工厂对象」 |

`wrappers.hpp`：**241 行 → 152 行**（删 Factory 后 157，再删适配器捕获链后 152）。

---

## 二、删除的功能分支

### 2.1 `partialColorOutput`

原本用于拒绝「游戏声明只输出部分颜色区域」的帧。

| 位置 | 改动 |
| --- | --- |
| `pass.hpp` `PassStatus` | 删 `partialColorOutput` + `colorWidth` / `colorHeight` / `colorRegionWidth` / `colorRegionHeight` / `colorX` / `colorY`（7 个字段） |
| `pass.cpp` `Record` 入口 | 删 `status.partialColorOutput = false;` |
| `pass.cpp` | 删整段检测 + `refuse(E_NOTIMPL, "Partial NR color output is not supported")` |
| `model_settings.cpp` `PublishPass` | 删该字段参与的去重条件 + 专门的日志行 |

**判断依据**：`guides.originX[0]` / `guides.width[0]` 在 DX12 的 `pass.cpp` 里
**一次都没被读取**，所以那段代码并没有保护任何 resolve 路径 —— 它只是「遇到
partial 就整帧跳过」。

### 2.2 `applyModel`

原本允许「NR 照常求值但把游戏原帧留在屏幕上」。

| 位置 | 改动 |
| --- | --- |
| `pass.hpp` `PassSettings` | 删 `bool applyModel=true` |
| `pass.cpp` | `if (settings.applyModel) { ... } else { ... }` → 直通 `Transition` / `readable` / `final` |
| `model_settings.cpp` | 删 INI 读取 `ApplyModel` + `Save(L"ApplyModel", ...)` |
| `panel_ui.cpp` | 删「Apply NR result」复选框 + 提示 |
| `DLSSNR_LiteDX12.ini` | 删 `ApplyModel=1` 及说明注释 |

### 2.3 `GpuTicket::waitedFence` / `PrepareSubmission` 的 `waited` out 参数

`waitedFence` 只在 `PrepareSubmission` 里被赋值，从不读取；`capture12.cpp` 里
接收它的 `bool waited{}` 也从不读取。

| 位置 | 改动 |
| --- | --- |
| `gpu_ticket.hpp` | 删 `waitedFence` 字段 |
| `gpu_ticket.hpp` | `PrepareSubmission(queue, bool* waited)` → `PrepareSubmission(queue)` |
| `capture12.cpp` | 删 `bool waited{}`，调用点同步 |

控制流核对：`auto predecessor=dependency` 仍保证 `dependency` 的锁生存期，
`queue->Wait` 仍只在「跨队列且未完成」时调用。

### 2.4 `retry_once.hpp`

11 行模板，全工程只有 `proxy.cpp` 一个调用点。

`ResolveDxgi` 内联为：

```cpp
DWORD error = ERROR_SUCCESS;
try { std::call_once(systemOnce, [] { ... }); }
catch (DWORD code) { error = code; }
catch (...)       { error = ERROR_GEN_FAILURE; }
systemLoadError = error;
```

顺带拿到更严密的异常兜底：原来的 `RetryOnce` 只捕 `DWORD`，别的异常类型会穿透
到游戏的 `GetProcAddress` 调用栈。

---

## 三、行为变化（必须知道）

### 3.1 NR 现在总是应用结果

删掉 `applyModel` 后，**没有「只评估不应用」这个模式了**。`ApplyModel=0` 的
现存配置会静默地不再生效，NR 一律写回颜色。

### 3.2 声明了 partial color output 的帧不再被跳过

原来整帧 `E_NOTIMPL` 拒绝，现在按整帧处理。正常游戏不会在 DLSS 里声明 subrect，
但如果某个游戏画面出现异常，这是第一个要回看的地方。

---

## 四、结构性清理

### 4.1 `Debug*` → `Preview*` 改名

14 个标识符，9 个文件：

| 旧 | 新 |
| --- | --- |
| `DebugStatus` | `PreviewStatus` |
| `DebugActive` | `PreviewActive` |
| `DebugSnapshot` | `PreviewSnapshot` |
| `SyncDebugVisibility` | `SyncPreviewVisibility` |
| `ResetDebugHistory` | `ResetPreviewHistory` |
| `debugGeneration` | `previewGeneration` |
| `debugVisibility` | `previewVisibility` |
| `debugTexture` / `debugView` / `debugWidth` / `debugHeight` / `debugError` / `debugDisplayed` | `preview*` |
| 局部 `debug`（capture12.cpp） | `preview` |
| 局部 `missingDebug` | `missingPreview` |

日志字段 `model=%u debug=%u` → `model=%u preview=%u`。

**故意保留**：`UnavailableDebug`、`DXGIGetDebugInterface1` —— DXGI 导出名，改了会挂。

### 4.2 `d3d12_util` → `gpu_util`

* `src/nr/d3d12_util.hpp` / `.cpp` → `gpu_util.hpp` / `.cpp`（文件改名）
* 6 个 include 同步（`capture12_input.hpp` / `frame_input.hpp` / `ngx_parameters.hpp` /
  `preview.hpp` / `hdr_output.cpp` / 自身）
* `CMakeLists.txt` 源列表同步

### 4.3 适配器捕获链（DX12 与 Lite 同步删除）

删掉 `class Factory` 后，它专用的适配器捕获链失去唯一入口：

```
wrappers.hpp  CaptureAdapterInterface()   ← 唯一调用者是 class Factory 的 EnumAdapter* 三方法
     ↓
hooks.cpp     CaptureAdapter()            ← 零调用者
     ↓
hooks.cpp     HParent = Hook<Parent,...>  ← 永不 Stage
hooks.cpp     struct Parent               ← CallFrom 永不执行
hooks.cpp     lastParent                  ← 只剩声明
hooks.cpp     Finish() 里的 HParent::Finish(success)  ← 空转
```

约 20 行，运行时一次都不会执行。

**语义代价**：`IDXGIAdapter::GetParent`（vtable 槽 6）是第三条发现工厂的路径。
现在仍有另外两条：`proxy.cpp` 的 `WrapFactory`（拦截 `CreateDXGIFactory` /
`1` / `2` 三个导出）和 `hooks.cpp` 的 `FactoryExport::CallFrom`。

**保留**：`WithParentBypass` / `parentBypass` —— 这是 Present 期间屏蔽 `GetParent`
重入的另一套机制，仍在使用。

---

## 五、明确**不做**的事

以下三项在 Lite 里存在差异，但**不能反向搬回 DX12**：

### 5.1 `WhiteMode::Automatic` —— 保留

```cpp
enum class WhiteMode : unsigned {Automatic, Game, Measured, Manual};   // DX12
enum class WhiteMode : unsigned {Game, Measured, Manual};              // Lite
```

DX12 的枚举是 **1/2/3 有语义、0 是历史遗留**，`model_settings.cpp` 显式把
`Automatic` 折叠成 `Game`。删掉会让现存配置里的 `WhiteMode=2`（Measured）
变成 `Manual`。

**两个工程的 INI `WhiteMode` 数值互不兼容**，不要互相拷贝 INI。

### 5.2 `sampledPre` / `sampledPreValid` —— 保留

DX12 独有（8 处）。它用 GPU 回读得到的 `completed->exposurePre` 替换白点计算里
的 `whiteExposure.pre`（滞后一帧但真实）；Lite 直接用实时 NGX 值。删掉会改变
白点结果，不是精简。

### 5.3 `inFlight` —— 保留

两个工程都有，负责持有已提交但未退休的 `Recording`，保护 feature 生命周期。

### 5.4 HDR —— 保留（用户决定）

`hdr_output.cpp` / `hdr_output.hpp` / `hdr_ui.cpp` / `lite_hdr.hlsl` / `hdr_math.h`
+ `LITEDX12_HDR` 构建变体 + `[HDR]` INI 段 + `windows-release-hdr` preset +
`Build.ps1 -HDR` + `Licenses/RenoDX_HDR_MIT.txt` 全部保留。

---

## 六、代理层：补齐备用入口

DX12 原来只产出 `dxgi.dll`。现在与 Lite 一致，多两个注入入口。

| 新文件 | 说明 |
| --- | --- |
| `src/proxy/version.def` | 17 个导出，ordinal 1-17 |
| `src/proxy/winmm.def` | 180 个导出 |
| `src/proxy/proxy_common.hpp` | `BootRuntime` / `LoadOriginal` / `BootFailure` |
| `src/proxy/proxy_forward.cpp` | 通用转发入口 |

`CMakeLists.txt` 新增 `litedx12_entry(name)` 函数：configure 期从 `.def` 解析导出名
和序号，生成 `proxy_exports.generated.hpp` + `proxy_<name>.asm`，并检查
`Proxy<N>` 必须落在位置 N（否则 `FATAL_ERROR`）。

**对齐结果**（把工程名/命名空间替换成占位符后逐行比对）：

| 文件 | 结果 |
| --- | --- |
| `proxy.cpp` | 仅 `#include` 路径不同（目录布局） |
| `proxy_common.hpp` | 完全相同 |
| `proxy_forward.cpp` | 完全相同 |
| `version.def` / `winmm.def` | 字节级相同 |
| `dxgi.asm` / `dxgi.def` | 字节级相同 |

---

## 七、产物与部署

### 二进制体积

| 阶段 | `nvngx.dll_litedx12.dll` |
| --- | --- |
| 优化前 | 1130496 |
| 删 Factory 后 | 1122304 |
| 删 partial / applyModel / waited 后 | 1121792 |
| 改名 + 适配器链清理后 | 1121792（不变，删的都是链接器本来就会剔除的） |

Lite：`nvngx.dll_lite.dll` 1395200（DX11 + DX12 全栈）。

### 部署（Alan Wake 2）

```
G:\EpicGames\AlanWake2\
  dxgi.dll                171008
  nvngx.dll_litedx12.dll  1121792
  DLSSNR_LiteDX12.ini     保持用户配置不动
```

**不要**同时放 `dxgi.dll` + `version.dll` + `winmm.dll` —— 三个代理都会去
`Start` 运行时，`call_once` 虽然挡住重复初始化，但没有意义。

回退点：`H:\AgentWorkPlace\DLSS5\_backup\20261102-212151-aw2-deploy\`

---

## 八、验证状态

| 项 | 状态 |
| --- | --- |
| 标准构建 | 零错误零警告（`/W4`） |
| **HDR 变体构建** | 零错误零警告（改名碰过 `hdr_output.cpp` 的 include，专门单独验证） |
| `ninja -n` | `no work to do` |
| 死符号扫描 | 头文件声明的自由函数全部有调用者 |
| INI 键 ↔ 代码读取 | 双向核对，无孤儿键、无未声明读取 |
| AW2 实机 | 用户确认可用（INI `Level=0`，无日志） |

---

## 九、白点模式重编号 + 反向进着色器 + 测量解耦

三个工程同时改（`DLSSNR_LiteDX12` / `DLSSNR_Lite`）。这一节取代第四节之前
关于白点的所有描述。

### 9.1 模式重编号（用户选 C：不迁移旧 INI）

```cpp
enum class WhiteMode : unsigned {Game, Measured, Manual};  // 0 / 1 / 2
```

- 删除 `Automatic`。旧 DX12 的 1/2/3 与 Lite 的 0/1/2 互不兼容的问题就此消失。
- INI 注释改为 `0 Game, 1 Measured, 2 Manual.`；两张 INI 的 `WhiteMode` 都是 `0`。
- `SetModelSettings` 钳制改为 `if (unsigned(w.mode) > 2) w.mode = WhiteMode::Game;`。
- 顺带修掉 Lite 面板的既有 Bug：枚举本来已是 0/1/2，面板却还在 `int(mode) - 1`
  / `WhiteMode(whiteMode + 1)`，导致下拉显示错位（Game 时空显示、选 Game 实际
  设成 Measured）。现在两边都是直通转换。

### 9.2 反向不再走 CPU 测量

旧的 DX12 反向路径靠 `WhiteMeter12` 把曝光纹理的值搬到 `grid[0]`，再用
`reverseMeasurement` 借道测量模式做回读，然后在 CPU 上重算 `white`。共 8 处：

| 删除 | 位置 |
| --- | --- |
| `exposurePre` 局部 | `nr/pass.cpp` 原 L155 |
| `sampledPre` / `sampledPreValid` | 原 L219-220 |
| `sampledPreValid = false` ×2 | 原 L418 / L481 |
| `if (status.gpuExposureValid) { sampledPre = ... }` | 原 L508-511 |
| `whiteExposure` / `whiteSettings` override 块 | 原 L533-544 |
| `recording->exposurePre = exposure.pre;` | 原 L747 |

现在只有一条路径：`live` 判定 + `c.live = reverseRatio ? 2u : 1u`，着色器里算：

```hlsl
// liveExposure 1 = pre/exposure, 2 = exposure/pre。纹理值本身从不取倒数，
// 只是换到分数的另一侧。
float ratio = max(liveExposure == 2 ? e / max(preMul, 1e-8)
                                    : preMul / max(e, 1e-8), 1e-8);
v = clamp((exponent == 1.0 ? ratio : pow(ratio, exponent)) * trim, 1e-8, 1024);
```

**关键点**：`e` 是 NGX `DLSS.ExposureTexture` 的原始值（零成本，与测量无关），
`preMul` 是 `DLSS.Pre.Exposure`。反向只交换分子分母。

`Recording::liveExposure` 从 `bool` 改成 `unsigned`（0/1/2），`nr/pass.cpp`
`recording->liveExposure = c.live;`（原来是 `= live`，丢掉了 1/2 的区别）。

### 9.3 测量与 Game 模式彻底解耦

```cpp
const bool modelMeasurement = exposure.sceneLinear && settings.white.mode == WhiteMode::Measured;
recording->modelMeasurement = modelMeasurement;

const bool measure = recording->modelMeasurement || settings.panelStatistics;  // panelStatistics == PanelVisible()
if (!measure) meter.reset();
if (measure && (frameIndex++ % clamp(settings.white.every, 1u, 120u) == 0)) { ... }
```

| 场景 | 测量 | 白点算在哪 |
| --- | --- | --- |
| 面板开（任意模式） | ✅ 每 `white.every` 帧 | Game 正/反向仍由着色器算；回读只供面板显示 |
| 面板关 + Game + 有纹理 | ❌ | 着色器（`liveExposure` 1/2） |
| 面板关 + Game + 无纹理 | ❌ | CPU `WhitePoint::Resolve`（标量 pre/scale） |
| 面板关 + Measured | ✅ | CPU（`logAverage`） |
| 面板关 + Manual | ❌ | CPU（`manual`） |

`frameIndex++` 只在 `measure` 为真时递增，`every` 的含义是「第 N 个需要测量的帧」。
面板关闭 + Game 时 `meter` 被释放（`meter.reset()`），零回读开销。

### 9.4 面板读数

- `gpuDivisor` 显示公式按反向切换：`liveExposure == 2 ? exposure / preMultiplier
  : preMultiplier / exposure`，再 `pow(·, exponent) * trim` 并钳制到
  `[1e-8, 1024]`。
- `liveWhite` 判定去掉了 `!reverseRatio` —— 反向现在也是 GPU 实时值。
- `Exposure ratio` 的提示文案改为「两侧都在 GPU 上实时求值，纹理值从不取倒数」。

### 9.5 验证

| 项 | 状态 |
| --- | --- |
| `DLSSNR_LiteDX12` 标准构建 | 零错误零警告 |
| `DLSSNR_LiteDX12` HDR 变体 | 零错误零警告 |
| `DLSSNR_Lite` 构建 | 零错误零警告 |
| 两工程 `nr/pass.cpp` ↔ `model/pass.cpp` | `liveExposure` / divisor / 测量 gating 逐行一致 |
### 9.6 复查补丁（三处）

| # | 问题 | 修法 |
| --- | --- | --- |
| 1 | `Pre.Exposure` 缺失或为 0 时，着色器仍把它当除数用（`Clamped` 兜到 `1e-8`），反向会得到 `white ≈ 0` → 近黑画面。CPU 标量路径有 `Positive(pre)` 守卫，着色器没有。 | 取 `preUsable = exposure.prePresent && exposure.pre > 0`，不满足就 `preMul = 1`，与标量路径一致。`prePresent=false` 时 `pre` 本来就是 1，所以正常游戏行为不变。 |
| 2 | `gpuDivisorValid` 只在成功采样时置 `true`，历史重建（recording discarded）和 reset 时不清，面板会显示重建前的旧白点。 | 两处 `status.gpuExposureValid = false` 合并为 `status.gpuExposureValid = status.gpuDivisorValid = false`。 |
| 3 | `white_point.hpp` 的 `exponent` 注释第二行缩进错位（正则替换留下 2 个多余空格）。 | 对齐。 |

另修正 9.4 的措辞：`Last completed sample` 已改名为 `Last completed white`（那是算出来的白点除数，不是纹理样本）。

### 9.7 复查确认（无需改动）

- `frame.exposureTexture` 的屏障由 `capture12.cpp` 的 `reads.Add(input.exposureResource, input.exposureState)` 声明、`reads.Restore()` 恢复；`directExposure` 为假时还会先 `CopyShaderInput12` 转成通用格式。之前担心的「t3 没有 barrier」不存在。
- `SameTuning()` 只比较模型参数（preset/style/mask/intensity/structure/tone/skin），不含任何 `WhiteSettings` 字段，所以切换反向、exponent、trim 都不会触发 feature 重建，没有 1 帧卡顿。
- `frameIndex` 只服务测量节流，不参与其他逻辑。
- 两工程 `pass.cpp` 去掉空白后仍只差 4 类：include/namespace、Lite 的 `orderProven()`（DX11 支持）、`status.generatedMotion`（DX11）、以及由前者派生的两处 `canFollow` 表达式。白点相关 109 行逐字一致。
### 9.8 第二轮复查

代码层面没有新 Bug。发现的是**已部署配置里的陈旧注释**（改代码时只更新了项目模板 INI，没同步到 AW2 那份）：

| 位置 | 原文 | 问题 |
| --- | --- | --- |
| AW2 INI `WhiteMode` 上方 | `; 1 Game, 2 Measured, 3 Manual. Legacy 0 maps to Game.` | 紧挨 `WhiteMode=0`，但 0 现在就是 Game，注释还说是「legacy 映射」 |
| AW2 INI `WhiteRatio` 上方 | `; ... 1 reverse ratio from completed readback.` | 反向已不再走回读 |
| 两张模板 INI `WhiteExponent` 上方 | 只给了正向公式 | 没说明反向是 `(exposure/pre)^n * trim` |

三处已改，AW2 的用户数值（`WhiteMode=0`、`WhiteRatio=0`、`WhiteUseTexture=1`、`WhiteUsePreExposure` 缺省、`WhiteUseExposureScale` 缺省）保持不动。

复查通过、无需改动的项：

| 检查点 | 结论 |
| --- | --- |
| INI 键完整性 | 两工程都是 **25 个键读取 == 25 个键写入**，完全对称；无孤儿键、无未声明读取 |
| `PassStatus` 白点字段 | `preExposure`/`exposureScale`/`gameExposureTexture`/`gpuExposure`/`gpuDivisor`/`whiteMeasurementError`/`white.*` 逐个查过，全部有读者 |
| `Exposure` 串帧风险 | `DescribeExposure()` 每次全新构造（`pre=1`、`prePresent=false` 默认），`pass->Record(..., input.exposure, ...)` 与 `input.exposureResource` 同源同帧 |
| 配置写入压力 | `ConfigWriter::Queue` 有 150ms 合并窗口 + 与已落盘值去重 + 3 次重试，拖滑块不会砸盘 |
| 着色器 `White()` 调用次数 | 每 dispatch 每像素刚好 1 次（mode 0 编码 1 次、mode 1 解算 1 次）；`c` 结构在两次 `run()` 之间复用，含 `c.live`/`c.preMul`，编码与解算不会用不同的白点 |
| 数值边界 | `pow(1e14, 4)` 溢出为 `inf` → `clamp` 到 1024，不产生 NaN；`ratio` 有 `max(·, 1e-8)` 下界 |
| 白点 tab UI | 两工程整块（154 条归一化行）**逐字相同** |
### 9.9 面板与配置收尾

**1. 隐藏只在 Measured 生效的滑块**（两工程 `panel_ui.cpp` / `panel.cpp`）

`Mid-gray anchor` / `Filter seconds` / `Deadband` / `Attack` / `Release` 这五个只出现在
`WhitePoint::Resolve` 的 Measured 分支里，Game / Manual 模式下拖动毫无作用。现在包进
`if (whiteMode == 1)`。`Measure every N frames` 保持常显 —— 面板打开时任何模式都会测量，
它控制的是回读刷新节奏。

**2. 滑块范围对齐代码钳制**

| 滑块 | 原范围 | 新范围 | `SetModelSettings` 钳制 |
| --- | --- | --- | --- |
| Filter seconds | 0.01–3 | **0.001–60** + 对数刻度 + `%.3g` | 0.001–60 |
| Deadband | 0–0.5 | **0–1** | 0–1 |
| Attack (stops/s) | 0–8 | **0–16** | 0–16 |
| Release (stops/s) | 0–8 | **0–16** | 0–16 |
| Measure every N frames | 1–30 | **1–120** | 1–120 |

`Mid-gray anchor`、`White trim`、`Manual / fallback`、`Game exponent` 本来就已对齐，未动。
`Filter seconds` 跨度 4.8 个数量级，改成对数刻度、格式 `%.3g`（原来 `%.2f` 会把 0.001 显示成 0.00）。

**3. 删掉 AW2 INI 的 `ApplyModel=1`**

该键对应的功能已在第二节删除，代码里再无读取。同时把 AW2 INI 里两条过期注释改成与模板一致。

**验证**：两工程 Release + DX12 HDR 全部零错误零警告；白点 tab 整块 UI 仍是 157 条归一化行
**逐字相同**。
### 9.10 Difference gain 上限 4 → 2

四处同步（两工程）：

| 位置 | 原 | 新 |
| --- | --- | --- |
| 面板滑块 `slider("Difference gain", value, 0, ?, 1)` | 0–4 | **0–2** |
| `SetModelSettings` 钳制 | `Clamped(p.differenceGain, 0, 4, 1)` | `Clamped(p.differenceGain, 0, 2, 1)` |
| 着色器 `ApplyGain` | `clamp(gain, 0, 4)` | `clamp(gain, 0, 2)` |
| INI 注释 | `… up to 4.` | `… 2 double.` |

工具提示同步：`ui_widgets.cpp` 的说明改成「0 shows original, 1 normal NR, 2 doubles the change」，
面板内联提示改成「2: double the NR difference」。滑块的实际范围由 `SliderHelp()` 从
`PanelSliderFloat(label, …, low, high)` 的参数自动打印，无需另外维护。

默认值仍是 1，所以现有配置（含 AW2 的 `DifferenceGain=1`）行为不变；
若旧 INI 写着 3 或 4，`ConfigureModel` 会经 `SetModelSettings` 钳到 2。