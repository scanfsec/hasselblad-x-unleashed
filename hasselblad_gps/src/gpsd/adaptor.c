/* adaptor.c — com.hasselblad.gpsd 服务端 + 相机侧客户端调用(纯 libdbus)
 * 私有注释逐段对照: x2print/src/printer-service/adaptor.c(已真机验证的同构实现)
 * 与 docs/X2DII_PORT_ANALYSIS.md §2.3 七步注册配方。
 */
#include "adaptor.h"
#include "config.h"
#include "position.h"
#include "monitor.h"
#include "log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include <time.h>

#define ERR_FAILED "com.hasselblad.error.failed"
#define ERR_PARAM  "com.hasselblad.error.parametererror"

static DBusConnection *g_conn;

/* ---------- IBIS 节流状态 ---------- */
static double   s_ibis_lat, s_ibis_lon;
static int      s_ibis_sent;
static long long s_ibis_last_ms;

/* ---------- 通用回复 ---------- */
static DBusHandlerResult reply_err(DBusConnection *c, DBusMessage *req,
                                   const char *name, const char *msg)
{
    DBusMessage *r = dbus_message_new_error(req, name, msg);
    if (!r) { LOGE("oom: error reply"); return DBUS_HANDLER_RESULT_NEED_MEMORY; }
    dbus_connection_send(c, r, NULL);
    dbus_message_unref(r);
    return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult reply_empty(DBusConnection *c, DBusMessage *req)
{
    DBusMessage *r = dbus_message_new_method_return(req);
    if (!r) return DBUS_HANDLER_RESULT_NEED_MEMORY;
    dbus_connection_send(c, r, NULL);
    dbus_message_unref(r);
    return DBUS_HANDLER_RESULT_HANDLED;
}

/* ---------- a{sv} 字典小工具 ---------- */
static void dict_s(DBusMessageIter *arr, const char *k, const char *v)
{
    DBusMessageIter e, w;
    dbus_message_iter_open_container(arr, DBUS_TYPE_DICT_ENTRY, NULL, &e);
    dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &k);
    dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, DBUS_TYPE_STRING_AS_STRING, &w);
    dbus_message_iter_append_basic(&w, DBUS_TYPE_STRING, &v);
    dbus_message_iter_close_container(&e, &w);
    dbus_message_iter_close_container(arr, &e);
}
static void dict_d(DBusMessageIter *arr, const char *k, double v)
{
    DBusMessageIter e, w;
    dbus_message_iter_open_container(arr, DBUS_TYPE_DICT_ENTRY, NULL, &e);
    dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &k);
    dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, DBUS_TYPE_DOUBLE_AS_STRING, &w);
    dbus_message_iter_append_basic(&w, DBUS_TYPE_DOUBLE, &v);
    dbus_message_iter_close_container(&e, &w);
    dbus_message_iter_close_container(arr, &e);
}
static void dict_b(DBusMessageIter *arr, const char *k, dbus_bool_t v)
{
    DBusMessageIter e, w;
    dbus_message_iter_open_container(arr, DBUS_TYPE_DICT_ENTRY, NULL, &e);
    dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &k);
    dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, DBUS_TYPE_BOOLEAN_AS_STRING, &w);
    dbus_message_iter_append_basic(&w, DBUS_TYPE_BOOLEAN, &v);
    dbus_message_iter_close_container(&e, &w);
    dbus_message_iter_close_container(arr, &e);
}
static void dict_u(DBusMessageIter *arr, const char *k, dbus_uint32_t v)
{
    DBusMessageIter e, w;
    dbus_message_iter_open_container(arr, DBUS_TYPE_DICT_ENTRY, NULL, &e);
    dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &k);
    dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, DBUS_TYPE_UINT32_AS_STRING, &w);
    dbus_message_iter_append_basic(&w, DBUS_TYPE_UINT32, &v);
    dbus_message_iter_close_container(&e, &w);
    dbus_message_iter_close_container(arr, &e);
}
static void dict_x(DBusMessageIter *arr, const char *k, dbus_int64_t v)
{
    DBusMessageIter e, w;
    dbus_message_iter_open_container(arr, DBUS_TYPE_DICT_ENTRY, NULL, &e);
    dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &k);
    dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, DBUS_TYPE_INT64_AS_STRING, &w);
    dbus_message_iter_append_basic(&w, DBUS_TYPE_INT64, &v);
    dbus_message_iter_close_container(&e, &w);
    dbus_message_iter_close_container(arr, &e);
}

