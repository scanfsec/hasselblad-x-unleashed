/* gpsctl — com.hasselblad.gpsd 的 CLI 客户端(busctl/dbus-send 不可用时的自产工具)
 * 用法:
 *   gpsctl [--bus ADDR] set <lat> <lon> [alt] [--dop f] [--speed f] [--track f]
 *                                 [--nsat n] [--utc "s"] [--nofix]
 *   gpsctl [--bus ADDR] get <属性名> | get-all
 *   gpsctl [--bus ADDR] status            # 人读状态总览(GetPosition+属性)
 *   gpsctl [--bus ADDR] enable | disable  # gps_enable(i)/gps_disable(i)
 *   gpsctl [--bus ADDR] simulate          # SimulateFix(X1D 工厂后门致敬)
 *   gpsctl [--bus ADDR] monitor           # 跟本服务全部信号(Ctrl-C 退出)
 * 纯 libdbus, 无第三方依赖; printerctl(printer-service)同构。
 * 订阅用宽匹配(仅 sender —— 窄匹配含 path+member 相机 dbus-daemon 不配送),
 * 并重复一次 AddMatch(Qt 客户端怪癖, odindb-cli 实录)。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <dbus/dbus.h>

#define GPSD_DEST "com.hasselblad.gpsd"
#define GPSD_PATH "/gpsd"
#define GPSD_IFACE GPSD_DEST
#define IFACE_PROPERTIES "org.freedesktop.DBus.Properties"

static DBusConnection *g_conn;
static volatile sig_atomic_t g_mon_run = 1;

static void on_sig(int s) { (void)s; g_mon_run = 0; }

static void usage(void)
{
    fputs(
        "用法: gpsctl [--bus ADDR] <命令>\n"
        "  set <lat> <lon> [alt] [--dop F] [--speed F] [--track F] [--nsat N]\n"
        "                    [--utc \"YYYY-MM-DD hh:mm:ss\"] [--nofix]\n"
        "                                       推坐标(SetPosition; --nofix 只更新缓存为无定位)\n"
        "  get <属性名>                         读单属性(Properties.Get)\n"
        "  get-all                              全属性(GetAll)\n"
        "  status                               人读状态总览\n"
        "  enable | disable                     启停注入管线(gps_enable/gps_disable)\n"
        "  simulate                             SimulateFix(哥德堡测试坐标)\n"
        "  monitor                              持续监听本服务信号(Ctrl-C 退出)\n"
        "属性: gps_available latitude longitude altitude fix_valid fix_age_ms enabled\n",
        stderr);
}

/* ---------- 总线连接 ---------- */
static DBusConnection *connect_bus(const char *addr)
{
    DBusError e;
    dbus_error_init(&e);
    DBusConnection *c;
    if (addr) {
        c = dbus_connection_open_private(addr, &e);
        if (c && !dbus_bus_register(c, &e)) c = NULL;
    } else if (getenv("DBUS_SYSTEM_BUS_ADDRESS")) {
        c = dbus_bus_get(DBUS_BUS_SYSTEM, &e);
    } else if (getenv("DBUS_SESSION_BUS_ADDRESS")) {
        c = dbus_bus_get(DBUS_BUS_SESSION, &e);
    } else {
        c = dbus_bus_get(DBUS_BUS_SYSTEM, &e);
    }
    if (!c) {
        fprintf(stderr, "gpsctl: 连接总线失败: %s\n", e.message ? e.message : "?");
        dbus_error_free(&e);
        exit(1);
    }
    return c;
}

/* ---------- 人读打印: 递归打印任意 D-Bus 值 ---------- */
static void print_value(DBusMessageIter *iter, int depth);

static void print_dict_sv(DBusMessageIter *arr)
{
    DBusMessageIter entry;
    int first = 1;
    printf("{");
    while (dbus_message_iter_get_arg_type(arr) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter v;
        const char *key = NULL;
        dbus_message_iter_recurse(arr, &entry);
        dbus_message_iter_get_basic(&entry, &key);
        dbus_message_iter_next(&entry);
        dbus_message_iter_recurse(&entry, &v);
        printf("%s%s: ", first ? "" : ", ", key);
        print_value(&v, 1);
        dbus_message_iter_next(arr);
        first = 0;
    }
    printf("}");
}

