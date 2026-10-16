# x2print — 生产化部署速览

x2print 是 X2D II 相机上的通用 IPP 无线打印服务（`com.hasselblad.printer`，D-Bus 契约见
`dbus/INTERFACE.md`）。生产部署用根目录 `service.sh`（PC 侧 Git Bash 经 adb 驱动相机）：

连接生命周期（GUI 视角）：`Connect_to_ap(ssid,psk)` → joining → joined（相机侧
insmod bcmdhd → wpa_supplicant→COMPLETED → udhcpc）→ `Start_discovery`（三级级联：
printer_ip 缓存直连 → mDNS → 网关/24 扫描，IPP 验明后回写 config）→
`Select_printer`（能力探测/发布）→ `Start_print_job` → done 后
`Disconnect_and_restore`（幂等，恢复相机 Wi-Fi 基线零残留）。psk 只落
`/data/x2print/psk`（0600），绝不做成 D-Bus 属性；GUI 崩溃兜底有闲置看门狗
（config `idle_restore_min`，缺省 5 分钟，0=禁用）。构建/回归/真机验收全记录见
`NOTES.md`（§14 wpa 交叉编译、§15 真机全通、§16 生产化固化）。
