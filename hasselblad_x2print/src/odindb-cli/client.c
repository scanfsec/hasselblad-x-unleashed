/* client.c — libdbus 客户端封装实现
 * 报文级对齐原版 QtDBus 可见序列(qemu+dbus-monitor 实测, 见 conformance 文档):
 *   AddMatch propertiesChanged → AddMatch NameOwnerChanged → GetNameOwner
 *   → AddMatch NameOwnerChanged(重复, QtDBus 怪癖) → NameHasOwner
 *   → GetAll(com.hasselblad.<mod>) → GetAll(com.hasselblad.linkstatus)
 */
#include "client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

int odin_error_exitcode(const char *name)
{
    if (!name) return 1;
    if (!strcmp(name, "com.hasselblad.error.failed"))         return 1;
    if (!strcmp(name, "com.hasselblad.error.parametererror")) return 2;
    if (!strcmp(name, "com.hasselblad.error.busy"))           return 3;
    if (!strcmp(name, "com.hasselblad.error.canceled"))       return 4;
    if (!strcmp(name, "com.hasselblad.error.timeout"))        return 255;
    return 1;                         /* 其余 dbus 错误统一非 0 */
}

/* ---------------- 模型查找 ---------------- */
static int name_eq_ci(const char *a, const char *b)
{
    return strcasecmp(a, b) == 0;
}

const odin_method *odin_iface_find_method(const odin_iface *ifc, const char *name)
{
    for (int i = 0; i < ifc->n_methods; i++)
        if (!strcmp(ifc->methods[i].name, name)) return &ifc->methods[i];
    /* member 去 do 前缀(odindb C 层 doXxx → wire Xxx) + 大小写不敏感兜底 */
    if (name[0] == 'd' && name[1] == 'o')
        for (int i = 0; i < ifc->n_methods; i++)
            if (!strcmp(ifc->methods[i].name, name + 2)) return &ifc->methods[i];
    for (int i = 0; i < ifc->n_methods; i++)
        if (name_eq_ci(ifc->methods[i].name, name)) return &ifc->methods[i];
    return NULL;
}

const odin_prop *odin_iface_find_prop(const odin_iface *ifc, const char *name)
{
    for (int i = 0; i < ifc->n_props; i++)
        if (!strcmp(ifc->props[i].name, name)) return &ifc->props[i];
    for (int i = 0; i < ifc->n_props; i++)
        if (name_eq_ci(ifc->props[i].name, name)) return &ifc->props[i];
    return NULL;
}

const odin_signal *odin_iface_find_signal(const odin_iface *ifc, const char *name)
{
    for (int i = 0; i < ifc->n_signals; i++)
        if (!strcmp(ifc->signals[i].name, name)) return &ifc->signals[i];
    for (int i = 0; i < ifc->n_signals; i++)
        if (name_eq_ci(ifc->signals[i].name, name)) return &ifc->signals[i];
    return NULL;
}

/* ---------------- 总线连接 ---------------- */
static DBusConnection *bus_connect(const char *bus, DBusError *e)
{
    dbus_error_init(e);
    dbus_connection_set_change_sigpipe(FALSE);
    if (bus && !strcmp(bus, "session"))
        return dbus_bus_get(DBUS_BUS_SESSION, e);
    if (bus && !strcmp(bus, "none"))
        return NULL;                  /* 不连: 仅本地操作(--list-enums 等) */

    /* system(默认): libdbus 默认路径(含 DBUS_SYSTEM_BUS_ADDRESS),
     * 失败再依次试 HBL 自定义 DBUS_SOCKET 与相机 /dev/socket/dbus */
    DBusConnection *c = dbus_bus_get(DBUS_BUS_SYSTEM, e);
    if (c) return c;
    const char *cand = getenv("DBUS_SOCKET");
    for (int i = 0; i < 2; i++, cand = "/dev/socket/dbus") {
        if (!cand) continue;
        char addr[300];
        snprintf(addr, sizeof addr, "unix:path=%s", cand);
        dbus_error_free(e);
        c = dbus_connection_open_private(addr, e);
        if (!c) continue;
        if (!dbus_bus_register(c, e)) {
            dbus_connection_close(c);
            dbus_connection_unref(c);
            c = NULL;
            continue;
        }
        return c;
    }
    return NULL;
}