/* 位置快照字典(positionChanged 与 GetPosition 共用) */
static void append_position_dict(DBusMessageIter *arr)
{
    const char *utc = g_pos.utc[0] ? g_pos.utc : "";
    dict_d(arr, "latitude",  g_pos.lat);
    dict_d(arr, "longitude", g_pos.lon);
    dict_d(arr, "altitude",  g_pos.have_alt ? g_pos.alt : 0.0);
    dict_d(arr, "dop",       g_pos.dop);
    dict_d(arr, "speed",     g_pos.speed);
    dict_d(arr, "track",     g_pos.track);
    dict_u(arr, "nsat",      (dbus_uint32_t)g_pos.nsat);
    dict_b(arr, "fix_valid", g_pos.fix_valid ? TRUE : FALSE);
    dict_s(arr, "utc",       utc);
    dict_x(arr, "fix_age_ms", (dbus_int64_t)gps_pos_age_ms());
    dict_s(arr, "source",    g_pos.source[0] ? g_pos.source : "");
}

/* ---------- 信号: positionChanged(a{sv}) ---------- */
static void emit_position_changed(void)
{
    if (!g_conn) return;
    DBusMessage *sig = dbus_message_new_signal(GPSD_PATH, GPSD_IFACE, "positionChanged");
    if (!sig) { LOGE("oom: signal"); return; }
    DBusMessageIter it, arr;
    dbus_message_iter_init_append(sig, &it);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &arr);
    append_position_dict(&arr);
    dbus_message_iter_close_container(&it, &arr);
    dbus_connection_send(g_conn, sig, NULL);
    dbus_message_unref(sig);
    LOGD("signal positionChanged lat=%.6f lon=%.6f fix=%d", g_pos.lat, g_pos.lon,
         g_pos.fix_valid);
}

/* ---------- 信号: nmea_sentence(ay data, y length) ----------
 * X1D II GpsdProxy::nmea_sentence(const QByteArray&, uchar) 的线格式;
 * length=字节长(uchar 截断, 与原厂一致), data=整句含 '$' 与换行外的本体。 */
void gps_adaptor_emit_nmea(const char *line)
{
    if (!g_conn || !line) return;
    int len = (int)strlen(line);
    const char *p = line;
    unsigned char ucl = (unsigned char)(len > 255 ? 255 : len);
    DBusMessage *sig = dbus_message_new_signal(GPSD_PATH, GPSD_IFACE, "nmea_sentence");
    if (!sig) return;
    if (!dbus_message_append_args(sig, DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE,
                                  &p, len, DBUS_TYPE_BYTE, &ucl, DBUS_TYPE_INVALID)) {
        dbus_message_unref(sig);
        return;
    }
    dbus_connection_send(g_conn, sig, NULL);
    dbus_message_unref(sig);
    LOGD("signal nmea_sentence len=%d", len);
}

/* ---------- 属性变更信号(平台方言: 小写 propertiesChanged, sa{sv}as 恒空) ---------- */
static void emit_prop_changed(const char *name, int type, ...)
{
    if (!g_conn) return;
    DBusMessage *sig = dbus_message_new_signal(GPSD_PATH, GPSD_IFACE_PROPERTIES,
                                               GPSD_SIG_PROPCHANGED);
    if (!sig) { LOGE("oom: signal"); return; }
    DBusMessageIter it, arr, entry, v, inv;
    const char *iface_name = GPSD_IFACE;
    dbus_message_iter_init_append(sig, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface_name);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &arr);
    dbus_message_iter_open_container(&arr, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name);

    va_list ap;
    va_start(ap, type);
    if (type == DBUS_TYPE_BOOLEAN) {
        dbus_bool_t b = (dbus_bool_t)va_arg(ap, int);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
                                         DBUS_TYPE_BOOLEAN_AS_STRING, &v);
        dbus_message_iter_append_basic(&v, DBUS_TYPE_BOOLEAN, &b);
    } else if (type == DBUS_TYPE_DOUBLE) {
        double d = va_arg(ap, double);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
                                         DBUS_TYPE_DOUBLE_AS_STRING, &v);
        dbus_message_iter_append_basic(&v, DBUS_TYPE_DOUBLE, &d);
    } else if (type == DBUS_TYPE_INT64) {
        dbus_int64_t x = (dbus_int64_t)va_arg(ap, long long);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
                                         DBUS_TYPE_INT64_AS_STRING, &v);
        dbus_message_iter_append_basic(&v, DBUS_TYPE_INT64, &x);
    } else {
        const char *s = va_arg(ap, const char *);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
                                         DBUS_TYPE_STRING_AS_STRING, &v);
        dbus_message_iter_append_basic(&v, DBUS_TYPE_STRING, &s);
    }
    va_end(ap);
    dbus_message_iter_close_container(&entry, &v);
    dbus_message_iter_close_container(&arr, &entry);
    dbus_message_iter_close_container(&it, &arr);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &inv); /* as 恒空 */
    dbus_message_iter_close_container(&it, &inv);
    dbus_connection_send(g_conn, sig, NULL);
    dbus_message_unref(sig);
    LOGD("signal propertiesChanged: %s", name);
}

