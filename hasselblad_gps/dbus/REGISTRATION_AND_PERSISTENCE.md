# com.hasselblad.gpsd 上总线与自启注册说明

日期:2026-11-04 | 对齐 `x2print/dbus/REGISTRATION_AND_PERSISTENCE.md` 的配方与
证据口径;本文只述 hasselblad_gps 的差异面,通用原理(bus 拓扑/方言/坑清单)
以 x2print 文档为准,规格输入为 `docs/X2DII_PORT_ANALYSIS.md` §2/§4。

---

## 1. 总线环境(继承,未改动)

- 单 **system bus** `unix:path=/dev/socket/dbus`,dbus.rc 由 init 拉起,
  属性开关 `hbl.dbus_service=1`;
- dbus.conf 策略全开(`allow own="*"`/`send_destination="*"`/eavesdrop)——
  占名 `com.hasselblad.gpsd` **零策略修改**(docs/X2DII_PORT_ANALYSIS.md §2.1);
- 316 固件全树 `com.hasselblad.gpsd` 精确串 0 命中,无冲突(§2.4);
- **init 环境无 `DBUS_SYSTEM_BUS_ADDRESS`**——生产拉起必须显式注入
  (boot wrapper/service.sh 均已带;R7 坑)。

## 2. 进程内注册序列(src/gpsd/main.c + adaptor.c,printer-service 同构)

1. `dbus_connection_set_change_sigpipe(FALSE)` + `signal(SIGPIPE, SIG_IGN)`;
2. 连接:`--bus` 显式地址 → `dbus_connection_open_private` + **必补
   `dbus_bus_register`**;否则 `DBUS_SYSTEM_BUS_ADDRESS` 环境变量 →
   `dbus_bus_get(DBUS_BUS_SYSTEM)`;
3. 占名 `dbus_bus_request_name(conn, "com.hasselblad.gpsd",
   DBUS_NAME_FLAG_DO_NOT_QUEUE)`——非 PRIMARY_OWNER 立即退出
   (单实例语义,不设 ALLOW_REPLACEMENT);
4. `dbus_connection_register_object_path(conn, "/gpsd", &vtable, NULL)`
   (`DBusObjectPathVTable{unregister, message, pad×4}`);单 `obj_filter`
   分发链:Introspect(静态 XML)→ Ping → Properties Get/Set/GetAll → 业务方法;
5. 属性变更信号 `dbus_message_new_signal("/gpsd",
   "org.freedesktop.DBus.Properties", "propertiesChanged")`——**小写 p**,
   `sa{sv}as`,invalidated 恒空;
6. **订阅面(gpsd 特有)**:`dbus_bus_add_match` 宽匹配×2
   (`sender='com.hasselblad.camera'` 与 `sender='com.hasselblad.storage'`,
   各带 `interface='org.freedesktop.DBus.Properties'`,**绝不含 path+member**
   ——窄匹配不配送,R3 坑),并各重复一次(Qt 客户端怪癖);
   `dbus_connection_add_filter` 收两种拼写的 propertiesChanged,
   观察 `exposures_processing_counter`/`storage_processing_counter`;
7. 主循环 `dbus_connection_read_write_dispatch(conn, 100ms)` + tick
   (gps.cur 轮询/TCP 桥/DCIM 扫描/注入子进程收割);断连干净退出不重连;
8. 封送纪律:借用指针不 free;错误名 `com.hasselblad.error.*`;
   同连接复用做客户端(`set_gps_position` IBIS 副产品,2s 界)。

## 3. 部署布局(docs/X2DII_PORT_ANALYSIS.md §4.2)

```
/data/hasselblad_gps/                ← 载荷根(跨 OTA 存活; SELinux: chcon
                                        u:object_r:system_file:s0 每次 install 重打)
  bin/gpsd            服务守护(本仓 build/aarch64/gpsd)
  bin/gpsctl          CLI 客户端
  bin/hginject        注入器(inject/ 并行开发线产出; install 时缺则告警跳过)
  etc/gpsd.conf       配置(首启自动生成默认)
  log/gpsd.log[.1]    滚动日志 512KB×2; stdout.log(wrapper 重定向)
  run/done.list       已处理清单(启动全量 seed + 即时追加)
  run/gps.cur         位置文件次选(/tmp/gps.cur 为首选)

/system/etc/init/s_hgpsd.rc          ← zz_boot 模式 A 三件套(OTA 冲刷面)
/system/bin/s_hgpsd_boot.sh             (s_x2dprint 配方改名)
/system/bin/zzsh                       (x2print/x2nes 共用入口, 复用不重建)
persist.zz.hgpsd = 1|0                ← 自启开关(0=重启后 100% 原厂)
```

## 4. 自启链(early-init → wrapper → gpsd)

