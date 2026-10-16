# UVCMonitor Android — X2D II 外接监视器（安卓显示端）

## 功能

斑马纹(阈值滑杆) / 假彩色(9段) / 峰值对焦(Sobel染红,灵敏度滑杆) / RGB直方图(NV21抽样0.5s一更) / 网格(三分对角中心黄金) / 画幅遮罩(16:9 4:3 1:1 **65:24** 3:2) / 黑白 / 数字变焦1-8×(捏合+拖拽,双击复位) / 冻结帧 / 截图存相册(MediaStore免权限) / 顶栏HUD(设备名=USB iProduct/格式/实测fps) / 单击收起UI / 插入UVC设备自动拉起app(device_filter)。

## 架构

```
app/src/main/java/com/x2dii/uvcmonitor/
  MainActivity.kt   入口 + 状态 + 监听回调
  UvcEngine.kt      UsbManager(权限/热插拔) + UVCCamera(YUYV/MJPEG→NV21帧回调) + fps/直方图/截图
  GlRenderer.kt     GLSurfaceView + GLSL ES2 单pass（NV21双平面→RGB + 变焦/峰值/黑白/斑马/假彩）
  Ui.kt             Compose 全套 UI（顶栏/工具栏/滑杆/overlay/手势/直方图）
  MonitorSettings.kt 状态 + 枚举
app/src/main/java/com/serenegiant/utils/
  HandlerThreadHandler.java / BuildCheck.java   ← 补丁类（见下）
```