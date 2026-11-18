# com.hasselblad.gpsd — X2D II 地理标记服务 D-Bus 接口规格书(v1.0)

日期:2026-11-04 | 本文件是**唯一契约**:gpsd(src/gpsd)、gpsctl(src/gpsctl)、
service.sh 与上层推送端(PC/手机)共同遵守。风格与证据密度对齐
`x2print/dbus/INTERFACE.md`;移植语义源自 `docs/GPS_LIFECYCLE_X1D_X1DII.md`
(X1D II com.hasselblad.gpsd 接口的致敬复刻)与 `docs/X2DII_PORT_ANALYSIS.md`
(事件面/部署/构建规格)。

**与 X1D II 原厂 gpsd 的关系**:接口名/对象路径/方法与信号命名致敬原厂
(`gps_enable`/`gps_disable`/`nmea_sentence`/`gps_available`),但 X2D II 无 GPS
硬件——位置数据全部来自外部喂入(DBus 方法为主通道,文件轮询与 TCP NMEA 桥
为兼容叠加),消费端从"喂 Qt 定位插件"改为"注入照片 EXIF(hginject)"。

---

## 1. 总线与标识

| 项 | 值 |
|---|---|
| 总线 | system bus,相机 socket `unix:path=/dev/socket/dbus`(init 环境无该地址变量,须显式注入 `DBUS_SYSTEM_BUS_ADDRESS`) |
| bus name (destination) | `com.hasselblad.gpsd`(316 固件全树 grep 零冲突,docs/X2DII_PORT_ANALYSIS.md §2.4) |
| object path | `/gpsd` |
| 业务接口名 | `com.hasselblad.gpsd`(= bus name,odindb 同体约定) |
| 属性接口 | `org.freedesktop.DBus.Properties` 的 `Get(ss)v`、`Set(ssv)`、`GetAll(s)→a{sv}`;interface 实参填 `com.hasselblad.gpsd`(Set 一律拒绝——属性只读) |
| 属性变更信号 | `org.freedesktop.DBus.Properties.propertiesChanged`(**首字母小写 p!平台方言,非标准拼写**),path=`/gpsd`,签名 `sa{sv}as`,arg2(invalidated)恒空数组 |
| 方法 member | snake_case 原样(`gps_enable`/`gps_disable`);`SetPosition`/`GetPosition`/`SimulateFix` 为本项目自有命名 |
| 错误名 | `com.hasselblad.error.{failed\|parametererror}`(全小写连写;本服务只用到这两个) |
| Introspect | 实现 `org.freedesktop.DBus.Introspectable.Introspect` → 手工 XML(与本文一致);`Peer.Ping` 应答空回复 |

**方言坑(必读,全部实测继承自 x2print)**:

1. **窄匹配不配送**:相机 dbus-daemon 对同时含 `path=` 与 `member=` 的 match
   rule 不投递信号——订阅一律宽匹配(仅 sender/interface);
2. **小写 propertiesChanged**:发信号用 `propertiesChanged`,收信号两种拼写
   (`propertiesChanged`/`PropertiesChanged`)都接受(原厂二进制两种拼写并存);
3. **Qt 客户端怪癖**:连接后需重复一次 AddMatch(gpsctl 已内置);
4. `dbus_message_get_args` 取出的 string 是借用指针,不可 free。

客户端 match rule(宽匹配示例):

```
type='signal',sender='com.hasselblad.gpsd'
```

---

## 2. 属性全表(7 个,全部 notify,全部只读)

GetAll 返回顺序即下表顺序。

| # | 属性 | 签名 | 初始值 | 语义 |
|--:|---|---|---|---|
| 1 | `gps_available` | `b` | `false` | 数据管线可用(致敬 X1D II `gps_available`):**收到过至少一次位置更新且 age ≤ max_age_s**(默认 300s)。与 `fix_valid` 正交(有数据≠定位有效) |
| 2 | `latitude` | `d` | `0` | 纬度(度,WGS-84,十进制) |
| 3 | `longitude` | `d` | `0` | 经度 |
| 4 | `altitude` | `d` | `0` | 海拔(米;无高程来源时 0) |
| 5 | `fix_valid` | `b` | `false` | 最近一次定位有效(RMC 状态 A / GGA quality>0 / 推送端断言) |
| 6 | `fix_age_ms` | `x` | `-1` | 距最近一次位置更新的毫秒数(CLOCK_MONOTONIC);从未更新过 = -1 |
| 7 | `enabled` | `b` | `true` | 注入管线启停:`gps_enable`/`gps_disable` 切换;false=只缓存位置不写照片 |