static void print_value(DBusMessageIter *iter, int depth)
{
    (void)depth;
    int t = dbus_message_iter_get_arg_type(iter);
    DBusMessageIter sub;
    switch (t) {
    case DBUS_TYPE_BOOLEAN: { dbus_bool_t b; dbus_message_iter_get_basic(iter, &b); printf("%s", b ? "true" : "false"); break; }
    case DBUS_TYPE_BYTE:    { unsigned char v; dbus_message_iter_get_basic(iter, &v); printf("%u", (unsigned)v); break; }
    case DBUS_TYPE_INT32:   { dbus_int32_t v; dbus_message_iter_get_basic(iter, &v); printf("%d", (int)v); break; }
    case DBUS_TYPE_UINT32:  { dbus_uint32_t v; dbus_message_iter_get_basic(iter, &v); printf("%u", (unsigned)v); break; }
    case DBUS_TYPE_INT64:   { dbus_int64_t v; dbus_message_iter_get_basic(iter, &v); printf("%lld", (long long)v); break; }
    case DBUS_TYPE_DOUBLE:  { double v; dbus_message_iter_get_basic(iter, &v); printf("%.7f", v); break; }
    case DBUS_TYPE_STRING:  { const char *s; dbus_message_iter_get_basic(iter, &s); printf("%s", s ? s : ""); break; }
    case DBUS_TYPE_VARIANT:
        dbus_message_iter_recurse(iter, &sub);
        print_value(&sub, depth + 1);
        break;
    case DBUS_TYPE_ARRAY: {
        dbus_message_iter_recurse(iter, &sub);
        if (dbus_message_iter_get_arg_type(&sub) == DBUS_TYPE_DICT_ENTRY) {
            print_dict_sv(&sub);
        } else if (dbus_message_iter_get_arg_type(&sub) == DBUS_TYPE_BYTE) {
            printf("[");
            int first = 1, n = 0;
            while (dbus_message_iter_get_arg_type(&sub) == DBUS_TYPE_BYTE && n < 96) {
                unsigned char b;
                dbus_message_iter_get_basic(&sub, &b);
                printf("%s%02x", first ? "" : " ", b);
                dbus_message_iter_next(&sub);
                first = 0; n++;
            }
            if (dbus_message_iter_get_arg_type(&sub) == DBUS_TYPE_BYTE) printf(" …");
            printf("]");
        } else {
            int first = 1;
            printf("[");
            while (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_INVALID) {
                printf("%s", first ? "" : ", ");
                print_value(&sub, depth + 1);
                dbus_message_iter_next(&sub);
                first = 0;
            }
            printf("]");
        }
        break;
    }
    case DBUS_TYPE_INVALID: printf("(none)"); break;
    default: printf("<type %c>", t); break;
    }
}

/* 调用并打印回复; 返回 0=ok 1=错误 */
static int call_and_print(DBusMessage *req, int timeout_ms)
{
    DBusError e;
    dbus_error_init(&e);
    DBusMessage *rep = dbus_connection_send_with_reply_and_block(g_conn, req, timeout_ms, &e);
    if (!rep) {
        fprintf(stderr, "gpsctl: 调用失败: %s: %s\n", e.name ? e.name : "?",
                e.message ? e.message : "");
        dbus_error_free(&e);
        return 1;
    }
    if (dbus_message_get_type(rep) == DBUS_MESSAGE_TYPE_ERROR) {
        const char *emsg = "";
        DBusMessageIter it;
        if (dbus_message_iter_init(rep, &it) &&
            dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRING)
            dbus_message_iter_get_basic(&it, &emsg);
        fprintf(stderr, "gpsctl: 错误: %s: %s\n",
                dbus_message_get_error_name(rep) ? dbus_message_get_error_name(rep) : "?", emsg);
        dbus_message_unref(rep);
        return 1;
    }
    DBusMessageIter it;
    if (dbus_message_iter_init(rep, &it)) {
        do {
            print_value(&it, 0);
            printf("\n");
        } while (dbus_message_iter_next(&it));
    }
    dbus_message_unref(rep);
    return 0;
}

/* ---------- get / get-all ---------- */
static DBusMessage *new_props_call(const char *member)
{
    return dbus_message_new_method_call(GPSD_DEST, GPSD_PATH, IFACE_PROPERTIES, member);
}

static int cmd_get(const char *prop)
{
    const char *iface = GPSD_IFACE;
    DBusMessage *req = new_props_call("Get");
    dbus_message_append_args(req, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop,
                             DBUS_TYPE_INVALID);
    printf("%s = ", prop);
    int rc = call_and_print(req, 15000);
    dbus_message_unref(req);
    return rc;
}

