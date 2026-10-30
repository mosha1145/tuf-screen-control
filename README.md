# TUF Screen Control

> 面向华硕 ASUS InfoHub LCD 一体式水冷散热器的第三方开源屏幕控制工具。

TUF Screen Control 通过 USB HID 与设备直接通信，不依赖官方后台服务，
为水冷屏幕提供视频与图片播放、亮度与显示方向控制、自定义文本与时钟叠加等能力。
项目目标是提供一个轻量、稳定、界面现代的控制工具，作为官方 InfoHub 的替代方案。

当前版本：**v1.0**（Python 版，最终 Python 版本）

---

## 适配状态

本项目**仅支持**下面这一款设备，其他型号未经实机验证，不在支持范围内。

| 型号 | InfoHub 软件版本 | 状态 |
|---|---|---|
| **TUF Gaming LC III 360 ARGB LCD** | v1.0.0.15 | 已实测支持 |

> 设备标识：USB HID，`VID 0x0B05` / `PID 0x1C7B`。

> **设备约束**：屏幕原生分辨率固定为 **320 × 320**，设备只接受这一尺寸的画面，
> 且不保留最后一帧（需持续发送才能维持显示）。
>
> **软件行为**：素材的缩放与适配**全部由本软件在发送前完成** —— 软件先把画面处理成
> 320 × 320，再推送给设备。非正方形素材的处理方式取决于所选的**显示适配模式**：
> 默认「适应」为等比缩放后居中填充黑边，「填充」为等比缩放后居中裁剪，
> 「拉伸」为直接拉伸到全屏（会变形）。设备端不做任何缩放、裁剪或适配。

---

## 功能

### v1.0（Python 版）

- 视频 / 图片播放（MP4、JPG）
- 配套素材库：导入时自动转码与缩放
- 显示适配模式：适应 / 填充 / 拉伸
- 画面旋转（0° / 90° / 180° / 270°，适配不同安装方向）
- 亮度调节、屏幕开关
- 播放模式：单曲循环 / 列表循环
- 自定义文本与时钟叠加（位置、字号、颜色、透明度可调）
- Windows SMTC 媒体封面捕获
- 桌面实时投射
- 配置自动持久化，启动时恢复上次显示内容
- HID 直接通信，不依赖官方后台服务

### v2（C++ / WinUI 3，开发中）

v2 为原生重写版本，当前处于工程骨架阶段：

- HID 协议层（原生 Win32，不依赖 .NET 运行时）
- 帧处理层（几何变换、JPEG 编码）
- 配置系统（含 v1 → v2 配置迁移）
- WinUI 3 界面：Mica 系统背衬、NavigationView 导航、深色 / 浅色 / 跟随系统

---

## 依赖

- Python 3.10+
- PySide6
- OpenCV（opencv-python）
- NumPy
- FFmpeg（视频处理，将 `ffmpeg.exe` / `ffprobe.exe` 放入 `tools\` 目录，
  或安装并加入 `PATH`）

---

## 使用

```bash
pip install PySide6 opencv-python numpy
python tuf_gui.py
```

或双击 `启动GUI.bat`。

> 注意：`启动GUI.bat` 中的 Python 解释器路径需按本机环境调整。

---

## 项目结构

| 文件 | 说明 |
|---|---|
| `tuf_gui.py` | 应用主体：界面、播放线程、素材管理、配置编排 |
| `tuf_hid.py` | HID 协议封装：设备枚举、控制命令、图像分块发送 |
| `asset_lib.py` | 素材库与画面处理：转码入库、适配、旋转、JPEG 编码 |
| `config_store.py` | 配置读写与默认值合并 |
| `overlay.py` | 画面叠加层：时钟、自定义文本 |
| `theme.py` | 界面设计令牌与样式 |
| `tuf_direct.py` | 纯 JPEG 直发验证脚本（协议探针，非应用组成部分） |
| `PROTOCOL_REVERSE_REPORT.md` | HID 协议逆向报告（数据包格式、命令、通信机制） |
| `project_understanding_v2.md` | v2 架构理解与重构规划 |
| `v2/` | v2（C++ / WinUI 3）工程源码与 MSBuild 解决方案 |

---

## 构建（v2）

命令行直接构建（无需打开 Visual Studio 解决方案）：

```bat
"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" ^
    v2\src\ScreenControl.App\ScreenControl.App.vcxproj /p:Configuration=Debug /p:Platform=x64
```

输出：`v2\out\x64\Debug\TufScreenControl.exe`

> **移动工作区后首次构建**：请先删除 `v2\out\obj`，否则中间产物里缓存的旧路径
> 会导致 `RC2135: file not found ... .exe.embed.manifest`。
>
> 生成目录约定：C++/WinRT 头文件与 XAML 生成代码统一落在 `v2\out\generated\`，
> 编译中间文件在 `v2\out\obj\`，均已被 `.gitignore` 忽略。

---

## 未来规划

v1.0 是最后一个 Python 版本。后续将以 **C++ / WinUI 3** 原生重写（v2），
以获得更低的启动开销、更稳定的帧率表现与更现代的界面体验，
并以独立大版本发布。

---

## 声明

本项目为个人开源项目，与华硕（ASUS）官方无关，不包含官方 InfoHub 软件的任何代码。
所有协议信息均来自对公开软件的行为分析与实机抓包验证。请遵守设备厂商的使用条款。
