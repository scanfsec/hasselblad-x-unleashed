/* printerctl — com.hasselblad.printer 的 CLI 客户端(odindb-send 风格子集)
 * printerctl [--bus ADDR] get <prop> | set <prop> <val> | call <method> [args...]
 *            | monitor | --get-all
 * 纯 libdbus; 默认: 有 DBUS_SESSION_BUS_ADDRESS 用会话总线, 否则系统总线。
 * 属性/方法签名表直接复用 state.c(链接同一份, 避免漂移)。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <dbus/dbus.h>

#include "state.h"                 /* 属性元信息表 */

#define PRN_DEST "com.hasselblad.printer"
#define PRN_PATH "/printer"
#define PRN_IFACE PRN_DEST
#define IFACE_PROPERTIES "org.freedesktop.DBus.Properties"

static DBusConnection *g_conn;
static volatile sig_atomic_t g_mon_run = 1;

static void on_sig(int s) { (void)s; g_mon_run = 0; }

static void usage(void)
{
    fputs(
        "用法: printerctl [--bus ADDR] <命令>\n"
        "  get <属性名>              读单属性(Properties.Get)\n"
        "  set <属性名> <值>         写属性(Properties.Set, 类型自动封送)\n"
        "  call <方法名> [参数...]   调方法(签名表自动封送; 数字→i, true/false→b, 其余→s;\n"
        "                            Start_print_job 末尾 k=v 对进 a{sv} 选项字典)\n"
        "  caps                      人可读打印当前选中打印机的全部能力\n"
        "  syswifi on|off            相机合法 Wi-Fi 电源(com.hasselblad.system)\n"
        "  monitor                   持续监听本服务信号(Ctrl-C 退出)\n"
        "  --get-all                 全属性+当前值\n",
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
        if (c && !dbus_bus_register(c, &e)) { c = NULL; }
    } else if (getenv("DBUS_SESSION_BUS_ADDRESS")) {
        c = dbus_bus_get(DBUS_BUS_SESSION, &e);
    } else {
        c = dbus_bus_get(DBUS_BUS_SYSTEM, &e);
    }
    if (!c) {
        fprintf(stderr, "printerctl: 连接总线失败: %s\n", e.message ? e.message : "?");
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
    case DBUS_TYPE_BOOLEAN: {
        dbus_bool_t b; dbus_message_iter_get_basic(iter, &b); printf("%s", b ? "true" : "false"); break;
    }
    case DBUS_TYPE_INT32:   { dbus_int32_t v;  dbus_message_iter_get_basic(iter, &v); printf("%d", v); break; }
    case DBUS_TYPE_UINT32:  { dbus_uint32_t v; dbus_message_iter_get_basic(iter, &v); printf("%u", v); break; }
    case DBUS_TYPE_INT64:   { dbus_int64_t v;  dbus_message_iter_get_basic(iter, &v); printf("%lld", (long long)v); break; }
    case DBUS_TYPE_UINT64:  { dbus_uint64_t v; dbus_message_iter_get_basic(iter, &v); printf("%llu", (unsigned long long)v); break; }
    case DBUS_TYPE_DOUBLE:  { double v;         dbus_message_iter_get_basic(iter, &v); printf("%g", v); break; }
    case DBUS_TYPE_STRING:
    case DBUS_TYPE_OBJECT_PATH: {
        const char *s; dbus_message_iter_get_basic(iter, &s); printf("\"%s\"", s); break;
    }
    case DBUS_TYPE_VARIANT:
        dbus_message_iter_recurse(iter, &sub);
        print_value(&sub, depth + 1);
        break;
    case DBUS_TYPE_ARRAY: {
        /* 字典判定看首元素类型(libdbus 的 get_signature 对数组含 "a" 前缀,
         * 不能用 sig[0] 判 dict-entry —— §18 Get_photo_info 回 a{sv} 踩过) */
        dbus_message_iter_recurse(iter, &sub);
        if (dbus_message_iter_get_arg_type(&sub) == DBUS_TYPE_DICT_ENTRY) {
            print_dict_sv(&sub);
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
    case DBUS_TYPE_DICT_ENTRY: {                    /* 顶层直接给 dict entry 时 */
        print_dict_sv(iter);
        break;
    }
    case DBUS_TYPE_INVALID:
        printf("(none)");
        break;
    default:
        printf("<type %c>", t);
        break;
    }
}

/* 调用并打印回复; 返回 0=ok 1=错误回复 */
static int call_and_print(DBusConnection *c, DBusMessage *req, int want_reply)
{
    DBusError e;
    dbus_error_init(&e);
    if (!want_reply) {
        dbus_connection_send(c, req, NULL);
        dbus_connection_flush(c);
        return 0;
    }
    DBusMessage *rep = dbus_connection_send_with_reply_and_block(c, req, 60000, &e);
    if (!rep) {
        fprintf(stderr, "printerctl: 调用失败: %s: %s\n", e.name ? e.name : "?",
                e.message ? e.message : "");
        dbus_error_free(&e);
        return 1;
    }
    if (dbus_message_get_type(rep) == DBUS_MESSAGE_TYPE_ERROR) {
        /* libdbus: 错误正文=报文 arg0(string), 无专门 getter */
        const char *emsg = "";
        DBusMessageIter it;
        if (dbus_message_iter_init(rep, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRING)
            dbus_message_iter_get_basic(&it, &emsg);
        fprintf(stderr, "printerctl: 错误: %s: %s\n",
                dbus_message_get_error_name(rep) ? dbus_message_get_error_name(rep) : "?", emsg);
        dbus_message_unref(rep);
        return 1;
    }
    DBusMessageIter it;
    if (dbus_message_iter_init(rep, &it)) {
        int i = 0;
        do {
            if (i) printf("\n");
            printf("  ret%d = ", i++);
            print_value(&it, 0);
        } while (dbus_message_iter_next(&it));
        printf("\n");
    }
    dbus_message_unref(rep);
    return 0;
}

/* ---------- get / --get-all ---------- */
static DBusMessage *new_props_call(const char *member)
{
    return dbus_message_new_method_call(PRN_DEST, PRN_PATH, IFACE_PROPERTIES, member);
}

static int cmd_get(const char *prop)
{
    int idx = prn_prop_index(prop);
    if (idx < 0) { fprintf(stderr, "printerctl: 未知属性 '%s'\n", prop); return 2; }
    const char *iface = PRN_IFACE;
    DBusMessage *req = new_props_call("Get");
    dbus_message_append_args(req, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop,
                             DBUS_TYPE_INVALID);
    printf("%s = ", prop);
    int rc;
    DBusError e;
    dbus_error_init(&e);
    DBusMessage *rep = dbus_connection_send_with_reply_and_block(g_conn, req, 15000, &e);
    if (!rep) {
        printf("(失败: %s)\n", e.message ? e.message : "?");
        dbus_error_free(&e);
        rc = 1;
    } else if (dbus_message_get_type(rep) == DBUS_MESSAGE_TYPE_ERROR) {
        printf("(错误: %s)\n", dbus_message_get_error_name(rep));
        rc = 1;
    } else {
        DBusMessageIter it, v;
        dbus_message_iter_init(rep, &it);
        dbus_message_iter_recurse(&it, &v);
        print_value(&v, 0);
        printf("\n");
        rc = 0;
    }
    if (rep) dbus_message_unref(rep);
    dbus_message_unref(req);
    return rc;
}

static int cmd_get_all(void)
{
    const char *iface = PRN_IFACE;
    DBusMessage *req = new_props_call("GetAll");
    dbus_message_append_args(req, DBUS_TYPE_STRING, &iface, DBUS_TYPE_INVALID);
    DBusError e;
    dbus_error_init(&e);
    DBusMessage *rep = dbus_connection_send_with_reply_and_block(g_conn, req, 15000, &e);
    if (!rep) {
        fprintf(stderr, "printerctl: GetAll 失败: %s\n", e.message ? e.message : "?");
        dbus_error_free(&e);
        return 1;
    }
    if (dbus_message_get_type(rep) == DBUS_MESSAGE_TYPE_ERROR) {
        fprintf(stderr, "printerctl: 错误: %s\n", dbus_message_get_error_name(rep));
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
        printf("%-20s = ", key);
        print_value(&v, 0);
        printf("\n");
        dbus_message_iter_next(&arr);
    }
    dbus_message_unref(rep);
    dbus_message_unref(req);
    return 0;
}

/* ---------- set ---------- */
static int cmd_set(const char *prop, const char *val)
{
    int idx = prn_prop_index(prop);
    if (idx < 0) { fprintf(stderr, "printerctl: 未知属性 '%s'\n", prop); return 2; }
    char t = prn_prop_info_get(idx)->type;
    /* 可写性交给服务端裁决(本地只管按类型封送, 错误原样呈现) */
    const char *iface = PRN_IFACE;
    DBusMessage *req = new_props_call("Set");
    DBusMessageIter it, v;
    dbus_message_iter_init_append(req, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &prop);
    if (t == 'i') {
        char *end = NULL;
        long n = strtol(val, &end, 10);
        if (end == val || (end && *end)) { fprintf(stderr, "printerctl: '%s' 不是整数\n", val); return 2; }
        dbus_int32_t iv = (dbus_int32_t)n;
        dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, DBUS_TYPE_INT32_AS_STRING, &v);
        dbus_message_iter_append_basic(&v, DBUS_TYPE_INT32, &iv);
    } else {
        dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, DBUS_TYPE_STRING_AS_STRING, &v);
        dbus_message_iter_append_basic(&v, DBUS_TYPE_STRING, &val);
    }
    dbus_message_iter_close_container(&it, &v);
    int rc = call_and_print(g_conn, req, 1);
    if (rc == 0) printf("ok: %s 已设置\n", prop);
    dbus_message_unref(req);
    return rc;
}

/* ---------- call: 方法签名表 ---------- */
static const struct {
    const char *member;
    const char *sig;    /* 每字符一个 CLI 参数; 'D'=as+a{sv}(Start_print_job);
                           'E'=s+a{sv}(§18 Preview_print, 末尾 k=v 对进字典) */
} method_sigs[] = {
    { "Connect_to_ap",          "ss" },
    { "Scan_wifi",              ""   },   /* §20: → as "SSID\tRSSI\tSEC" */
    { "Disconnect_and_restore", ""   },
    { "Start_discovery",        ""   },
    { "Select_printer",         "s"  },
    { "Get_photo_list",         "s"  },
    { "Start_print_job",        "D"  },
    { "Cancel_job",             ""   },
    { "Get_photo_preview",      "sii" },   /* §18 */
    { "Get_photo_info",         "s"    },   /* §18 */
    { "Preview_print",          "E"    },   /* §18 */
};

/* 追加单个参数(按类型字符) */
static int append_arg(DBusMessageIter *it, char t, const char *val)
{
    if (t == 's') {
        dbus_message_iter_append_basic(it, DBUS_TYPE_STRING, &val);
    } else if (t == 'i') {
        char *end = NULL;
        long n = strtol(val, &end, 10);
        if (end == val || (end && *end)) { fprintf(stderr, "printerctl: '%s' 不是整数\n", val); return -1; }
        dbus_int32_t v = (dbus_int32_t)n;
        dbus_message_iter_append_basic(it, DBUS_TYPE_INT32, &v);
    } else if (t == 'b') {
        dbus_bool_t v = (!strcmp(val, "true") || !strcmp(val, "1")) ? TRUE : FALSE;
        dbus_message_iter_append_basic(it, DBUS_TYPE_BOOLEAN, &v);
    }
    return 0;
}

/* k=v 选项 → a{sv} 字典条目。类型按键名定(INTERFACE.md v2.1 §3/§3b):
 *   crop_x/crop_y/crop_w/crop_h → double 'd'（"0.5" 这类小数）
 *   copies/rotation             → int32 'i'
 *   其余(media/print-quality/…) → string 's'
 * 返回 0=ok, -1=值无法按目标类型解析。 */
static int append_opt_kv(DBusMessageIter *dict, const char *key, const char *val)
{
    /* 先解析值(失败时消息未动), 再开容器封送 */
    double dv = 0.0;
    dbus_int32_t iv = 0;
    int is_double = !strncmp(key, "crop_", 5);
    int is_int = !strcmp(key, "copies") || !strcmp(key, "rotation");
    if (is_double) {
        char *end = NULL;
        dv = strtod(val, &end);
        if (end == val || (end && *end)) {
            fprintf(stderr, "printerctl: '%s=%s' 不是小数\n", key, val);
            return -1;
        }
    } else if (is_int) {
        char *end = NULL;
        long n = strtol(val, &end, 10);
        if (end == val || (end && *end)) {
            fprintf(stderr, "printerctl: '%s=%s' 不是整数\n", key, val);
            return -1;
        }
        iv = (dbus_int32_t)n;
    }
    DBusMessageIter entry, v;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    if (is_double) {
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
                                         DBUS_TYPE_DOUBLE_AS_STRING, &v);
        dbus_message_iter_append_basic(&v, DBUS_TYPE_DOUBLE, &dv);
    } else if (is_int) {
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
                                         DBUS_TYPE_INT32_AS_STRING, &v);
        dbus_message_iter_append_basic(&v, DBUS_TYPE_INT32, &iv);
    } else {
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
                                         DBUS_TYPE_STRING_AS_STRING, &v);
        dbus_message_iter_append_basic(&v, DBUS_TYPE_STRING, &val);
    }
    dbus_message_iter_close_container(&entry, &v);
    dbus_message_iter_close_container(dict, &entry);
    return 0;
}