void gps_adaptor_emit_enabled(void)
{
    emit_prop_changed("enabled", DBUS_TYPE_BOOLEAN, gps_monitor_enabled() ? 1 : 0);
}

/* ---------- 属性值封送(Properties.Get/GetAll 共用) ---------- */
static const char *prop_sig(const char *name)
{
    if (!strcmp(name, "gps_available") || !strcmp(name, "fix_valid") ||
        !strcmp(name, "enabled"))
        return DBUS_TYPE_BOOLEAN_AS_STRING;
    if (!strcmp(name, "latitude") || !strcmp(name, "longitude") ||
        !strcmp(name, "altitude"))
        return DBUS_TYPE_DOUBLE_AS_STRING;
    return DBUS_TYPE_INT64_AS_STRING;                /* fix_age_ms */
}

static void append_prop(DBusMessageIter *iter, const char *name)
{
    if (!strcmp(name, "gps_available"))
        dbus_message_iter_append_basic(iter, DBUS_TYPE_BOOLEAN,
                                       &(dbus_bool_t){ gps_pos_available() ? TRUE : FALSE });
    else if (!strcmp(name, "enabled"))
        dbus_message_iter_append_basic(iter, DBUS_TYPE_BOOLEAN,
                                       &(dbus_bool_t){ gps_monitor_enabled() ? TRUE : FALSE });
    else if (!strcmp(name, "fix_valid"))
        dbus_message_iter_append_basic(iter, DBUS_TYPE_BOOLEAN,
                                       &(dbus_bool_t){ g_pos.fix_valid ? TRUE : FALSE });
    else if (!strcmp(name, "latitude"))
        dbus_message_iter_append_basic(iter, DBUS_TYPE_DOUBLE, &g_pos.lat);
    else if (!strcmp(name, "longitude"))
        dbus_message_iter_append_basic(iter, DBUS_TYPE_DOUBLE, &g_pos.lon);
    else if (!strcmp(name, "altitude"))
        dbus_message_iter_append_basic(iter, DBUS_TYPE_DOUBLE, &g_pos.alt);
    else { /* fix_age_ms */
        dbus_int64_t age = (dbus_int64_t)gps_pos_age_ms();
        dbus_message_iter_append_basic(iter, DBUS_TYPE_INT64, &age);
    }
}

static const char *const PROP_TABLE[] = {
    "gps_available", "latitude", "longitude", "altitude",
    "fix_valid", "fix_age_ms", "enabled",
};
#define PROP_COUNT ((int)(sizeof PROP_TABLE / sizeof PROP_TABLE[0]))