static int cmd_get_all(void)
{
    const char *iface = GPSD_IFACE;
    DBusMessage *req = new_props_call("GetAll");
    dbus_message_append_args(req, DBUS_TYPE_STRING, &iface, DBUS_TYPE_INVALID);
    DBusError e;
    dbus_error_init(&e);
    DBusMessage *rep = dbus_connection_send_with_reply_and_block(g_conn, req, 15000, &e);
    dbus_message_unref(req);
    if (!rep) {
        fprintf(stderr, "gpsctl: GetAll 失败: %s\n", e.message ? e.message : "?");
        dbus_error_free(&e);
        return 1;
    }
    if (dbus_message_get_type(rep) == DBUS_MESSAGE_TYPE_ERROR) {
        fprintf(stderr, "gpsctl: 错误: %s\n",
                dbus_message_get_error_name(rep) ? dbus_message_get_error_name(rep) : "?");
        dbus_message_unref(rep);
        return 1;
    }
    DBusMessageIter it, arr;
    dbus_message_iter_init(rep, &it);
    dbus_message_iter_recurse(&it, &arr);
    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry, v;
        const char *key = NULL;
        dbus_message_iter_recurse(&arr, &entry);
        dbus_message_iter_get_basic(&entry, &key);
        dbus_message_iter_next(&entry);
        dbus_message_iter_recurse(&entry, &v);
        printf("%-14s = ", key);
        print_value(&v, 0);
        printf("\n");
        dbus_message_iter_next(&arr);
    }
    dbus_message_unref(rep);
    return 0;
}

/* ---------- set: SetPosition(ddddddubs) ---------- */
static int cmd_set(char **args, int n)
{
    if (n < 2) { fprintf(stderr, "gpsctl: set <lat> <lon> [alt] [选项]\n"); return 2; }
    double lat = strtod(args[0], NULL);
    double lon = strtod(args[1], NULL);
    double alt = 0, dop = 0, speed = 0, track = 0;
    dbus_uint32_t nsat = 0;
    dbus_bool_t fix = TRUE;
    const char *utc = "";
    int have_alt = 0;

    for (int i = 2; i < n; i++) {
        if (!strcmp(args[i], "--nofix")) fix = FALSE;
        else if (!strncmp(args[i], "--alt=", 6))   { alt = strtod(args[i] + 6, NULL); have_alt = 1; }
        else if (!strncmp(args[i], "--dop=", 6))     dop = strtod(args[i] + 6, NULL);
        else if (!strncmp(args[i], "--speed=", 8))   speed = strtod(args[i] + 8, NULL);
        else if (!strncmp(args[i], "--track=", 8))   track = strtod(args[i] + 8, NULL);
        else if (!strncmp(args[i], "--nsat=", 7))    nsat = (dbus_uint32_t)strtoul(args[i] + 7, NULL, 10);
        else if (!strncmp(args[i], "--utc=", 6))     utc = args[i] + 6;
        else if (i == 2 && args[i][0] != '-')        { alt = strtod(args[i], NULL); have_alt = 1; }
        else { fprintf(stderr, "gpsctl: 未识别参数 '%s'\n", args[i]); return 2; }
    }
    if (!have_alt) alt = 0.0 / 0.0;                   /* NaN = 无高程 */

    DBusMessage *req = dbus_message_new_method_call(GPSD_DEST, GPSD_PATH, GPSD_IFACE, "SetPosition");
    if (!req) { fprintf(stderr, "oom\n"); return 1; }
    if (!dbus_message_append_args(req,
                                  DBUS_TYPE_DOUBLE, &lat, DBUS_TYPE_DOUBLE, &lon,
                                  DBUS_TYPE_DOUBLE, &alt, DBUS_TYPE_DOUBLE, &dop,
                                  DBUS_TYPE_DOUBLE, &speed, DBUS_TYPE_DOUBLE, &track,
                                  DBUS_TYPE_UINT32, &nsat, DBUS_TYPE_BOOLEAN, &fix,
                                  DBUS_TYPE_STRING, &utc, DBUS_TYPE_INVALID)) {
        dbus_message_unref(req);
        return 1;
    }
    int rc = call_and_print(req, 15000);
    dbus_message_unref(req);
    if (rc == 0) printf("SetPosition ok\n");
    return rc;
}

