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
| 直连无线打印机 | 无需电脑或手机中转，相机直接通过无线网络连接打印机输出照片 | ✅ 已完成 |
| 机内 NES 游戏 / DOOM | 在相机屏幕上运行 NES 模拟器与经典游戏 DOOM，并支持通过桥接连接 PS5（DualSense）手柄进行游玩 | ✅ 已完成 |
| 机内无线麦克风 | 支持无线麦克风音频输入 | ✅ 已完成 |
| 视频录制 | 解锁机身视频录制能力 | 🚧 开发中 |

> 更多功能持续探索中，欢迎交流。

## 📝 TODO

- [ ] **视频录制与无线麦克风音轨结合** —— 让机内视频录制直接合入无线麦克风的音轨。
- [ ] **后续可能支持 DJI RC 控制** —— 探索通过 DJI RC 遥控器对相机进行操控。

> 相关功能在后续完善后会开源；届时会移除其中可能侵犯权益、或涉及触犯法律的漏洞利用部分，仅保留可公开、合规的实现思路。

## 📸 功能截图

### 景深包围 / Pixel Shift 拍摄模式

机内拍摄模式菜单，新增「景深包围」与「Pixel Shift」等选项（传感器位移序列驱动 IBIS）。

![拍摄模式](assets/screenshots/pixel_shift.jpg)

### 堆栈合成

机内直接完成「摇摇乐（Pixel Shift）合成」与「景深包围合成」，输出 TIF。

![堆栈合成](assets/screenshots/stacks.jpg)

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