/* ---------- org.freedesktop.DBus.Properties ---------- */
static DBusHandlerResult handle_get(DBusConnection *c, DBusMessage *m)
{
    DBusMessageIter args;
    char *iface_arg = NULL, *name = NULL;
    if (!dbus_message_iter_init(m, &args) ||
        dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_STRING)
        return reply_err(c, m, "org.freedesktop.DBus.Error.InvalidArgs", "bad Get args");
    dbus_message_iter_get_basic(&args, &iface_arg);      /* 借用指针, 不 free */
    dbus_message_iter_next(&args);
    if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_STRING)
        return reply_err(c, m, "org.freedesktop.DBus.Error.InvalidArgs", "bad Get args");
    dbus_message_iter_get_basic(&args, &name);
    (void)iface_arg;                                     /* 单接口服务 */

    int found = 0;
    for (int i = 0; i < PROP_COUNT; i++) found |= !strcmp(PROP_TABLE[i], name);
    if (!found) {
        char msg[128];
        snprintf(msg, sizeof msg, "No such property '%s'", name ? name : "?");
        return reply_err(c, m, ERR_PARAM, msg);
    }
    DBusMessage *rep = dbus_message_new_method_return(m);
    DBusMessageIter it, v;
    dbus_message_iter_init_append(rep, &it);
    dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, prop_sig(name), &v);
    append_prop(&v, name);
    dbus_message_iter_close_container(&it, &v);
    dbus_connection_send(c, rep, NULL);
    dbus_message_unref(rep);
    return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult handle_get_all(DBusConnection *c, DBusMessage *m)
{
    DBusMessageIter args;
    char *iface_arg = NULL;
    if (dbus_message_iter_init(m, &args) &&
        dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_STRING)
        dbus_message_iter_get_basic(&args, &iface_arg);
    (void)iface_arg;                                     /* 宽容任意 interface 实参 */
    DBusMessage *rep = dbus_message_new_method_return(m);
    DBusMessageIter it, arr;
    dbus_message_iter_init_append(rep, &it);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &arr);
    for (int i = 0; i < PROP_COUNT; i++) {
        DBusMessageIter entry, v;
        dbus_message_iter_open_container(&arr, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &PROP_TABLE[i]);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
                                         prop_sig(PROP_TABLE[i]), &v);
        append_prop(&v, PROP_TABLE[i]);
        dbus_message_iter_close_container(&entry, &v);
        dbus_message_iter_close_container(&arr, &entry);
    }
    dbus_message_iter_close_container(&it, &arr);
    dbus_connection_send(c, rep, NULL);
    dbus_message_unref(rep);
    return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult handle_set(DBusConnection *c, DBusMessage *m)
{
    /* 全部属性只读(启停走 gps_enable/gps_disable 方法) */
    return reply_err(c, m, ERR_PARAM, "gpsd properties are read-only");
}

/* ---------- 业务方法 ---------- */

/* SetPosition(d lat, d lon, d alt, d dop, d speed, d track, u nsat,
 *            b fix_valid, s utc) → ()
 * 主数据通道(PC/手机推坐标)。alt=NaN 表示无高程; lat/lon 必须有限且在域内。 */
static DBusHandlerResult m_set_position(DBusConnection *c, DBusMessage *m)
{
    DBusError e;
    dbus_error_init(&e);
    double lat = 0, lon = 0, alt = 0, dop = 0, speed = 0, track = 0;
    dbus_uint32_t nsat = 0;
    dbus_bool_t fix_valid = FALSE;
    const char *utc = NULL;
    if (!dbus_message_get_args(m, &e,
                               DBUS_TYPE_DOUBLE, &lat, DBUS_TYPE_DOUBLE, &lon,
                               DBUS_TYPE_DOUBLE, &alt, DBUS_TYPE_DOUBLE, &dop,
                               DBUS_TYPE_DOUBLE, &speed, DBUS_TYPE_DOUBLE, &track,
                               DBUS_TYPE_UINT32, &nsat, DBUS_TYPE_BOOLEAN, &fix_valid,
                               DBUS_TYPE_STRING, &utc, DBUS_TYPE_INVALID)) {
        DBusHandlerResult r = reply_err(c, m, "org.freedesktop.DBus.Error.InvalidArgs",
                                        e.message ? e.message : "expect ddddddubs");
        dbus_error_free(&e);
        return r;
    }
    if (isnan(lat) || isnan(lon) || isinf(lat) || isinf(lon) ||
        lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
        return reply_err(c, m, ERR_PARAM, "latitude/longitude 越界或非有限值");
    }
    g_pos.lat = lat;
    g_pos.lon = lon;
    if (isnan(alt) || isinf(alt)) { g_pos.alt = 0; g_pos.have_alt = 0; }
    else                          { g_pos.alt = alt; g_pos.have_alt = 1; }
    g_pos.dop = (isnan(dop) || isinf(dop)) ? 0 : dop;
    g_pos.speed = (isnan(speed) || isinf(speed)) ? 0 : speed;
    g_pos.track = (isnan(track) || isinf(track)) ? 0 : track;
    g_pos.nsat = nsat;
    g_pos.fix_valid = fix_valid ? 1 : 0;
    snprintf(g_pos.utc, sizeof g_pos.utc, "%s", (utc && *utc) ? utc : "");
    gps_pos_stamp("dbus");

    LOGI("SetPosition: lat=%.6f lon=%.6f alt=%.1f fix=%d nsat=%u", lat, lon,
         g_pos.have_alt ? g_pos.alt : 0.0, g_pos.fix_valid, nsat);
    gps_adaptor_on_position_updated();
    return reply_empty(c, m);
}