/* ---------- enable/disable/simulate ---------- */
static int cmd_simple_method(const char *member)
{
    DBusMessage *req = dbus_message_new_method_call(GPSD_DEST, GPSD_PATH, GPSD_IFACE, member);
    if (!req) { fprintf(stderr, "oom\n"); return 1; }
    dbus_int32_t mode = 0;                             /* X1D II (int) 签名兼容 */
    dbus_message_append_args(req, DBUS_TYPE_INT32, &mode, DBUS_TYPE_INVALID);
    int rc = call_and_print(req, 15000);
    dbus_message_unref(req);
    return rc;
}

/* ---------- status: GetPosition 人读总览 ---------- */
static int cmd_status(void)
{
    DBusMessage *req = dbus_message_new_method_call(GPSD_DEST, GPSD_PATH, GPSD_IFACE, "GetPosition");
    if (!req) { fprintf(stderr, "oom\n"); return 1; }
    DBusError e;
    dbus_error_init(&e);
    DBusMessage *rep = dbus_connection_send_with_reply_and_block(g_conn, req, 15000, &e);
    dbus_message_unref(req);
    if (!rep) {
        fprintf(stderr, "gpsctl: GetPosition 失败(服务未起?): %s\n",
                e.message ? e.message : "?");
        dbus_error_free(&e);
        return 1;
    }
    if (dbus_message_get_type(rep) == DBUS_MESSAGE_TYPE_ERROR) {
        fprintf(stderr, "gpsctl: 错误: %s\n",
                dbus_message_get_error_name(rep) ? dbus_message_get_error_name(rep) : "?");
        dbus_message_unref(rep);
        return 1;
    }
    DBusMessageIter it, arr;
    dbus_message_iter_init(rep, &it);
    dbus_message_iter_recurse(&it, &arr);
    double lat = 0, lon = 0, alt = 0;
    long long age = -1;
    int fix = 0, enabled = 0, avail = 0;
    char utc[64] = "", source[24] = "";
    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry, v;
        const char *key = NULL;
        dbus_message_iter_recurse(&arr, &entry);
        dbus_message_iter_get_basic(&entry, &key);
        dbus_message_iter_next(&entry);
        dbus_message_iter_recurse(&entry, &v);
        if (!strcmp(key, "latitude"))       dbus_message_iter_get_basic(&v, &lat);
        else if (!strcmp(key, "longitude")) dbus_message_iter_get_basic(&v, &lon);
        else if (!strcmp(key, "altitude"))  dbus_message_iter_get_basic(&v, &alt);
        else if (!strcmp(key, "fix_valid")) { dbus_bool_t b; dbus_message_iter_get_basic(&v, &b); fix = b; }
        else if (!strcmp(key, "enabled"))   { dbus_bool_t b; dbus_message_iter_get_basic(&v, &b); enabled = b; }
        else if (!strcmp(key, "gps_available")) { dbus_bool_t b; dbus_message_iter_get_basic(&v, &b); avail = b; }
        else if (!strcmp(key, "fix_age_ms")){ dbus_int64_t x; dbus_message_iter_get_basic(&v, &x); age = (long long)x; }
        else if (!strcmp(key, "utc"))       { const char *s; dbus_message_iter_get_basic(&v, &s); snprintf(utc, sizeof utc, "%s", s); }
        else if (!strcmp(key, "source"))    { const char *s; dbus_message_iter_get_basic(&v, &s); snprintf(source, sizeof source, "%s", s); }
        dbus_message_iter_next(&arr);
    }
    dbus_message_unref(rep);

    printf("== com.hasselblad.gpsd 状态 ==\n");
    printf("gps_available : %s\n", avail ? "true" : "false");
    printf("fix_valid     : %s\n", fix ? "true" : "false");
    printf("latitude      : %.7f\n", lat);
    printf("longitude     : %.7f\n", lon);
    printf("altitude      : %.2f m\n", alt);
    printf("fix_age_ms    : %lld (%.1fs)\n", age, age < 0 ? -1.0 : (double)age / 1000.0);
    printf("utc           : %s\n", utc[0] ? utc : "(无)");
    printf("source        : %s\n", source[0] ? source : "(无)");
    printf("enabled(注入) : %s\n", enabled ? "true (新照片写 GPS)" : "false (只缓存不写)");
    return 0;
}