/* args[from..n) 的 k=v 对追加进已打开的 dict 容器 */
static int append_opt_pairs(DBusMessageIter *dict, char **args, int from, int n)
{
    for (int i = from; i < n; i++) {
        char *eq = strchr(args[i], '=');
        if (!eq) { fprintf(stderr, "printerctl: 忽略无 '=' 的选项 '%s'\n", args[i]); continue; }
        *eq = 0;
        if (append_opt_kv(dict, args[i], eq + 1) != 0) return -1;
    }
    return 0;
}

/* Start_print_job: 裸参数→as, 末尾 k=v→a{sv} */
static int append_print_job_args(DBusMessageIter *it, char **args, int n)
{
    DBusMessageIter arr, dict;
    dbus_message_iter_open_container(it, DBUS_TYPE_ARRAY, "s", &arr);
    int npaths = 0;
    for (int i = 0; i < n && !strchr(args[i], '='); i++) {
        dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &args[i]);
        npaths++;
    }
    dbus_message_iter_close_container(it, &arr);
    if (npaths == 0) { fprintf(stderr, "printerctl: Start_print_job 至少一个路径\n"); return -1; }

    dbus_message_iter_open_container(it, DBUS_TYPE_ARRAY, "{sv}", &dict);
    int rc = append_opt_pairs(&dict, args, npaths, n);
    dbus_message_iter_close_container(it, &dict);
    return rc;
}

