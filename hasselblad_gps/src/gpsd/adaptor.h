/* adaptor.h — com.hasselblad.gpsd 服务 adaptor(libdbus, x2print printer-service 同构)
 * 线协议方言(docs/X2DII_PORT_ANALYSIS.md §1.2/§2.3, 实测坑全继承):
 *   - destination == interface == com.hasselblad.gpsd, path == /gpsd
 *   - 属性变更信号 member=propertiesChanged(首字母小写 p!), sa{sv}as,
 *     invalidated 恒空
 *   - 订阅 /camera /storage 用宽匹配(仅 sender+interface —— 窄匹配含
 *     path+member 时相机 dbus-daemon 不配送, NOTES:905)
 *   - 错误名 com.hasselblad.error.{failed|parametererror|busy|...}
 *   - 方法名 snake_case 原样(gps_enable/gps_disable 致敬 X1D II
 *     GpsdProxy 签名 (int); docs/GPS_LIFECYCLE_X1D_X1DII.md §2.2)
 */
#ifndef GPS_ADAPTOR_H
#define GPS_ADAPTOR_H

#include <dbus/dbus.h>

#define GPSD_NAME  "com.hasselblad.gpsd"
#define GPSD_PATH  "/gpsd"
#define GPSD_IFACE GPSD_NAME

#define GPSD_IFACE_PROPERTIES "org.freedesktop.DBus.Properties"
#define GPSD_IFACE_INTROSPECT "org.freedesktop.DBus.Introspectable"
#define GPSD_IFACE_PEER       "org.freedesktop.DBus.Peer"
/* 平台方言: 属性变更信号首字母小写(不是标准 PropertiesChanged!) */
#define GPSD_SIG_PROPCHANGED  "propertiesChanged"

int  gps_adaptor_start(DBusConnection *c);   /* 注册 /gpsd vtable */

/* 位置缓存更新后的统一出口: 广播 positionChanged + propertiesChanged,
 * 并按配置节流喂 IBIS(com.hasselblad.camera set_gps_position, setgps.c 逻辑)。 */
void gps_adaptor_on_position_updated(void);

/* TCP 桥整句转发(X1D II nmea_sentence(QByteArray,uchar) 致敬, wire = ay,y) */
void gps_adaptor_emit_nmea(const char *line);

/* 管线启停状态广播(enabled 属性) */
void gps_adaptor_emit_enabled(void);

/* 订阅 /camera /storage 的 propertiesChanged(触发层, 见 monitor.h) */
int  gps_adaptor_install_subscriptions(DBusConnection *c);

#endif