/* ---------- monitor: 订阅本服务全部信号 ---------- */
static DBusHandlerResult mon_filter(DBusConnection *c, DBusMessage *m, void *data)
{
    (void)c; (void)data;
    if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_SIGNAL) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    const char *iface = dbus_message_get_interface(m);
    const char *member = dbus_message_get_member(m);
    if (!iface || !member) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (!strcmp(iface, IFACE_PROPERTIES) &&
        (!strcmp(member, "propertiesChanged") || !strcmp(member, "PropertiesChanged"))) {
        /* 平台方言: 首字母小写; 标准拼写也收 */
        DBusMessageIter it;
        printf("[propertiesChanged]\n");
        if (dbus_message_iter_init(m, &it)) {
            DBusMessageIter arr;
            dbus_message_iter_next(&it);                 /* arg0 接口名 */
            dbus_message_iter_recurse(&it, &arr);
            while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY) {
                DBusMessageIter entry, v;
                const char *key = NULL;
                dbus_message_iter_recurse(&arr, &entry);
                dbus_message_iter_get_basic(&entry, &key);
                dbus_message_iter_next(&entry);
                dbus_message_iter_recurse(&entry, &v);
                printf("  %-14s = ", key);
                print_value(&v, 0);
                printf("\n");
                dbus_message_iter_next(&arr);
            }
        }
        fflush(stdout);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (!strcmp(iface, GPSD_IFACE)) {
        DBusMessageIter it;
        printf("[%s]\n", member);
        if (dbus_message_iter_init(m, &it)) {
            int i = 0;
            do {
                printf("  arg%d = ", i++);
                print_value(&it, 0);
                printf("\n");
            } while (dbus_message_iter_next(&it));
        }
        fflush(stdout);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static int cmd_monitor(void)
{
    DBusError e;
    dbus_error_init(&e);
    /* 宽匹配: 仅 sender(窄匹配含 path+member 相机不配送) */
    const char *rule = "type='signal',sender='" GPSD_DEST "'";
    dbus_bus_add_match(g_conn, rule, &e);
    if (dbus_error_is_set(&e)) {
        fprintf(stderr, "gpsctl: add_match 失败: %s\n", e.message);
        dbus_error_free(&e);
        return 1;
    }
    dbus_bus_add_match(g_conn, rule, &e);              /* 重复 AddMatch(Qt 怪癖) */
    dbus_error_free(&e);
    if (!dbus_connection_add_filter(g_conn, mon_filter, NULL, NULL)) {
        fprintf(stderr, "gpsctl: add_filter 失败\n");
        return 1;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sig;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    printf("monitoring com.hasselblad.gpsd signals (Ctrl-C 退出)\n");
    while (g_mon_run && dbus_connection_read_write_dispatch(g_conn, 100))
        ;
    printf("\nmonitor 结束\n");
    return 0;
}

/* ---------- main ---------- */
int main(int argc, char **argv)
{
    const char *bus_addr = NULL;
    int i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "--bus") && i + 1 < argc) bus_addr = argv[++i];
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
        else break;
    }
    if (i >= argc) { usage(); return 2; }
    const char *cmd = argv[i];
    char **rest = argv + i + 1;
    int nrest = argc - i - 1;

    if (!strcmp(cmd, "get-all") || !strcmp(cmd, "--get-all")) {
        g_conn = connect_bus(bus_addr);
        return cmd_get_all();
    }
    if (!strcmp(cmd, "get")) {
        if (nrest != 1) { usage(); return 2; }
        g_conn = connect_bus(bus_addr);
        return cmd_get(rest[0]);
    }
    if (!strcmp(cmd, "set")) {
        g_conn = connect_bus(bus_addr);
        return cmd_set(rest, nrest);
    }
    if (!strcmp(cmd, "status")) {
        g_conn = connect_bus(bus_addr);
        return cmd_status();
    }
    if (!strcmp(cmd, "enable")) {
        g_conn = connect_bus(bus_addr);
        return cmd_simple_method("gps_enable");
    }
    if (!strcmp(cmd, "disable")) {
        g_conn = connect_bus(bus_addr);
        return cmd_simple_method("gps_disable");
    }
    if (!strcmp(cmd, "simulate")) {
        g_conn = connect_bus(bus_addr);
        return cmd_simple_method("SimulateFix");
    }
    if (!strcmp(cmd, "monitor")) {
        g_conn = connect_bus(bus_addr);
        return cmd_monitor();
    }
    usage();
    return 2;
}