/* §18 Preview_print: s path + 末尾 k=v→a{sv} */
static int append_preview_args(DBusMessageIter *it, char **args, int n)
{
    if (n < 1 || strchr(args[0], '=')) {
        fprintf(stderr, "printerctl: Preview_print 第一个参数是照片路径\n");
        return -1;
    }
    dbus_message_iter_append_basic(it, DBUS_TYPE_STRING, &args[0]);
    DBusMessageIter dict;
    dbus_message_iter_open_container(it, DBUS_TYPE_ARRAY, "{sv}", &dict);
    int rc = append_opt_pairs(&dict, args, 1, n);
    dbus_message_iter_close_container(it, &dict);
    return rc;
}

static int cmd_call(char **args, int n)
{
    if (n < 1) { usage(); return 2; }
    DBusMessage *req = dbus_message_new_method_call(PRN_DEST, PRN_PATH, PRN_IFACE, args[0]);
    if (!req) { fprintf(stderr, "oom\n"); return 1; }

    const char *sig = NULL;
    for (size_t i = 0; i < sizeof method_sigs / sizeof method_sigs[0]; i++)
        if (!strcmp(method_sigs[i].member, args[0])) { sig = method_sigs[i].sig; break; }

    DBusMessageIter it;
    dbus_message_iter_init_append(req, &it);
    if (sig) {
        if (sig[0] == 'D') {                       /* Start_print_job */
            if (append_print_job_args(&it, args + 1, n - 1) != 0) { dbus_message_unref(req); return 2; }
        } else if (sig[0] == 'E') {                /* §18 Preview_print */
            if (append_preview_args(&it, args + 1, n - 1) != 0) { dbus_message_unref(req); return 2; }
        } else {
            size_t len = strlen(sig);
            if ((int)len != n - 1) {
                fprintf(stderr, "printerctl: %s 需要 %zu 个参数\n", args[0], len);
                dbus_message_unref(req);
                return 2;
            }
            for (size_t i = 0; i < len; i++)
                if (append_arg(&it, sig[i], args[1 + i]) != 0) { dbus_message_unref(req); return 2; }
        }
    } else {
        fprintf(stderr, "printerctl: 未知方法 '%s'(允许的方法见 INTERFACE.md §3)\n", args[0]);
        dbus_message_unref(req);
        return 2;
    }
    int rc = call_and_print(g_conn, req, 1);
    dbus_message_unref(req);
    return rc;
}