/* GetPosition() → a{sv} 全缓存字段 + age + enabled/source */
static DBusHandlerResult m_get_position(DBusConnection *c, DBusMessage *m)
{
    DBusMessage *rep = dbus_message_new_method_return(m);
    DBusMessageIter it, arr;
    dbus_message_iter_init_append(rep, &it);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &arr);
    append_position_dict(&arr);
    dict_b(&arr, "enabled", gps_monitor_enabled() ? TRUE : FALSE);
    dict_b(&arr, "gps_available", gps_pos_available() ? TRUE : FALSE);
    dbus_message_iter_close_container(&it, &arr);
    dbus_connection_send(c, rep, NULL);
    dbus_message_unref(rep);
    return DBUS_HANDLER_RESULT_HANDLED;
}

/* gps_enable(i)/gps_disable(i) —— X1D II GpsdProxy (int) 签名兼容;
 * 无参调用同样接受。enable=注入管线启停(disable 时只缓存不写照片)。 */
static DBusHandlerResult m_gps_enable(DBusConnection *c, DBusMessage *m, int on)
{
    DBusMessageIter it;
    if (dbus_message_iter_init(m, &it) &&
        dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_INT32) {
        dbus_int32_t v = 0;
        dbus_message_iter_get_basic(&it, &v);            /* int 参数语义同 X1D II: 忽略 */
        (void)v;
    }
    gps_monitor_set_enabled(on);
    gps_adaptor_emit_enabled();
    return reply_empty(c, m);
}

/* SimulateFix() → () —— 致敬 X1D 工厂 GPS_Simulation(哥德堡坐标,
 * docs/GPS_LIFECYCLE_X1D_X1DII.md §1.1.2 注入集同源) */
static DBusHandlerResult m_simulate_fix(DBusConnection *c, DBusMessage *m)
{
    g_pos.lat = 57.705617;         /* 57°42.365'N */
    g_pos.lon = 11.935300;         /* 11°56.118'E */
    g_pos.alt = 10.0;
    g_pos.have_alt = 1;
    g_pos.dop = 1.0;
    g_pos.speed = 0;
    g_pos.track = 0;
    g_pos.nsat = 8;
    g_pos.fix_valid = 1;
    time_t t = time(NULL);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    strftime(g_pos.utc, sizeof g_pos.utc, "%Y-%m-%d %H:%M:%S", &tmv);
    gps_pos_stamp("simulate");
    LOGI("SimulateFix: Göteborg 57.705617 11.935300 (X1D 工厂模拟致敬)");
    gps_adaptor_on_position_updated();
    return reply_empty(c, m);
}