int odin_client_init(odin_client *oc, const char *module, const char *bus, DBusError *e)
{
    memset(oc, 0, sizeof *oc);
    const char *m = module;
    if (!strncmp(m, "com.hasselblad.", 15)) m += 15;   /* 容忍全名输入 */
    snprintf(oc->module, sizeof oc->module, "%s", m);
    snprintf(oc->dest, sizeof oc->dest, "com.hasselblad.%s", m);
    snprintf(oc->path, sizeof oc->path, "/%s", m);
    oc->conn = bus_connect(bus, e);
    if (!oc->conn && !(bus && !strcmp(bus, "none")))
        return -1;
    return 0;
}

void odin_client_free(odin_client *oc)
{
    if (oc->conn) {
        dbus_connection_flush(oc->conn);
        dbus_connection_unref(oc->conn);
        oc->conn = NULL;
    }
}

/* ---------------- 通用调用 ---------------- */
DBusMessage *odin_call(odin_client *oc, const char *dest, const char *path,
                       const char *iface, const char *member, int timeout_ms,
                       DBusError *e, DBusMessage *req)
{
    (void)dest; (void)path; (void)iface; (void)member; /* 身份已封进 req */
    if (!req) return NULL;
    DBusMessage *rep = dbus_connection_send_with_reply_and_block(
        oc->conn, req, timeout_ms > 0 ? timeout_ms : ODIN_DEFAULT_TIMEOUT_MS, e);
    dbus_message_unref(req);
    return rep;
}

DBusMessage *odin_getall(odin_client *oc, const char *iface, DBusError *e)
{
    DBusMessage *m = dbus_message_new_method_call(
        oc->dest, oc->path, ODIN_IFACE_PROPERTIES, "GetAll");
    const char *s = iface;
    dbus_message_append_args(m, DBUS_TYPE_STRING, &s, DBUS_TYPE_INVALID);
    return odin_call(oc, oc->dest, oc->path, ODIN_IFACE_PROPERTIES, "GetAll", 0, e, m);
}

/* 单属性 Get(ss)→v: 原版生成代码从不调用(实测零 xref), 此处仅在
 * GetAll 不可用时兜底探测 —— 与报告 Q2 结论一致 */
DBusMessage *odin_get(odin_client *oc, const char *prop, DBusError *e)
{
    DBusMessage *m = dbus_message_new_method_call(
        oc->dest, oc->path, ODIN_IFACE_PROPERTIES, "Get");
    const char *i = oc->dest, *n = prop;
    dbus_message_append_args(m, DBUS_TYPE_STRING, &i, DBUS_TYPE_STRING, &n,
                             DBUS_TYPE_INVALID);
    return odin_call(oc, oc->dest, oc->path, ODIN_IFACE_PROPERTIES, "Get", 0, e, m);
}

/* ---------------- Set 构造器: ssv, 值封 variant ---------------- */
int odin_set_begin(odin_client *oc, const char *prop, const char *value_sig,
                   odin_set_builder *b)
{
    b->msg = dbus_message_new_method_call(
        oc->dest, oc->path, ODIN_IFACE_PROPERTIES, "Set");
    if (!b->msg) return -1;
    const char *i = oc->dest, *n = prop;
    dbus_message_append_args(b->msg, DBUS_TYPE_STRING, &i, DBUS_TYPE_STRING, &n,
                             DBUS_TYPE_INVALID);
    dbus_message_iter_init_append(b->msg, &b->parent);
    if (!dbus_message_iter_open_container(&b->parent, DBUS_TYPE_VARIANT,
                                          value_sig, &b->var)) {
        dbus_message_unref(b->msg);
        b->msg = NULL;
        return -1;
    }
    return 0;
}

DBusMessage *odin_set_end(odin_client *oc, odin_set_builder *b, DBusError *e)
{
    dbus_message_iter_close_container(&b->parent, &b->var);
    DBusMessage *rep = dbus_connection_send_with_reply_and_block(
        oc->conn, b->msg, ODIN_DEFAULT_TIMEOUT_MS, e);
    dbus_message_unref(b->msg);
    b->msg = NULL;
    return rep;
}