/* ---------- caps: 人可读打印全部能力(选中打印机; 探测后可见) ---------- */
static int get_prop_str(const char *prop, const char **out)
{
    DBusError e;
    dbus_error_init(&e);
    DBusMessage *req = new_props_call("Get");
    const char *iface = PRN_IFACE;
    dbus_message_append_args(req, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop,
                             DBUS_TYPE_INVALID);
    DBusMessage *rep = dbus_connection_send_with_reply_and_block(g_conn, req, 15000, &e);
    dbus_message_unref(req);
    if (!rep) { dbus_error_free(&e); return -1; }
    if (dbus_message_get_type(rep) == DBUS_MESSAGE_TYPE_ERROR) {
        dbus_message_unref(rep);
        return -1;
    }
    DBusMessageIter it, v;
    const char *s = NULL;
    dbus_message_iter_init(rep, &it);
    dbus_message_iter_recurse(&it, &v);
    if (dbus_message_iter_get_arg_type(&v) == DBUS_TYPE_STRING)
        dbus_message_iter_get_basic(&v, &s);
    *out = s ? strdup(s) : strdup("");
    dbus_message_unref(rep);
    return 0;
}

static int get_prop_as(const char *prop, char ***out_items, int *out_n)
{
    DBusError e;
    dbus_error_init(&e);
    DBusMessage *req = new_props_call("Get");
    const char *iface = PRN_IFACE;
    dbus_message_append_args(req, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop,
                             DBUS_TYPE_INVALID);
    DBusMessage *rep = dbus_connection_send_with_reply_and_block(g_conn, req, 15000, &e);
    dbus_message_unref(req);
    if (!rep) { dbus_error_free(&e); return -1; }
    if (dbus_message_get_type(rep) == DBUS_MESSAGE_TYPE_ERROR) {
        dbus_message_unref(rep);
        return -1;
    }
    DBusMessageIter it, v, arr;
    dbus_message_iter_init(rep, &it);
    dbus_message_iter_recurse(&it, &v);
    dbus_message_iter_recurse(&v, &arr);               /* arr = as 数组迭代器 */
    int n = 0;
    DBusMessageIter cnt = arr;
    while (dbus_message_iter_get_arg_type(&cnt) == DBUS_TYPE_STRING) { n++; dbus_message_iter_next(&cnt); }
    char **items = calloc((size_t)n + 1, sizeof(char *));
    int i = 0;
    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING && i < n) {
        const char *s = NULL;
        dbus_message_iter_get_basic(&arr, &s);
        items[i++] = strdup(s ? s : "");
        dbus_message_iter_next(&arr);
    }
    dbus_message_unref(rep);
    *out_items = items;
    *out_n = n;
    return 0;
}

