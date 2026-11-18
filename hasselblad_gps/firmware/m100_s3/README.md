# ESP32-S3 + HGLRC M100 Mini

固件通过 UART1 接收 GNSS 数据，识别 NMEA 和 UBX，从 NMEA RMC/GGA 或 UBX NAV-PVT 获取定位。ESP32 广播 `M100-GPS`，Windows 可通过 BLE 订阅 UART 原始字节。

## 接线

以 ESP32-S3-DevKitC-1 为构建目标。断电后按板上引脚标识接线。

| M100 Mini | ESP32-S3 |
| --- | --- |
| VCC | 3V3 |
| GND | GND |
| TX | GPIO18（UART1 RX） |
| RX | GPIO17（UART1 TX，查询版本和卫星详情时需要） |

GPS UART 默认为 `115200 8N1`。模块供电规格为 3.3–5 V，本接线用 3.3 V。UART 信号使用 3.3 V 逻辑。参见 [HGLRC 模块规格](https://www.hglrc.com/collections/new-arrivals/products/hglrc-m100_mini-gps) 和 [Espressif 引脚表](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.0.html)。

## 构建、烧录和验收

在 Arduino IDE 中打开 `M100_GPS/M100_GPS.ino`，选择 **ESP32S3 Dev Module** 和开发板的 COM 口。依赖为 TinyGPSPlus 和 NimBLE-Arduino 2.5.1。板卡的 Flash 大小按实物设置。

用数据线连接板上 USB-to-UART 口。在 `hasselblad_gps` 目录运行以下命令，将 `COMx` 换成实际端口。

```powershell
python -m platformio run -d firmware/m100_s3
python -m platformio run -d firmware/m100_s3 -t upload --upload-port COMx
python -m platformio device monitor -p COMx -b 115200
```

- `rx=0` 表示 ESP32 尚未收到字节。检查供电、共地和 TX 到 GPIO18。
- `rx` 增长但 `nmea` 和 `ubx` 都为 0 时，检查 GPS 串口波特率和信号线。
- `nmea` 或 `ubx` 持续增长表示收到了通过校验的帧。此时 `fix=0` 可以在室外开阔处继续等待定位。
- `fix=1` 时输出经纬度。NMEA 路径会在最后一次有效位置更新后 2.5 秒清除定位状态。

固件优先采用新鲜的 UBX NAV-PVT 定位，其次采用 NMEA。UBX 字段布局见 [u-blox M10 接口说明](https://content.u-blox.com/sites/default/files/u-blox-M10-SPG-5.10_InterfaceDescription_UBX-21035062.pdf)。

## Windows BLE 采集

安装 Python 的 `bleak` 库后，在 `hasselblad_gps` 运行：

```powershell
python firmware/m100_s3/pc_capture.py --advertisement-only
python firmware/m100_s3/pc_capture.py --scan-only
python firmware/m100_s3/pc_capture.py --interactive
python firmware/m100_s3/pc_capture.py --seconds 20 --output m100_uart.bin
python firmware/m100_s3/pc_capture.py --diagnostics --seconds 15
```

`--advertisement-only` 只发现 BLE 广播，适合相机同时排查直连。`--scan-only` 会短暂连接 GATT，确认原始数据特征存在后退出。

交互模式的命令：

| 命令 | 用途 |
| --- | --- |
| `status` | 显示最近的 UBX NAV-PVT、NMEA RMC/GGA 定位状态和收包计数 |
| `nmea` | 显示通过校验的 RMC/GGA 和 GSV 星座、卫星号与载噪比 |
| `version` | 通过 BLE 发出 MON-VER 查询，显示模块软硬件及协议版本 |
| `satellites` | 通过 BLE 发出 NAV-SAT 查询，显示星座、卫星号、载噪比和跟踪状态 |
| `watch 10` | 每秒显示一次状态，持续 10 秒 |
| `save m100_uart.bin`、`stop` | 开始和停止保存当前 BLE 收到的原始 GPS 字节 |
| `help`、`quit` | 显示帮助或断开退出 |

批量查询可用 `--query version --query satellites`。刚升级固件后，如果 Windows 仍显示旧的 GATT 特征，可加 `--fresh-gatt` 重新发现。PC 客户端只通过 BLE 读写 GATT。两项查询由 ESP32 转发给模块，不修改 GPS 配置。

支持诊断特征的固件可用 `--diagnostics` 查看采集前后的 ESP32 运行时间、UART 接收字节数、BLE 通知提交成功和失败次数，以及发送队列是否曾溢出。`notify_accepted` 统计 BLE 栈接受的通知，实际到达 PC 的通知数见 `packets`。整个过程只使用 BLE。

通知前四字节为 ESP32 从启动起接收的 UART 字节偏移（小端 uint32），其余为原始数据。PC 将数据连续写入文件并报告通知之间的缺失字节数。首包之前、末包之后以及进入 ESP32 UART 驱动之前的丢失不在统计范围内。

Windows 可以直接连接这个 BLE 服务，无需先在系统设置中配对。如果能扫描到 `M100-GPS`，但读取 GATT 服务或订阅时报 `Unreachable`，检查 Windows 是否保留了这台设备的配对记录。移除该设备的配对记录后，直接重新运行采集脚本。实机测试中，移除配对记录后，同一版固件和同一个蓝牙驱动恢复了通知。