```
init(early-init) → start s_hgpsd(class core, oneshot)
  → /system/bin/zzsh /system/bin/s_hgpsd_boot.sh   (seclabel u:r:hbl_camera_service:s0)
      ① persist.zz.hgpsd != 1 → 退出(软禁用)
      ② pidof gpsd 已在跑 → 退出(幂等)
      ③ 等 /dev/socket/dbus(≤120s)与 ssd 挂载 /mnt/media_rw/ssd(≤60s)
      ④ nohup DBUS_SYSTEM_BUS_ADDRESS=unix:path=/dev/socket/dbus \
           /data/hasselblad_gps/bin/gpsd >> log/stdout.log 2>&1 &
```

要点(全部继承 s_x2dprint/zz_boot 实测配方):

- **zzsh 技巧**:`cp /system/bin/sh /system/bin/zzsh` +
  `chcon u:object_r:hbl_camera_service_exec:s0`,rc 以 zzsh 为命令 +
  `seclabel u:r:hbl_camera_service:s0` 进 hbl 特权域——enforcing 下即有
  exec/setprop/rw /data 全权限(docs/X2DII_PORT_ANALYSIS.md §2.5);
  已装 x2print/x2nes 时复用现有 zzsh,卸载时**不删**(见 §6);
- **boot 时 property 触发器不重估**——自启靠 early-init+class core 兜底,
  persist 属性只作门控(zz_boot README §1.6);
- **等 ssd**:照片在 /mnt/media_rw/ssd(geotagd_boot.sh 同款);等 dbus:
  服务用总线(二者都是 class core,一般 0-2s 就绪,wrapper 有超时上限);
- **nohup 后台化 + pidof 防重**:oneshot 语义下 wrapper 退出, gpsd 常驻。
- SELinux 退场纪律:/data 载荷标签失效(restorecon/出厂重置)时 gpsd
  exec 失败——boot wrapper 记日志后退出,**不卡死不重试**(R5)。

## 5. PC 侧驱动(service.sh,adb 通道)

```
./service.sh install            # 停旧→推载荷→chcon→remount,rw 落 /system 三件套
                                #   →persist.zz.hgpsd=1→当场启动→自检
                                #   (先探测并停用 x2gps 旧 geotagd, R10)
./service.sh uninstall [--keep-config]   # persist=0→停→rm 三件套→rm /data 载荷
                                #   (--keep-config 备份 gpsd.conf+done.list)
./service.sh start|stop|status|log [n]|ctl <args...>|sh "<cmd>"
```

- start 幂等(pidof);stop = SIGTERM 干净退出(done.list 已即时、日志 flush);
- status 四口径:进程 pidof + 总线名 NameHasOwner + gpsctl get-all +
  done.list 行数 + persist/rc 部署态;
- OTA 纪律:OTA 前 `./service.sh uninstall`(block-based 全量刷槽冲掉
  /system 改动,R6);升级后重新 install。

## 6. 卸载语义

1. `setprop persist.zz.hgpsd 0`(下次 reboot 起 init 不再拉起);
2. `killall -TERM gpsd`(SIGTERM:杀注入子进程→flush→退出;照片零接触);
3. `rm /system/etc/init/s_hgpsd.rc /system/bin/s_hgpsd_boot.sh`;
   **zzsh 保留**(x2print/x2nes 共用;确无他人使用可手工
   `rm /system/bin/zzsh`);
4. `rm -rf /data/hasselblad_gps`(--keep-config 先拉回 gpsd.conf/done.list);
5. 若 install 时停用过 x2gps geotagd,原样保持停用(不自动恢复——旧 daemon
   会与新服务竞争同一 DCIM)。

## 7. 验收清单(真机)

| # | 项 | 命令 |
|---|---|---|
| 1 | 占名/对象 | `service.sh status`(NameHasOwner=true) |
| 2 | Introspect XML | `adb shell dbus-send --print-reply --address=unix:path=/dev/socket/dbus --dest=com.hasselblad.gpsd /gpsd org.freedesktop.DBus.Introspectable.Introspect` |
| 3 | 推坐标→属性 | `service.sh ctl set 31.2304 121.4737 10.5` 后 `ctl get latitude` |
| 4 | 信号面(V1) | `service.sh ctl monitor` + 连拍 5 张,观察 camera/storage counter 触发与 2s 延迟扫描 |
| 5 | 注入管线 | done.list seed 行数=存量照片数;拍一张→gpsd.log 出现 "OK <path>";hginject --revert 可逆 |
| 6 | 优雅退出 | `service.sh stop`;gpsd.log 尾部 "shutting down" 且无残留 gpsd/hginject 进程 |
| 7 | 重启自启 | reboot 后 `service.sh status` 服务在跑;`setprop persist.zz.hgpsd 0` + reboot 后纯原厂 |