static DBusHandlerResult handle_methods(DBusConnection *c, DBusMessage *m)
{
    if (dbus_message_is_method_call(m, GPSD_IFACE, "SetPosition")) return m_set_position(c, m);
    if (dbus_message_is_method_call(m, GPSD_IFACE, "GetPosition")) return m_get_position(c, m);
    if (dbus_message_is_method_call(m, GPSD_IFACE, "gps_enable"))  return m_gps_enable(c, m, 1);
    if (dbus_message_is_method_call(m, GPSD_IFACE, "gps_disable")) return m_gps_enable(c, m, 0);
    if (dbus_message_is_method_call(m, GPSD_IFACE, "SimulateFix")) return m_simulate_fix(c, m);
    if (dbus_message_has_destination(m, GPSD_NAME))
        return reply_err(c, m, ERR_FAILED, "No such method");
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* ---------- IBIS 客户端调用(x2gps/setgps.c 逻辑, 同连接复用) ----------
 * com.hasselblad.camera /camera set_gps_position(a{sv}) 只喂 IBIS 星野
 * 补偿不进照片(实测) —— 纯增益副产品, 失败仅告警不影响 GPS 打点。 */
int gps_adaptor_ibis_set(double lat, double lon)
{
    if (!g_conn) return -1;
    DBusMessage *req = dbus_message_new_method_call(
        "com.hasselblad.camera", "/camera", "com.hasselblad.camera", "set_gps_position");
    if (!req) return -1;
    DBusMessageIter it, arr, entry, v;
    const char *k;
    dbus_message_iter_init_append(req, &it);
    if (!dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &arr)) {
        dbus_message_unref(req);
        return -1;
    }
    k = "latitude";
    dbus_message_iter_open_container(&arr, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &k);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
                                     DBUS_TYPE_DOUBLE_AS_STRING, &v);
    dbus_message_iter_append_basic(&v, DBUS_TYPE_DOUBLE, &lat);
    dbus_message_iter_close_container(&entry, &v);
    dbus_message_iter_close_container(&arr, &entry);
    k = "longitude";
    dbus_message_iter_open_container(&arr, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &k);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
                                     DBUS_TYPE_DOUBLE_AS_STRING, &v);
    dbus_message_iter_append_basic(&v, DBUS_TYPE_DOUBLE, &lon);
    dbus_message_iter_close_container(&entry, &v);
    dbus_message_iter_close_container(&arr, &entry);
    dbus_message_iter_close_container(&it, &arr);

    DBusError e;
    dbus_error_init(&e);
    DBusMessage *rep = dbus_connection_send_with_reply_and_block(g_conn, req, 2000, &e);
    dbus_message_unref(req);
    if (!rep) {
        LOGW("set_gps_position(lat=%.5f lon=%.5f) 调用失败: %s", lat, lon,
             e.message ? e.message : "?");
        dbus_error_free(&e);
        return -1;
    }
    if (dbus_message_get_type(rep) == DBUS_MESSAGE_TYPE_ERROR) {
        LOGW("set_gps_position 错误回复: %s",
             dbus_message_get_error_name(rep) ? dbus_message_get_error_name(rep) : "?");
        dbus_message_unref(rep);
        return -1;
    }
    dbus_message_unref(rep);
    LOGI("set_gps_position(IBIS) ok lat=%.5f lon=%.5f", lat, lon);
    return 0;
}

void gps_adaptor_on_position_updated(void)
{
    emit_position_changed();
    /* 属性面: 缓存更新时逐项广播(fix_age_ms 同步随之更新) */
    emit_prop_changed("gps_available", DBUS_TYPE_BOOLEAN, gps_pos_available() ? 1 : 0);
    emit_prop_changed("latitude", DBUS_TYPE_DOUBLE, g_pos.lat);
    emit_prop_changed("longitude", DBUS_TYPE_DOUBLE, g_pos.lon);
    emit_prop_changed("altitude", DBUS_TYPE_DOUBLE, g_pos.alt);
    emit_prop_changed("fix_valid", DBUS_TYPE_BOOLEAN, g_pos.fix_valid ? 1 : 0);
    emit_prop_changed("fix_age_ms", DBUS_TYPE_INT64, (long long)gps_pos_age_ms());

    /* IBIS 节流: 首次, 或位移>~55m(0.0005°), 或距上次>60s */
    if (!g_cfg.ibis_compensate || !g_pos.fix_valid) return;
    long long now = gps_now_ms();
    double moved = fabs(g_pos.lat - s_ibis_lat) + fabs(g_pos.lon - s_ibis_lon);
    if (s_ibis_sent && moved < 0.0005 && now - s_ibis_last_ms < 60000) return;
    if (gps_adaptor_ibis_set(g_pos.lat, g_pos.lon) == 0) {
        s_ibis_lat = g_pos.lat;
        s_ibis_lon = g_pos.lon;
        s_ibis_sent = 1;
        s_ibis_last_ms = now;
    }
}

/* ---------- 触发层: /camera /storage propertiesChanged 订阅 ----------
 * 两种拼写都收(平台小写方言 + 标准拼写并存于原厂二进制);
 * counter 变化 → monitor 延迟扫描(宽匹配由总线保证只有这两个 sender 送达)。 */
