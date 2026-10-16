# Hasselblad X Unleashed

> 哈苏 X 系统相机机内能力拓展研究项目
> Exploring and extending in-camera capabilities of the Hasselblad X System.

[English](README.en.md) · **简体中文**

---

## 📌 项目简介

本项目致力于探索哈苏 X 系统（X1D / X2D / X2DII / CFV 等）相机固件的潜力，在机身内实现原厂尚未提供的拍摄与交互功能，让这台精密的中画幅相机拥有更多可能性。

## 🚀 开发进展

| 功能 | 说明 | 状态 |
| --- | --- | :---: |
| 对焦包围（景深包围） | 在机内原有采样功能的基础上进一步强化，自动按步进移动对焦点连拍，用于微距 / 产品摄影的景深合成 | ✅ 已完成 |
| 像素偏移机内合成 | 利用传感器位移多次曝光，并在机身内直接合成高分辨率图像 | ✅ 已完成 |
| RAW 堆栈合成 | 多帧 RAW 的最大值 / 最小值 / 平均值机内合成 | ✅ 已完成 |
| 直连无线打印机 | 无需电脑或手机中转，相机直接通过无线网络连接打印机输出照片 | ✅ 已完成 |
| 机内 NES 游戏 / DOOM | 在相机屏幕上运行 NES 模拟器与经典游戏 DOOM，并支持通过桥接连接 PS5（DualSense）手柄进行游玩 | ✅ 已完成 |
| 机内无线麦克风 | 支持双 DJI MIC 无线麦克风立体声音频输入：内录 48 kHz / 24 bit，相机 Wi-Fi 48 kHz / 16 bit | ✅ 已完成 |
| 外接监视器（UVC） | 相机通过 USB 以标准 UVC 设备输出实时取景画面，电脑即插即用；配套安卓 / iOS 监看 App，内置斑马纹、假彩色、峰值对焦、直方图、画幅遮罩等专业监看工具 | ✅ 已完成 |
| 视频录制 | 解锁机身视频录制能力，机身内直接产出 MP4 | 🚧 开发中 |

> 更多功能持续探索中，欢迎交流。

## 📝 TODO

- [ ] **视频录制与无线麦克风音轨结合** —— 让机内视频录制直接合入无线麦克风的音轨。
- [ ] **后续可能支持 DJI RC 控制** —— 探索通过 DJI RC 遥控器对相机进行操控。

> 相关功能在后续完善后会开源；届时会移除其中可能侵犯权益、或涉及触犯法律的漏洞利用部分，仅保留可公开、合规的实现思路。

## 📂 仓库结构

| 目录 | 内容 |
| --- | --- |
| `hasselblad_mic/` | 无线麦克风：扫描 / 配对、连接管理与状态设置、LC3plus 解码、PCM FIFO、WAV 录音，以及供视频录制使用的音频租约接口 |
| `hasselblad_monitor/` | 外接监视器（相机侧）：UVC gadget 喂帧器与内核运行时防护模块 |
| `hasselblad_monitor_android/` | 外接监视器（安卓监看端）：UVC 取流 + GL 渲染，内置全套监看工具 |
| `hasselblad_x2nes/` | 机内 NES 模拟器与 DOOM，含原厂 GUI 菜单入口注入 |
| `hasselblad_x2print/` | 机内 IPP 无线打印服务（D-Bus 契约、三级打印机发现、生产化部署） |

## 📸 功能截图

### 景深包围 / Pixel Shift 拍摄模式

机内拍摄模式菜单，新增「景深包围」与「Pixel Shift」等选项（传感器位移序列驱动 IBIS）。

![拍摄模式](assets/screenshots/pixel_shift.jpg)

### 堆栈合成

机内直接完成「摇摇乐（Pixel Shift）合成」与「景深包围合成」，输出 TIF。

![堆栈合成](assets/screenshots/stacks.jpg)

### Pixel Shift：Auto4 / Auto6 / Auto16 位移与合成用途