static void print_section(const char *title, char **items, int n)
{
    printf("%s:\n", title);
    if (n == 0) { printf("  (空)\n"); return; }
    for (int i = 0; i < n; i++) printf("  [%d] %s\n", i, items[i]);
}

static int cmd_caps(void)
{
    /* caps_ready(b) 经 Get 拿不到串 → 读 document_format 判断选中 */
    char *name = NULL, *ip = NULL, *docfmt = NULL, *media = NULL, *quality = NULL;
    char **media_l = NULL, **qual_l = NULL, **color_l = NULL, **fmt_l = NULL, **size_l = NULL;
    int nm = 0, nq = 0, nc = 0, nf = 0, ns = 0;
    const char *v;
    if (get_prop_str("printer_name", &v) == 0) name = (char *)v;
    if (get_prop_str("printer_ip", &v) == 0) ip = (char *)v;
    if (get_prop_str("document_format", &v) == 0) docfmt = (char *)v;
    if (get_prop_str("media", &v) == 0) media = (char *)v;
    if (get_prop_str("print_quality", &v) == 0) quality = (char *)v;
    if (!name || !name[0]) {
        printf("caps: 未选中打印机(先 Start_discovery + Select_printer)\n");
        free(name); free(ip); free(docfmt); free(media); free(quality);
        return 1;
    }
    printf("== 打印机能力: %s @ %s ==\n", name, ip ? ip : "?");
    printf("document_format(实际用值): %s\n", docfmt && docfmt[0] ? docfmt : "(未协商)");
    printf("当前生效: media=%s  print_quality=%s\n",
           media && media[0] ? media : "(默认)", quality && quality[0] ? quality : "(默认)");
    get_prop_as("supported_media", &media_l, &nm);
    get_prop_as("supported_qualities", &qual_l, &nq);
    get_prop_as("supported_color_modes", &color_l, &nc);
    get_prop_as("supported_formats", &fmt_l, &nf);
    get_prop_as("media_size_info", &size_l, &ns);
    print_section("supported_media(可下发 media 关键字)", media_l, nm);
    print_section("media_size_info(关键字=物理尺寸, 尺寸×分辨率=目标像素)", size_l, ns);
    print_section("supported_qualities", qual_l, nq);
    print_section("supported_color_modes", color_l, nc);
    print_section("supported_formats", fmt_l, nf);
    /* copies 上限单读 */
    DBusError e;
    dbus_error_init(&e);
    DBusMessage *req = new_props_call("Get");
    const char *iface = PRN_IFACE, *pmax = "copies_max";
    dbus_message_append_args(req, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &pmax,
                             DBUS_TYPE_INVALID);
    DBusMessage *rep = dbus_connection_send_with_reply_and_block(g_conn, req, 15000, &e);
    dbus_message_unref(req);
    if (rep && dbus_message_get_type(rep) != DBUS_MESSAGE_TYPE_ERROR) {
        DBusMessageIter it, v2;
        dbus_int32_t max = 0;
        dbus_message_iter_init(rep, &it);
        dbus_message_iter_recurse(&it, &v2);
        if (dbus_message_iter_get_arg_type(&v2) == DBUS_TYPE_INT32)
            dbus_message_iter_get_basic(&v2, &max);
        printf("copies: 1..%d\n", (int)max);
    }
    if (rep) dbus_message_unref(rep);

    for (int i = 0; i < nm; i++) free(media_l[i]);
    for (int i = 0; i < nq; i++) free(qual_l[i]);
    for (int i = 0; i < nc; i++) free(color_l[i]);
    for (int i = 0; i < nf; i++) free(fmt_l[i]);
    for (int i = 0; i < ns; i++) free(size_l[i]);
    free(media_l); free(qual_l); free(color_l); free(fmt_l); free(size_l);
    free(name); free(ip); free(docfmt); free(media); free(quality);
    return 0;
}