static DBusHandlerResult signal_filter(DBusConnection *c, DBusMessage *m, void *data)
{
    (void)c; (void)data;
    if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_SIGNAL) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    const char *iface = dbus_message_get_interface(m);
    const char *member = dbus_message_get_member(m);
    if (!iface || !member) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    if (strcmp(iface, GPSD_IFACE_PROPERTIES)) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    if (strcmp(member, "propertiesChanged") && strcmp(member, "PropertiesChanged"))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    DBusMessageIter it;
    if (!dbus_message_iter_init(m, &it) ||
        dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING)
        return DBUS_HANDLER_RESULT_HANDLED;
    dbus_message_iter_next(&it);                         /* arg0 接口名跳过 */
    if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY)
        return DBUS_HANDLER_RESULT_HANDLED;
    DBusMessageIter arr;
    dbus_message_iter_recurse(&it, &arr);
    int poked = 0;
    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry, v;
        const char *key = NULL;
        dbus_message_iter_recurse(&arr, &entry);
        dbus_message_iter_get_basic(&entry, &key);
        dbus_message_iter_next(&entry);
        dbus_message_iter_recurse(&entry, &v);
        int vt = dbus_message_iter_get_arg_type(&v);
        (void)vt;                                        /* 值本体不需要, 只看键 */
        if (key && !strcmp(key, "exposures_processing_counter")) {
            LOGD("camera exposures_processing_counter 变化");
            if (!poked) { gps_monitor_event_poke(); poked = 1; }
        } else if (key && !strcmp(key, "storage_processing_counter")) {
            LOGD("storage storage_processing_counter 变化");
            if (!poked) { gps_monitor_event_poke(); poked = 1; }
        } else if (key) {
            LOGD("props 信号键 %s(不触发)", key);
        }
        dbus_message_iter_next(&arr);
    }
    return DBUS_HANDLER_RESULT_HANDLED;
}

int gps_adaptor_install_subscriptions(DBusConnection *c)
{
    DBusError e;
    dbus_error_init(&e);
    /* 宽匹配: 仅 sender+interface —— 绝不同时含 path+member(窄匹配不配送) */
    const char *rules[] = {
        "type='signal',sender='com.hasselblad.camera',"
        "interface='org.freedesktop.DBus.Properties'",
        "type='signal',sender='com.hasselblad.storage',"
        "interface='org.freedesktop.DBus.Properties'",
    };
    for (size_t i = 0; i < sizeof rules / sizeof rules[0]; i++) {
        dbus_bus_add_match(c, rules[i], &e);
        if (dbus_error_is_set(&e)) {
            LOGE("add_match 失败: %s (%s)", e.message, rules[i]);
            dbus_error_free(&e);
            return -1;
        }
        /* Qt 客户端怪癖: 连接后重复一次 AddMatch(odindb-cli 实录, 无害) */
        dbus_bus_add_match(c, rules[i], &e);
        dbus_error_free(&e);
        dbus_error_init(&e);
    }
    if (!dbus_connection_add_filter(c, signal_filter, NULL, NULL)) {
        LOGE("add_filter 失败");
        return -1;
    }
    LOGI("已订阅 camera/storage propertiesChanged(宽匹配×2, 重复 AddMatch)");
    return 0;
}

