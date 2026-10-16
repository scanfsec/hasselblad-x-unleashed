/* client.h — libdbus 客户端封装(线协议见 analysis/dbus/REPORT_odindb_conventions.md)
 * 模板化三元组: destination=interface=com.hasselblad.<mod>, path=/<mod>
 */
#ifndef ODIN_CLIENT_H
#define ODIN_CLIENT_H

#include <dbus/dbus.h>
#include <stddef.h>

#define ODIN_IFACE_PROPERTIES "org.freedesktop.DBus.Properties"
#define ODIN_IFACE_INTROSPECT "org.freedesktop.DBus.Introspectable"
#define ODIN_BUS_DAEMON       "org.freedesktop.DBus"
#define ODIN_BUS_PATH         "/org/freedesktop/DBus"
/* 属性变更信号 member —— 首字母小写 p, 原版 QtDBus 逐字如此, 发标准拼写会收不到 */
#define ODIN_SIG_PROPCHANGED  "propertiesChanged"
/* LinkstatusProxy 基类在连接时总是附加拉取的接口(报文级对齐用) */
#define ODIN_IFACE_LINKSTATUS "com.hasselblad.linkstatus"

#define ODIN_DEFAULT_TIMEOUT_MS 25000   /* 原版: timeout default 25 seconds */

/* com.hasselblad.error.* → E_ReturnStatus 数值(即进程退出码) */
int odin_error_exitcode(const char *error_name);

/* ---- 自省解析模型(仅本模块接口) ---- */
typedef struct {
    char name[64];
    char type[12];
} odin_arg;

typedef struct {
    char     name[64];
    odin_arg in[8];
    int      n_in;
    odin_arg out[8];
    int      n_out;
} odin_method;

typedef struct {
    char name[64];
    char type[12];
    int  writable;                    /* introspection access 含 write */
} odin_prop;

typedef struct {
    char     name[64];
    odin_arg args[8];
    int      n_args;
} odin_signal;

typedef struct {
    odin_method methods[48];
    int         n_methods;
    odin_prop   props[64];
    int         n_props;
    odin_signal signals[48];
    int         n_signals;
} odin_iface;

/* 模型里按名查找, 未找到返回 NULL */
const odin_method *odin_iface_find_method(const odin_iface *ifc, const char *name);
const odin_prop   *odin_iface_find_prop(const odin_iface *ifc, const char *name);
const odin_signal *odin_iface_find_signal(const odin_iface *ifc, const char *name);

typedef struct {
    DBusConnection *conn;
    char module[48];                  /* 裸模块名 */
    char dest[96];                    /* com.hasselblad.<mod> */
    char path[64];                    /* /<mod> */
} odin_client;

/* 连总线(bus: "system"/"session"/NULL=system; "none" 不连) + 模板化三元组 */
int  odin_client_init(odin_client *oc, const char *module, const char *bus, DBusError *e);
void odin_client_free(odin_client *oc);

/* QtDBus 等效启动序列(AddMatch propertiesChanged / NameOwnerChanged ×2、
 * GetNameOwner、NameHasOwner、GetAll(本模块)、GetAll(linkstatus))。
 * 返回 0=名字在总线上; -1=不在; -2=总线错误(e 已填)。
 * *getall_reply(可为 NULL)返回本模块 GetAll 应答(借用, 调用方 unref)。 */
int odin_client_start(odin_client *oc, DBusMessage **getall_reply, DBusError *e);

/* 通用同步调用(25s 默认), 成功返回回复(调用方 unref), 失败 NULL 且 e/errno 填原因 */
DBusMessage *odin_call(odin_client *oc, const char *dest, const char *path,
                       const char *iface, const char *member, int timeout_ms,
                       DBusError *e, DBusMessage *req);

DBusMessage *odin_getall(odin_client *oc, const char *iface, DBusError *e);
DBusMessage *odin_get(odin_client *oc, const char *prop, DBusError *e);
DBusMessage *odin_introspect(odin_client *oc, DBusError *e);

/* Set(ssv) 构造器: begin 打开 variant 容器(调用方往 var 里填值), end 封口并发送 */
typedef struct {
    DBusMessage    *msg;
    DBusMessageIter parent;
    DBusMessageIter var;
} odin_set_builder;

int odin_set_begin(odin_client *oc, const char *prop, const char *value_sig,
                   odin_set_builder *b);
DBusMessage *odin_set_end(odin_client *oc, odin_set_builder *b, DBusError *e);

/* 总线名查询 / match rule 订阅(失败静默, 与原版启动序列容错一致) */
int  odin_name_has_owner(odin_client *oc, const char *name);
int  odin_add_match(odin_client *oc, const char *rule);
int  odin_remove_match(odin_client *oc, const char *rule);

/* 拉取 + 解析自省 XML 到 ifc(只保留本模块接口), 0=成功 */
int odin_introspect_parse(odin_client *oc, odin_iface *ifc, DBusError *e);

/* ListNames 过滤 com.hasselblad.* 后写入 names(每项 malloc, 调用方 free), 返回个数/-1 */
int odin_list_modules(odin_client *oc, char ***names, DBusError *e);

#endif
