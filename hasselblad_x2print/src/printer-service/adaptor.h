/* adaptor.h — libdbus adaptor: 占用 com.hasselblad.printer, 分发方法/属性,
 * 发 propertiesChanged(小写 p!) —— 线协议契约见 dbus/INTERFACE.md */
#ifndef PRN_ADAPTOR_H
#define PRN_ADAPTOR_H

#include <dbus/dbus.h>
#include "backend.h"

/* 注册 /printer 对象过滤器(方法分发表+Properties+Introspect); 0=ok,-1=失败 */
int  prn_adaptor_start(DBusConnection *c, const prn_backend *be);

/* 广播属性变更信号(用 start 时记住的连接):
 * path=/printer iface=org.freedesktop.DBus.Properties member=propertiesChanged
 * 签名 sa{sv}as, arg0=com.hasselblad.printer, arg2 恒空 */
void prn_adaptor_emit_changed(int prop_idx);

/* odindb-send-printer 兼容信号(INTERFACE.md §7b): 接口 com.hasselblad.printer,
 * path=/printer, member=rx|testrx, 签名 "yay"(y=message_len, ay=message)。
 * rx 触发方: backend_real job_status 变更(X2P 事件帧, 见 backend_real.c)与
 * tx() echo 回显; testrx 预留(无内部触发方)。客户端 `--signal rx|testrx` 订阅。 */
void prn_adaptor_emit_rx(unsigned char len, const unsigned char *data, int n);
void prn_adaptor_emit_testrx(unsigned char len, const unsigned char *data, int n);

/* ---- 业务活动时间戳(闲置看门狗判据, INTERFACE.md §4b) ----
 * 业务方法(handle_methods 全表, 含 tx)与 Properties.Set 入口刷新;
 * Properties.Get/GetAll(纯轮询)不刷新 —— GUI 打开着但不干活照样算闲置。
 * prn_adaptor_idle_ms(): 距上次业务调用毫秒数(自 start 起算)。 */
void prn_adaptor_touch(void);
long long prn_adaptor_idle_ms(void);

/* ---- 相机系统服务合法 Wi-Fi 电源同步(§4b 恢复协议) ----
 * 调 com.hasselblad.system /system power_on_wifi(b enable)(QtDBus 严格签名,
 * 恰一个 bool, 无 wrapper timeout 尾参)。返回 0=ok, -1=调用失败(日志已记)。
 * 用途: 会话前合法让出芯片(原 AP 在跑时先关)/恢复末尾同步 camera-system
 * 状态机(回到会话前原状, 防直接 rmmod 造成的状态失同步)。 */
int prn_adaptor_system_wifi_power(int on);

/* ---- 相机息屏超时(§19 会话-待机治理, 显示层) ----
 * com.hasselblad.system 属性 screen_idle_timeout(int, 秒): 会话期临时置大值
 * (打印不息屏, 用户口径), 恢复完成还原原值。经 org.freedesktop.DBus.Properties
 * Get/Set(标准 ssv, variant int32)。返回 0=ok, -1=失败(日志已记)。 */
int prn_adaptor_screen_idle_get(int *sec);
int prn_adaptor_screen_idle_set(int sec);

/* ---- Scan_wifi 延迟回复(P0 反饿死, NOTES §22) ----
 * 真后端 scan_wifi 返回 1=异步受理(芯片让出/保障/扫描在 tick 机推进)时,
 * adaptor 挂起方法回复(m_scan_wifi 里 ref 报文), 后端完成后调用本函数补发:
 * ok=1 → as(与 scanned_networks 属性同源); ok=0 → com.hasselblad.error.failed
 * (be_err 详情并入)。线协议契约不变(客户端看仍是阻塞至完成), 事件循环期间
 * 照常 dispatch。无挂起回复时为无操作。 */
void prn_adaptor_scan_complete(int ok);

#endif