/* ---------- 手工 Introspect XML(与 dbus/INTERFACE.md 一致) ---------- */
static const char INTROSPECTION_XML[] =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
    "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
    "<node name=\"gpsd\">\n"
    "  <interface name=\"com.hasselblad.gpsd\">\n"
    "    <method name=\"SetPosition\">\n"
    "      <arg name=\"lat\" direction=\"in\" type=\"d\"/>\n"
    "      <arg name=\"lon\" direction=\"in\" type=\"d\"/>\n"
    "      <arg name=\"alt\" direction=\"in\" type=\"d\"/>\n"
    "      <arg name=\"dop\" direction=\"in\" type=\"d\"/>\n"
    "      <arg name=\"speed\" direction=\"in\" type=\"d\"/>\n"
    "      <arg name=\"track\" direction=\"in\" type=\"d\"/>\n"
    "      <arg name=\"nsat\" direction=\"in\" type=\"u\"/>\n"
    "      <arg name=\"fix_valid\" direction=\"in\" type=\"b\"/>\n"
    "      <arg name=\"utc\" direction=\"in\" type=\"s\"/>\n"
    "    </method>\n"
    "    <method name=\"GetPosition\">\n"
    "      <arg name=\"position\" direction=\"out\" type=\"a{sv}\"/>\n"
    "    </method>\n"
    "    <method name=\"gps_enable\">\n"
    "      <arg name=\"mode\" direction=\"in\" type=\"i\"/>\n"
    "    </method>\n"
    "    <method name=\"gps_disable\">\n"
    "      <arg name=\"mode\" direction=\"in\" type=\"i\"/>\n"
    "    </method>\n"
    "    <method name=\"SimulateFix\"/>\n"
    "    <signal name=\"positionChanged\">\n"
    "      <arg name=\"position\" type=\"a{sv}\"/>\n"
    "    </signal>\n"
    "    <signal name=\"nmea_sentence\">\n"
    "      <arg name=\"data\" type=\"ay\"/>\n"
    "      <arg name=\"length\" type=\"y\"/>\n"
    "    </signal>\n"
    "    <property name=\"gps_available\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"latitude\" type=\"d\" access=\"read\"/>\n"
    "    <property name=\"longitude\" type=\"d\" access=\"read\"/>\n"
    "    <property name=\"altitude\" type=\"d\" access=\"read\"/>\n"
    "    <property name=\"fix_valid\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"fix_age_ms\" type=\"x\" access=\"read\"/>\n"
    "    <property name=\"enabled\" type=\"b\" access=\"read\"/>\n"
    "  </interface>\n"
    "  <interface name=\"org.freedesktop.DBus.Properties\">\n"
    "    <method name=\"Get\"><arg name=\"iface\" direction=\"in\" type=\"s\"/><arg name=\"name\" direction=\"in\" type=\"s\"/><arg name=\"value\" direction=\"out\" type=\"v\"/></method>\n"
    "    <method name=\"Set\"><arg name=\"iface\" direction=\"in\" type=\"s\"/><arg name=\"name\" direction=\"in\" type=\"s\"/><arg name=\"value\" direction=\"in\" type=\"v\"/></method>\n"
    "    <method name=\"GetAll\"><arg name=\"iface\" direction=\"in\" type=\"s\"/><arg name=\"props\" direction=\"out\" type=\"a{sv}\"/></method>\n"
    "    <signal name=\"propertiesChanged\">\n"
    "      <arg name=\"interface\" type=\"s\"/>\n"
    "      <arg name=\"changed\" type=\"a{sv}\"/>\n"
    "      <arg name=\"invalidated\" type=\"as\"/>\n"
    "    </signal>\n"
    "  </interface>\n"
    "  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
    "    <method name=\"Introspect\"><arg name=\"xml\" direction=\"out\" type=\"s\"/></method>\n"
    "  </interface>\n"
    "  <interface name=\"org.freedesktop.DBus.Peer\">\n"
    "    <method name=\"Ping\"/>\n"
    "  </interface>\n"
    "</node>\n";

/* ---------- 对象过滤器总入口 ---------- */
static DBusHandlerResult obj_filter(DBusConnection *c, DBusMessage *m, void *data)
{
    (void)data;
    if (dbus_message_is_method_call(m, GPSD_IFACE_INTROSPECT, "Introspect")) {
        const char *xml = INTROSPECTION_XML;
        DBusMessage *rep = dbus_message_new_method_return(m);
        dbus_message_append_args(rep, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID);
        dbus_connection_send(c, rep, NULL);
        dbus_message_unref(rep);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(m, GPSD_IFACE_PEER, "Ping"))
        return reply_empty(c, m);
    if (dbus_message_is_method_call(m, GPSD_IFACE_PROPERTIES, "GetAll")) return handle_get_all(c, m);
    if (dbus_message_is_method_call(m, GPSD_IFACE_PROPERTIES, "Get"))    return handle_get(c, m);
    if (dbus_message_is_method_call(m, GPSD_IFACE_PROPERTIES, "Set"))    return handle_set(c, m);
    return handle_methods(c, m);
}

int gps_adaptor_start(DBusConnection *c)
{
    g_conn = c;
    /* {unregister, message, pad×4} —— 不是裸函数指针! */
    static DBusObjectPathVTable vt = { NULL, obj_filter, NULL, NULL, NULL, NULL };
    if (!dbus_connection_register_object_path(c, GPSD_PATH, &vt, NULL)) {
        LOGE("register_object_path %s failed", GPSD_PATH);
        return -1;
    }
    LOGI("adaptor: object %s registered", GPSD_PATH);
    return 0;
}