DBusMessage *odin_introspect(odin_client *oc, DBusError *e)
{
    DBusMessage *m = dbus_message_new_method_call(
        oc->dest, oc->path, ODIN_IFACE_INTROSPECT, "Introspect");
    return odin_call(oc, oc->dest, oc->path, ODIN_IFACE_INTROSPECT, "Introspect", 0, e, m);
}

/* ---------------- 总线查询 / match rule ---------------- */
int odin_name_has_owner(odin_client *oc, const char *name)
{
    DBusError e;
    dbus_error_init(&e);
    DBusMessage *m = dbus_message_new_method_call(
        ODIN_BUS_DAEMON, ODIN_BUS_PATH, ODIN_BUS_DAEMON, "NameHasOwner");
    const char *n = name;
    dbus_message_append_args(m, DBUS_TYPE_STRING, &n, DBUS_TYPE_INVALID);
    DBusMessage *rep = dbus_connection_send_with_reply_and_block(
        oc->conn, m, 5000, &e);
    dbus_message_unref(m);
    if (!rep) {
        dbus_error_free(&e);
        return 0;
    }
    dbus_bool_t has = FALSE;
    if (dbus_message_get_type(rep) == DBUS_MESSAGE_TYPE_METHOD_RETURN)
        dbus_message_get_args(rep, &e, DBUS_TYPE_BOOLEAN, &has, DBUS_TYPE_INVALID);
    dbus_error_free(&e);
    dbus_message_unref(rep);
    return has != FALSE;
}

int odin_add_match(odin_client *oc, const char *rule)
{
    DBusError e;
    dbus_error_init(&e);
    dbus_bus_add_match(oc->conn, rule, &e);   /* 失败静默: 与原版启动序列容错一致 */
    dbus_error_free(&e);
    return 0;
}

int odin_remove_match(odin_client *oc, const char *rule)
{
    DBusError e;
    dbus_error_init(&e);
    dbus_bus_remove_match(oc->conn, rule, &e);
    dbus_error_free(&e);
    return 0;
}

/* ---------------- 启动序列 ---------------- */
static void add_match_props_changed(odin_client *oc)
{
    char rule[512];
    snprintf(rule, sizeof rule,
             "type='signal',sender='%s',path='%s',interface='%s',member='%s'",
             oc->dest, oc->path, ODIN_IFACE_PROPERTIES, ODIN_SIG_PROPCHANGED);
    odin_add_match(oc, rule);
}

static void add_match_owner_changed(odin_client *oc)
{
    char rule[512];
    snprintf(rule, sizeof rule,
             "type='signal',sender='org.freedesktop.DBus',"
             "interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='%s'",
             oc->dest);
    odin_add_match(oc, rule);
}

int odin_client_start(odin_client *oc, DBusMessage **getall_reply, DBusError *e)
{
    if (!oc->conn) return -2;
    if (getall_reply) *getall_reply = NULL;

    add_match_props_changed(oc);      /* DBusProxy 基类订阅 */
    add_match_owner_changed(oc);      /* QDBusServiceWatcher 等效 */

    /* GetNameOwner: QtDBus 连接服务时的探询, 结果忽略(下一个调用兜底) */
    DBusError ig;
    dbus_error_init(&ig);
    DBusMessage *m = dbus_message_new_method_call(
        ODIN_BUS_DAEMON, ODIN_BUS_PATH, ODIN_BUS_DAEMON, "GetNameOwner");
    const char *n = oc->dest;
    dbus_message_append_args(m, DBUS_TYPE_STRING, &n, DBUS_TYPE_INVALID);
    DBusMessage *rep = dbus_connection_send_with_reply_and_block(oc->conn, m, 5000, &ig);
    dbus_message_unref(m);
    if (rep) dbus_message_unref(rep);
    dbus_error_free(&ig);

    add_match_owner_changed(oc);      /* 重复 AddMatch: QtDBus 实测怪癖, 报文级对齐 */

    if (!odin_name_has_owner(oc, oc->dest))
        return -1;                    /* 名字不在总线上 → 上层按 No such service 处理 */

    /* 本模块属性缓存填充 */
    rep = odin_getall(oc, oc->dest, e);
    if (!rep) return -2;
    if (dbus_message_get_type(rep) == DBUS_MESSAGE_TYPE_ERROR) {
        if (getall_reply) *getall_reply = rep;
        else dbus_message_unref(rep);
        return -2;                    /* 总线层错误(非"名字不存在"路径) */
    }
    if (getall_reply) *getall_reply = rep;
    else dbus_message_unref(rep);

    /* LinkstatusProxy 基类照例拉取(内容忽略, 仅报文对齐) */
    rep = odin_getall(oc, ODIN_IFACE_LINKSTATUS, &ig);
    if (rep) dbus_message_unref(rep);
    dbus_error_free(&ig);
    return 0;
}

