# AMD 680M Clone Overlay Probe

> **性质：一次性可行性探针（throwaway test），不是正式提词器。**
>
> 目的：验证 Windows `Win + P -> 复制`（Clone/Duplicate）模式下，AMD Radeon 680M / WDDM 3.x 驱动是否允许一个 DirectComposition flip-model swap chain 通过 DXGI 的 `RESTRICT_TO_OUTPUT` 只出现在某个物理输出上。

## 为什么这版比单纯“赌 MPO”更值得测

Windows DXGI 有一个公开但很少被普通桌面软件使用的能力：

- 创建 flip-model swap chain 时传入一个 `IDXGIOutput* pRestrictToOutput`
- `Present1()` 时带 `DXGI_PRESENT_RESTRICT_TO_OUTPUT`

微软文档对这个标志的描述是：内容只显示在指定 output，在其它 output 上不可见/被黑掉。

这并不保证在 **Clone 拓扑** 下 AMD 驱动一定按我们希望的方式工作，但它比“普通透明窗口碰运气被 MPO promotion”更直接，所以本程序把它作为 **模式 3**。

## 编译

需要 Windows 10/11 + Visual Studio 2022（勾选 **Desktop development with C++**）。

最简单：

1. 双击 `build.bat`
2. 成功后目录里出现 `AMDCloneOverlayProbe.exe`

如果 `build.bat` 找不到编译器：

1. 打开 `x64 Native Tools Command Prompt for VS 2022`
2. `cd` 到本目录
3. 运行 `build_cl.bat`

> 本探针只调用 Windows 自带 Win32 / Direct3D 11 / DXGI / DirectComposition API，没有第三方运行库依赖。

## 正确测试步骤

### 1. HDMI 连接好

先确认笔记本内屏和 HDMI 外接屏都正常。

### 2. 开真正的复制模式

按：

`Win + P -> 复制`

**不要用扩展。**

### 3. 运行程序

启动 `AMDCloneOverlayProbe.exe`。

程序有两个窗口：

- 一个控制窗口
- 顶部一个青色测试条（测试内容）

### 4. 先验证基线

按 `1`：Layered GDI baseline。

预期：青色条在 **笔记本 + HDMI 两边都出现**。

如果连模式 1 都不是两边一样，请先不要继续，说明你的 Clone/输出链路本身有特殊情况。

按 `2`：DirectComposition，不限制 output。

预期：仍然两边都出现。

### 5. 关键测试

按 `3`：DirectComposition + `RESTRICT_TO_OUTPUT`。

然后用：

- `[` 上一个 DXGI output
- `]` 下一个 DXGI output

把所有 output 都试一遍。

你要找的结果是：

**A. 最理想**

- 笔记本：青色条存在
- HDMI：青色条彻底不存在，桌面仍正常

这意味着：有戏，可以继续做真正的“复制屏隐形提词器”。

**B. 次理想**

- 笔记本：青色条存在
- HDMI：对应位置出现黑块

这说明 per-output restriction 生效了，但输出抑制语义不保留透明 alpha。仍值得继续研究 DirectComposition visual / alpha / plane 路径。

**C. 没区别**

- 两边都有青色条

说明当前驱动在 clone 路径里没有按物理 target 分开处理这个 composition present，或者我们选中的 output 没对应到想要的物理 target。

### 6. R 键一定要试

在模式 `3` 下按 `R`：

- ON：`Present1` 带 `DXGI_PRESENT_RESTRICT_TO_OUTPUT`
- OFF：swap chain 创建时仍有 restricted output，但 Present 不带 restriction flag

如果 ON/OFF 在 HDMI 上表现发生变化，就是非常有价值的证据。

### 7. C 键只是对照实验

`C` 切换 `WDA_EXCLUDEFROMCAPTURE`。

这个功能正常情况下只影响截图/录屏等捕获路径，理论上不该改变真实 HDMI Clone。

如果它竟然改变了 HDMI 行为，也请记录下来。

## 快捷键

| 键 | 功能 |
|---|---|
| `0` | 隐藏测试条 |
| `1` | 普通 Layered GDI 基线 |
| `2` | DirectComposition，不限制 output |
| `3` | DirectComposition + restricted output |
| `[` / `]` | 切换物理 DXGI output |
| `R` | 开关 `DXGI_PRESENT_RESTRICT_TO_OUTPUT` |
| `C` | 开关 `WDA_EXCLUDEFROMCAPTURE` |
| `F5` | 重新检测显示拓扑 / output |
| `Esc` | 退出 |

## 日志

程序同目录会生成：

`clone_overlay_test.log`

它会记录：

- Windows 是否检测到多个 active display path 共用同一个 source（Clone）
- QueryDisplayConfig 的 source / target / output technology
- DXGI adapter / output 列表
- 当前 restricted output
- DirectComposition / Present1 的 HRESULT 错误

测试结束后，最好把：

1. `clone_overlay_test.log`
2. 笔记本屏幕照片
3. HDMI 大屏照片
4. 当时模式编号、output 编号、R 是 ON 还是 OFF

一起发回来。

## 一个重要提醒

`RESTRICT_TO_OUTPUT` 是微软公开的 DXGI 能力，但文档主要描述“一个 swap chain 在不同 output 上的可见性”，并没有承诺 **Clone/Duplicate 拓扑** 一定能实现“同一个桌面 source 上不同 physical target 显示不同 composition plane”。所以这个程序的任务就是把这件事在你实际的 AMD 680M 驱动上测死，而不是假设它一定成功。

---

## 不想安装几 GB 的 Visual Studio？

项目已经内置 GitHub Actions 在线编译：`.github/workflows/build-windows.yml`。

具体步骤见 `GITHUB_ACTIONS_CN.md`。上传到 GitHub 后，由 GitHub 的 Windows Runner 编译，构建成功后直接从 Actions 的 Artifacts 下载 `AMDCloneOverlayProbe.exe`，本机不需要安装 Visual Studio/CMake。