2026-9-28 已通过原厂位移调用、轴向应答／回读和 26 张实拍 RAW 的图像配准，核对以下点位顺序。详细记录见 [原厂多拍位移验证](docs/MULTISHOT_MOTION_20260928.md)。

| 模式 | 已确认的原厂点位设计 | 合成用途 | 当前产品规则 |
| --- | --- | --- | --- |
| Auto4 | 整数四点位移 | 适合约 1 亿像素全彩合成：同一场景位置取得 R、G1、G2、B 实测采样 | 仅逐张保存，供电脑端合成 |
| Auto6 | 在四个整数点位上增加两个半像素点位 | 适合约 4 亿像素合成；增加空间采样，仍需重建算法 | 可选机内 Merged 或逐张 Photos |
| Auto16 | 步长 0.5 像素的 4 × 4 蛇形网格 | 适合更充分的全彩超分辨率重建，也可配准后降噪；电脑端自行选择处理方式 | 仅逐张保存，供电脑端合成 |

逐张保存跟随机身画质中的图像格式设置；参考帧为可选项，不计入上表的 4／6／16 张位移帧。
四帧的全彩合成用途不表示当前提供四帧机内合成。

### 实拍对比：Pixel Shift 合成 vs 普通单张

![Pixel Shift 对比](assets/screenshots/pixel_shift2.png)

同一场景 ISO 6400 实拍（B0001053 / B0001054）：

| 项目 | 1053：Pixel Shift 合成 | 1054：普通单张 |
| --- | --- | --- |
| HEIC 尺寸 | 23310 × 17482（407.51 MP） | 11656 × 8742（101.90 MP） |
| HEIC 编码 | 10 位，4:2:0，8×6 瓦片 | 10 位，4:2:0，4×3 瓦片 |
| RAW 存储 | 23310×17482，16 位单通道 CFA | 11904×8842，16 位单通道 CFA（含传感器边界） |
| ISO / 镜头焦距 | 6400 / 100 mm | 6400 / 100 mm |
| 记录的曝光时间 / 光圈 | 0.00929 s / f4.74 | 0.01000 s / f4.8 |
| 拍摄时间 | 20:13:07 | 20:17:27 |
| 3FR | 819,400,704 字节 | 213,864,448 字节 |
| HEIC | 132,702,208 字节 | 56,125,440 字节 |

> 完整的同区域 400% / 800% 对比见 [`reports/Pixel Shift_compare_1053_1054`](reports/Pixel%20Shift_compare_1053_1054/) 目录（附本地交互版 REPORT.html）。

### 机内无线麦克风

直连 DJI MIC 等无线麦克风，实时监看电量、信号、电平与丢帧，并支持输入增益、低切 / 高切、静音等设置。

![麦克风管理](assets/screenshots/mic_1.jpg)
![麦克风设置](assets/screenshots/mic_2.jpg)

### 直连无线打印机

相机直接连接无线打印机输出照片（演示视频）：

🎬 [`assets/screenshots/print.mp4`](assets/screenshots/print.mp4)

### 机内 NES 游戏 / DOOM

在相机屏幕上运行 NES 模拟器与经典游戏 DOOM，并支持通过桥接连接 PS5（DualSense）手柄进行游玩（演示视频）：

🎬 [`assets/screenshots/nesgame.mp4`](assets/screenshots/nesgame.mp4)

## 🙏 致谢

特别感谢 **[@YuHaoyua](https://github.com/YuHaoyua)** 无私分享 CIM 固件的加密方式及相关密钥，正是这份开放精神，才让本项目成为可能：

👉 [hasselblad-cim-firmware-extractor](https://github.com/YuHaoyua/hasselblad-cim-firmware-extractor)

## ⚠️ 声明

- 为避免有人借此牟利、利用信息差获取不当收益，**本仓库仅公开可行的实现思路**，不涉及相机远程命令执行与 SELinux 绕过的具体说明。
- 本项目**仅供个人参考与学习研究之用**，请勿用于商业目的。
- 相关内容基于对公开信息与自有设备的研究，若有任何内容涉及侵犯哈苏（Hasselblad）的合法权益，请及时与我联系，我会第一时间处理。