/* ---------------- ListNames ---------------- */
int odin_list_modules(odin_client *oc, char ***names, DBusError *e)
{
    *names = NULL;
    DBusMessage *m = dbus_message_new_method_call(
        ODIN_BUS_DAEMON, ODIN_BUS_PATH, ODIN_BUS_DAEMON, "ListNames");
    DBusMessage *rep = dbus_connection_send_with_reply_and_block(
        oc->conn, m, 5000, e);
    dbus_message_unref(m);
    if (!rep) return -1;
    char **strs = NULL;
    int n = 0;
    if (!dbus_message_get_args(rep, e, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING,
                               &strs, &n, DBUS_TYPE_INVALID)) {
        dbus_message_unref(rep);
        return -1;
    }
    const char *pfx = "com.hasselblad.";
    int cnt = 0;
    char **out = calloc((size_t)(n > 0 ? n : 1), sizeof *out);
    for (int i = 0; i < n; i++)
        if (strs[i] && !strncmp(strs[i], pfx, 15) && strs[i][15])
            out[cnt++] = strdup(strs[i] + 15);
    dbus_free_string_array(strs);
    dbus_message_unref(rep);
    *names = out;
    return cnt;
}

/* ---------------- 自省 XML 解析(轻量字符串扫描) ---------------- */
/* 从 tag 文本中取 key="value"; 成功返回 value 起点并写结束引号位置 */
static const char *attr_value(const char *tag, const char *key, const char **end)
{
    size_t kl = strlen(key);
    const char *p = tag;
    while ((p = strstr(p, key)) != NULL) {
        const char *q = p + kl;
        if (q[0] == '=' && q[1] == '"') {
            *end = strchr(q + 2, '"');
            if (*end) return q + 2;
        }
        p += kl;
    }
    return NULL;
}

static int copy_attr(char *dst, size_t dstsz, const char *tag, const char *key)
{
    const char *end;
    const char *v = attr_value(tag, key, &end);
    if (!v) { dst[0] = 0; return -1; }
    size_t len = (size_t)(end - v);
    if (len >= dstsz) len = dstsz - 1;
    memcpy(dst, v, len);
    dst[len] = 0;
    return 0;
}

/* 处理 <arg .../>: 按方向归入 in/out */
static void parse_arg_tags(const char *block, odin_arg *in, int *n_in,
                           odin_arg *out, int *n_out)
{
    const char *p = block;
    while ((p = strstr(p, "<arg")) != NULL) {
        const char *close = strchr(p, '>');
        if (!close) break;
        char tag[256];
        size_t len = (size_t)(close - p);
        if (len >= sizeof tag) len = sizeof tag - 1;
        memcpy(tag, p, len + 1);
        tag[len] = 0;

        char dir[12] = "in";
        copy_attr(dir, sizeof dir, tag, "direction");
        odin_arg a;
        copy_attr(a.name, sizeof a.name, tag, "name");
        copy_attr(a.type, sizeof a.type, tag, "type");

        int is_out = !strcmp(dir, "out");
        odin_arg *dst = is_out ? out : in;
        int *cnt = is_out ? n_out : n_in;
        int cap = is_out ? 8 : 8;
        if (*cnt < cap) dst[(*cnt)++] = a;
        p = close + 1;
    }
}