notify 时机:`positionChanged` 数据面每次有效更新时,`gps_available`/
`latitude`/`longitude`/`altitude`/`fix_valid`/`fix_age_ms` 逐项广播;
`enabled` 仅在 `gps_enable`/`gps_disable` 时广播。`fix_age_ms` 不做周期推送
(读时现算)。

## 3. 方法全表

| member | 入参签名 | 返回签名 | 语义 | 错误 |
|---|---|---|---|---|
| `SetPosition` | `ddddddubs` (lat, lon, alt, dop, speed, track, nsat, fix_valid, utc) | `` | **主数据通道**(PC/手机推坐标)。更新 last-known 缓存并广播 `positionChanged`+`propertiesChanged`;若配置 `ibis_compensate=1` 且 fix_valid,节流(位移>~55m 或 >60s)后同步调 `com.hasselblad.camera /camera set_gps_position(a{sv})` 喂 IBIS 星野补偿。`alt` 传 NaN=无高程(注入时不写 --alt);`utc` 可空串(服务端不补) | `parametererror`(lat/lon 非有限或越界 ±90/±180);`InvalidArgs`(类型不符) |
| `GetPosition` | `` | `a{sv}` | 全缓存字段快照:`latitude/longitude/altitude/dop/speed/track(d)`、`nsat(u)`、`fix_valid(b)`、`utc(s)`、`fix_age_ms(x)`、`source(s: dbus\|file\|nmea\|simulate)`、`enabled(b)`、`gps_available(b)` | — |
| `gps_enable` | `i`(可省略) | `` | 启用注入管线(新照片尺寸稳定后写 GPS)。int 参数仅为 **X1D II `GpsdProxy::gps_enable(int)` 签名兼容**,取值忽略 | — |
| `gps_disable` | `i`(可省略) | `` | 停用注入管线(只缓存不写照片;候选照片不标 done,恢复后可补注入) | — |
| `SimulateFix` | `` | `` | 致敬 X1D 工厂 GPS_Simulation 后门:写入哥德堡测试坐标 57.705617, 11.935300(57°42.365'N 11°56.118'E,X1D 注入集同源),fix_valid=1,source=simulate,并广播 | — |

## 4. 信号全表

| member | 接口 | 签名 | 触发 |
|---|---|---|---|
| `positionChanged` | `com.hasselblad.gpsd` | `a{sv}` | 位置缓存每次更新(DBus/文件/TCP NMEA/模拟)。字典键同 `GetPosition` 的位置子集 |
| `nmea_sentence` | `com.hasselblad.gpsd` | `ay` data, `y` length | TCP NMEA 桥每收到一条完整语句广播一次(X1D II `GpsdProxy::nmea_sentence(QByteArray,uchar)` 线格式致敬;仅 nmea_tcp 配置时出现) |

`propertiesChanged`(见 §1)载荷示例:arg0=`"com.hasselblad.gpsd"`,
arg1=`{"fix_valid": <true>}`,arg2=`[]`。

## 5. 服务进程行为(数据源与照片管线)

规格来源:`docs/X2DII_PORT_ANALYSIS.md` §1.3/§2/§4.2。**全部可选叠加**:

```
数据源(互不排斥, 后写胜出):
  (a) DBus SetPosition —— 主通道
  (b) 文件轮询 /tmp/gps.cur 与 /data/hasselblad_gps/run/gps.cur
      ("lat lon [alt]" 一行, x2gps gps_push.py 协议; 内容变化才喂)
  (c) TCP NMEA 桥 nmea_tcp=host:port(配置; RMC/GGA 极简解析,
      talker 不校验, 校验和强校验; 断线 5s 重连)

照片管线(事件+轮询双轨):
  [触发层] 宽匹配订阅 com.hasselblad.camera / com.hasselblad.storage 的
           propertiesChanged → exposures_processing_counter /
           storage_processing_counter 变化 → 延迟 event_delay_ms(2s) 扫描
  [兜底层] poll_interval_ms(2s) 定时扫描 dcim_root(/mnt/media_rw/ssd/DCIM)
           递归 *.3fr/*.jpg/*.heic/*.hif(大小写不敏感)
  [稳定层] 候选文件 stable_ms(1.5s) 两次尺寸一致才注入
  [门控]   fix_valid=0 或坐标 age > max_age_s(300s) → 跳过并记日志(不标 done)
  [防重]   done.list 启动全量 seed 存量 + 处理过即追加(即时 flush)
  [注入]   exec hginject <file> --lat %.7f --lon %.7f [--alt %.2f]
           (单飞行, 超时 inject_timeout_s 60s 后 SIGKILL; 尝试过即标 done)
```