/* ---------- monitor ---------- */
static DBusHandlerResult mon_filter(DBusConnection *c, DBusMessage *m, void *data)
{
    (void)c; (void)data;
    if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_SIGNAL) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    const char *iface = dbus_message_get_interface(m);
    const char *member = dbus_message_get_member(m);
    if (!iface || !member) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    /* 只看本服务(sender 名由总线填) */
    const char *sender = dbus_message_get_sender(m);

    if (!strcmp(iface, IFACE_PROPERTIES) && !strcmp(member, "propertiesChanged")) {
        DBusMessageIter it;
        printf("[propertiesChanged] sender=%s\n", sender ? sender : "?");
        if (dbus_message_iter_init(m, &it)) {
            DBusMessageIter arr;
            dbus_message_iter_next(&it);                     /* 跳过 arg0 接口名 */
            dbus_message_iter_recurse(&it, &arr);
            while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY) {
                DBusMessageIter entry, v;
                const char *key = NULL;
                dbus_message_iter_recurse(&arr, &entry);
                dbus_message_iter_get_basic(&entry, &key);
                dbus_message_iter_next(&entry);
                dbus_message_iter_recurse(&entry, &v);
                printf("  %s = ", key);
                print_value(&v, 0);
                printf("\n");
                dbus_message_iter_next(&arr);
            }
            dbus_message_iter_next(&it);                     /* arg2: as(恒空) */
            DBusMessageIter inv;
            dbus_message_iter_recurse(&it, &inv);
            printf("  invalidated: [");
            int first = 1;
            while (dbus_message_iter_get_arg_type(&inv) == DBUS_TYPE_STRING) {
                const char *s = NULL;
                dbus_message_iter_get_basic(&inv, &s);
                printf("%s%s", first ? "" : ", ", s);
                dbus_message_iter_next(&inv);
                first = 0;
            }
            printf("]\n");
        }
        fflush(stdout);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    /* 自定义信号(阶段1无, 预留) */
    if (!strcmp(iface, PRN_IFACE)) {
        printf("[signal] %s sender=%s\n", member, sender ? sender : "?");
        DBusMessageIter it;
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
    dbus_bus_add_match(g_conn, "type='signal',sender='" PRN_DEST "'", &e);
    if (dbus_error_is_set(&e)) {
        fprintf(stderr, "printerctl: add_match 失败: %s\n", e.message);
        dbus_error_free(&e);
        return 1;
    }
    if (!dbus_connection_add_filter(g_conn, mon_filter, NULL, NULL)) {
        fprintf(stderr, "printerctl: add_filter 失败\n");
        return 1;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sig;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    printf("monitoring com.hasselblad.printer signals (Ctrl-C 退出)\n");
    while (g_mon_run && dbus_connection_read_write_dispatch(g_conn, 100))
        ;
    printf("\nmonitor 结束\n");
    return 0;
}

/* ---------- syswifi on|off: com.hasselblad.system.power_on_wifi(b) ----------
 * 相机合法 Wi-Fi 电源(诊断/service.sh verify 用)。与业务表无关的独立通路;
 * 恰一个 bool 实参(QtDBus 槽签名严格匹配, odindb wrapper 的 timeout 尾参不能带)。 */
static int cmd_syswifi(const char *onoff)
{
    dbus_bool_t on;
    if (!strcmp(onoff, "on") || !strcmp(onoff, "true") || !strcmp(onoff, "1")) on = TRUE;
    else if (!strcmp(onoff, "off") || !strcmp(onoff, "false") || !strcmp(onoff, "0")) on = FALSE;
    else { fprintf(stderr, "printerctl: syswifi 参数: on|off\n"); return 2; }

    DBusMessage *req = dbus_message_new_method_call(
        "com.hasselblad.system", "/system", "com.hasselblad.system", "power_on_wifi");
    if (!req) { fprintf(stderr, "oom\n"); return 1; }
    if (!dbus_message_append_args(req, DBUS_TYPE_BOOLEAN, &on, DBUS_TYPE_INVALID)) {
        dbus_message_unref(req);
        return 1;
    }
    DBusError e;
    dbus_error_init(&e);
    DBusMessage *rep = dbus_connection_send_with_reply_and_block(g_conn, req, 8000, &e);
    dbus_message_unref(req);
    if (!rep) {
        fprintf(stderr, "printerctl: power_on_wifi(%s) 调用失败: %s: %s\n",
                on ? "true" : "false", e.name ? e.name : "?", e.message ? e.message : "");
        dbus_error_free(&e);
        return 1;
    }
    if (dbus_message_get_type(rep) == DBUS_MESSAGE_TYPE_ERROR) {
        fprintf(stderr, "printerctl: 错误: %s\n",
                dbus_message_get_error_name(rep) ? dbus_message_get_error_name(rep) : "?");
        dbus_message_unref(rep);
        return 1;
    }
    dbus_message_unref(rep);
    printf("power_on_wifi(%s) ok\n", on ? "true" : "false");
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

    if (!strcmp(cmd, "--get-all") || !strcmp(cmd, "get-all")) {
        g_conn = connect_bus(bus_addr);
        return cmd_get_all();
    }
    if (!strcmp(cmd, "get")) {
        if (nrest != 1) { usage(); return 2; }
        g_conn = connect_bus(bus_addr);
        return cmd_get(rest[0]);
    }
    if (!strcmp(cmd, "set")) {
        if (nrest != 2) { usage(); return 2; }
        g_conn = connect_bus(bus_addr);
        return cmd_set(rest[0], rest[1]);
    }
    if (!strcmp(cmd, "call")) {
        g_conn = connect_bus(bus_addr);
        return cmd_call(rest, nrest);
    }
    if (!strcmp(cmd, "caps")) {
        g_conn = connect_bus(bus_addr);
        return cmd_caps();
    }
    if (!strcmp(cmd, "monitor")) {
        g_conn = connect_bus(bus_addr);
        return cmd_monitor();
    }
    if (!strcmp(cmd, "syswifi")) {                     /* 相机合法 Wi-Fi 电源(verify/诊断) */
        if (nrest != 1) { fprintf(stderr, "用法: syswifi on|off\n"); return 2; }
        g_conn = connect_bus(bus_addr);
        return cmd_syswifi(rest[0]);
    }
    usage();
    return 2;
}