int odin_introspect_parse(odin_client *oc, odin_iface *ifc, DBusError *e)
{
    memset(ifc, 0, sizeof *ifc);
    DBusMessage *rep = odin_introspect(oc, e);
    if (!rep) return -1;
    char *xml = NULL;
    int rc = -1;
    if (dbus_message_get_type(rep) == DBUS_MESSAGE_TYPE_ERROR ||
        !dbus_message_get_args(rep, e, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID))
        goto out;

    /* 逐 <interface name="X"> 块扫描, 只保留本模块接口 */
    const char *p = xml;
    while ((p = strstr(p, "<interface")) != NULL) {
        const char *tagend = strchr(p, '>');
        if (!tagend) break;
        char tag[160];
        size_t tl = (size_t)(tagend - p);
        if (tl >= sizeof tag) tl = sizeof tag - 1;
        memcpy(tag, p, tl + 1);
        tag[tl] = 0;
        char iname[96];
        if (copy_attr(iname, sizeof iname, tag, "name") == 0 &&
            !strcmp(iname, oc->dest)) {
            const char *close = strstr(tagend, "</interface>");
            const char *nextiface = strstr(tagend, "<interface");
            if (!close || (nextiface && nextiface < close)) { p = tagend + 1; continue; }
            size_t blen = (size_t)(close - tagend);
            char *block = malloc(blen + 1);
            if (!block) goto out;
            memcpy(block, tagend, blen + 1);

            const char *q = block;
            while ((q = strstr(q, "<method")) != NULL) {
                const char *me = strstr(q, "</method>");
                if (!me) break;
                const char *mt = strchr(q, '>');
                if (!mt || mt > me) { q += 7; continue; }
                char mtag[160];
                size_t ml = (size_t)(mt - q);
                if (ml >= sizeof mtag) ml = sizeof mtag - 1;
                memcpy(mtag, q, ml + 1);
                mtag[ml] = 0;
                if (ifc->n_methods < 48) {
                    odin_method *mm = &ifc->methods[ifc->n_methods++];
                    copy_attr(mm->name, sizeof mm->name, mtag, "name");
                    char *mb = malloc((size_t)(me - mt) + 1);
                    memcpy(mb, mt + 1, (size_t)(me - mt - 1) + 1);
                    mb[me - mt - 1] = 0;
                    parse_arg_tags(mb, mm->in, &mm->n_in, mm->out, &mm->n_out);
                    free(mb);
                }
                q = me + 9;
            }
            q = block;
            while ((q = strstr(q, "<property")) != NULL) {
                const char *pt = strchr(q, '>');
                if (!pt) break;
                char ptag[200];
                size_t pl = (size_t)(pt - q);
                if (pl >= sizeof ptag) pl = sizeof ptag - 1;
                memcpy(ptag, q, pl + 1);
                ptag[pl] = 0;
                if (ifc->n_props < 64) {
                    odin_prop *pp = &ifc->props[ifc->n_props++];
                    copy_attr(pp->name, sizeof pp->name, ptag, "name");
                    copy_attr(pp->type, sizeof pp->type, ptag, "type");
                    char acc[20] = "";
                    copy_attr(acc, sizeof acc, ptag, "access");
                    pp->writable = strstr(acc, "write") != NULL;
                }
                q = pt + 1;
            }
            q = block;
            while ((q = strstr(q, "<signal")) != NULL) {
                const char *se = strstr(q, "</signal>");
                if (!se) break;
                const char *st = strchr(q, '>');
                if (!st || st > se) { q += 7; continue; }
                char stag[160];
                size_t sl = (size_t)(st - q);
                if (sl >= sizeof stag) sl = sizeof stag - 1;
                memcpy(stag, q, sl + 1);
                stag[sl] = 0;
                if (ifc->n_signals < 48) {
                    odin_signal *ss = &ifc->signals[ifc->n_signals++];
                    copy_attr(ss->name, sizeof ss->name, stag, "name");
                    char *sb = malloc((size_t)(se - st) + 1);
                    memcpy(sb, st + 1, (size_t)(se - st - 1) + 1);
                    sb[se - st - 1] = 0;
                    int dummy = 0;
                    parse_arg_tags(sb, ss->args, &ss->n_args, ss->args, &dummy);
                    free(sb);
                }
                q = se + 10;
            }
            free(block);
            rc = 0;
            break;                    /* 本模块接口处理完 */
        }
        p = tagend + 1;
    }
out:
    dbus_message_unref(rep);
    return rc;
}