安全边界:服务**绝不修改/删除照片本体**(写入全部由 hginject 执行);
SIGTERM = 杀注入子进程 + flush 日志 + done.list 已即时落盘,干净退出。

## 6. 示例

### gpsctl(推荐,自带封送)

```sh
export DBUS_SYSTEM_BUS_ADDRESS=unix:path=/dev/socket/dbus
gpsctl set 31.2304 121.4737 10.5 --nsat 12 --dop 1.2
gpsctl set 31.2304 121.4737 --nofix          # 只更新缓存为"无定位"
gpsctl status                                 # 人读总览(坐标/age/启用态)
gpsctl get fix_age_ms
gpsctl get-all
gpsctl disable                                # 停注入(只缓存)
gpsctl enable
gpsctl simulate                               # 哥德堡测试坐标
gpsctl monitor                                # 跟全部信号
```

### dbus-send(相机自带 /system/bin/dbus-send,dbus.conf 全放行)

```sh
ADB="C:/Users/tom/AppData/Local/Android/Sdk/platform-tools/adb.exe"
BUS="--address=unix:path=/dev/socket/dbus"

# 推坐标(9 参顺序: lat lon alt dop speed track nsat fix_valid utc)
"$ADB" shell dbus-send --print-reply $BUS --dest=com.hasselblad.gpsd /gpsd \
  com.hasselblad.gpsd.SetPosition \
  double:31.2304 double:121.4737 double:10.5 \
  double:0 double:0 double:0 uint32:0 boolean:true string:""

# 读回快照
"$ADB" shell dbus-send --print-reply $BUS --dest=com.hasselblad.gpsd /gpsd \
  com.hasselblad.gpsd.GetPosition

# 启停注入(X1D II (int) 签名)
"$ADB" shell dbus-send --print-reply $BUS --dest=com.hasselblad.gpsd /gpsd \
  com.hasselblad.gpsd.gps_enable int32:0

# 单属性
"$ADB" shell dbus-send --print-reply $BUS --dest=com.hasselblad.gpsd /gpsd \
  org.freedesktop.DBus.Properties.Get \
  string:com.hasselblad.gpsd string:gps_available

# 总线名在位断言
"$ADB" shell dbus-send --print-reply $BUS --dest=org.freedesktop.DBus \
  /org/freedesktop/DBus org.freedesktop.DBus.NameHasOwner \
  string:com.hasselblad.gpsd
```

> busctl 在本机固件不可用——gpsctl 即为其等价替代(`docs/X2DII_PORT_ANALYSIS.md`
> V1 验证项由 gpsctl monitor 承担)。

## 7. Introspect XML

手工静态串,与本文一一对应,见 `src/gpsd/adaptor.c` 的 `INTROSPECTION_XML`。
含业务接口(5 方法/2 信号/7 属性)+ Properties(propertiesChanged 声明为
**小写** member)+ Introspectable + Peer 四段。

## 8. 兼容性说明

1. **odindb-send 不认识 gpsd**(ServiceFactory 硬编码 16 模块)——驱动本服务用
   gpsctl 或 dbus-send(§6);
2. 名称占用决策(docs/X2DII_PORT_ANALYSIS.md §2.4"决定后不再改"):
   主名 `com.hasselblad.gpsd` 已冻结——done.list/文档/工具链全部带此名;
3. 与 x2gps 旧 geotagd daemon 竞争同一 DCIM:install 时探测并停用旧件
   (persist.zz.geotagd=0 + kill,service.sh 已内置);
4. 服务晚于客户端启动无碍:X1D 生态标准 NameOwnerChanged 模式
   (docs/GPS_LIFECYCLE §2.3);推送端拿 `NameHasOwner` 或 `gpsctl status` 探测。

## 9. 服务进程参数

二进制 `/data/hasselblad_gps/bin/gpsd`(NDK r25b aarch64,链相机原 libdbus.so):
`--config <path>`(默认 /data/hasselblad_gps/etc/gpsd.conf)、`--bus <addr>`、
`--log <file>`、`--loglevel <lvl>`、`--no-seed`(调试,跳过存量 seed,危险)、
`--version/--help`。日志滚动 512KB×2(gpsd.log + gpsd.log.1)。
生产拉起/停止统一走根目录 `service.sh start|stop`(adb 通道)。
