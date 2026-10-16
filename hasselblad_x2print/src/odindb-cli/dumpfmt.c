/* dumpfmt.c — QVariant 风格输出格式化实现 */
#include "dumpfmt.h"

#include <string.h>

/* ay → 小写 hex 连写(原版 "583250ab" 实测); 其余数组 [a, b] */
static void dump_array(FILE *out, DBusMessageIter *v)
{
    DBusMessageIter sub;
    dbus_message_iter_recurse(v, &sub);
    int elem = dbus_message_iter_get_arg_type(&sub);

    if (dbus_message_iter_get_element_type(v) == DBUS_TYPE_BYTE) {
        do {
            unsigned char b;
            dbus_message_iter_get_basic(&sub, &b);
            fprintf(out, "%02x", b);
        } while (dbus_message_iter_next(&sub));
        return;
    }
    if (elem == DBUS_TYPE_DICT_ENTRY) {   /* a{sv} → Item {k = v, ...}(空 → Item {}) */
        fprintf(out, "Item {");
        int first = 1;
        do {
            DBusMessageIter ent;
            dbus_message_iter_recurse(&sub, &ent);
            const char *key = NULL;
            dbus_message_iter_get_basic(&ent, &key);
            dbus_message_iter_next(&ent);
            fprintf(out, "%s%s = ", first ? "" : ", ", key ? key : "?");
            dumpfmt_value(out, &ent);
            first = 0;
        } while (dbus_message_iter_next(&sub));
        fprintf(out, "}");
        return;
    }
    fprintf(out, "[");
    int first = 1;
    if (elem != DBUS_TYPE_INVALID) {
        do {
            fprintf(out, "%s", first ? "" : ", ");
            dumpfmt_value(out, &sub);
            first = 0;
        } while (dbus_message_iter_next(&sub));
    }
    fprintf(out, "]");
}

void dumpfmt_value(FILE *out, DBusMessageIter *v)
{
    int t = dbus_message_iter_get_arg_type(v);
    switch (t) {
    case DBUS_TYPE_BOOLEAN: {
        dbus_bool_t b;
        dbus_message_iter_get_basic(v, &b);
        fprintf(out, b ? "true" : "false");
        return;
    }
    case DBUS_TYPE_BYTE: {
        unsigned char y;
        dbus_message_iter_get_basic(v, &y);
        fprintf(out, "%u", (unsigned)y);
        return;
    }
    case DBUS_TYPE_INT16: {
        dbus_int16_t n;
        dbus_message_iter_get_basic(v, &n);
        fprintf(out, "%d", (int)n);
        return;
    }
    case DBUS_TYPE_UINT16: {
        dbus_uint16_t q;
        dbus_message_iter_get_basic(v, &q);
        fprintf(out, "%u", (unsigned)q);
        return;
    }
    case DBUS_TYPE_INT32: {
        dbus_int32_t i;
        dbus_message_iter_get_basic(v, &i);
        fprintf(out, "%d", (int)i);
        return;
    }
    case DBUS_TYPE_UINT32: {
        dbus_uint32_t u;
        dbus_message_iter_get_basic(v, &u);
        fprintf(out, "%u", (unsigned)u);
        return;
    }
    case DBUS_TYPE_INT64: {
        dbus_int64_t x;
        dbus_message_iter_get_basic(v, &x);
        fprintf(out, "%lld", (long long)x);
        return;
    }
    case DBUS_TYPE_UINT64: {
        dbus_uint64_t t64;
        dbus_message_iter_get_basic(v, &t64);
        fprintf(out, "%llu", (unsigned long long)t64);
        return;
    }
    case DBUS_TYPE_DOUBLE: {
        double d;
        dbus_message_iter_get_basic(v, &d);
        fprintf(out, "%g", d);
        return;
    }
    case DBUS_TYPE_STRING:
    case DBUS_TYPE_OBJECT_PATH:
    case DBUS_TYPE_SIGNATURE: {
        const char *s = NULL;
        dbus_message_iter_get_basic(v, &s);
        if (!s || !*s) fprintf(out, "<empty>");   /* 原版空串实测 */
        else fputs(s, out);
        return;
    }
    case DBUS_TYPE_VARIANT: {
        DBusMessageIter sub;
        dbus_message_iter_recurse(v, &sub);
        dumpfmt_value(out, &sub);
        return;
    }
    case DBUS_TYPE_ARRAY:
        dump_array(out, v);
        return;
    case DBUS_TYPE_STRUCT: {
        DBusMessageIter sub;
        dbus_message_iter_recurse(v, &sub);
        fprintf(out, "(");
        int first = 1;
        do {
            fprintf(out, "%s", first ? "" : ", ");
            dumpfmt_value(out, &sub);
            first = 0;
        } while (dbus_message_iter_next(&sub));
        fprintf(out, ")");
        return;
    }
    default:
        fprintf(out, "?");
        return;
    }
}

void dumpfmt_prop_line(FILE *out, const char *name, DBusMessageIter *v)
{
    fprintf(out, "%s = ", name);
    dumpfmt_value(out, v);
    fputc('\n', out);
}

void dumpfmt_signal_frame(FILE *out, const char *member,
                          const char *const *argnames, int n_names,
                          DBusMessage *args)
{
    fprintf(out, "%s:\n", member);
    DBusMessageIter sub;
    int i = 0;
    for (int more = dbus_message_iter_init(args, &sub); more;
         more = dbus_message_iter_next(&sub), i++) {
        fputs("     ", out);                       /* 原版 5 空格缩进 */
        if (i < n_names && argnames[i] && argnames[i][0])
            fprintf(out, "%s = ", argnames[i]);
        dumpfmt_value(out, &sub);
        fputc('\n', out);
    }
    fputc('\n', out);                              /* 帧后空行 */
}

/* D-Bus 签名 → Qt 类型名(原版列表输出风格) */
const char *dumpfmt_qttype(const char *sig)
{
    if (!sig || !sig[0]) return "?";
    if (sig[0] != 'a') {
        switch (sig[0]) {
        case 'b': return "bool";
        case 'y': return "uchar";
        case 'n': return "short";
        case 'q': return "ushort";
        case 'i': return "int";
        case 'u': return "uint";
        case 'x': return "qlonglong";
        case 't': return "qulonglong";
        case 'd': return "double";
        case 's': return "QString";
        case 'o': return "QDBusObjectPath";
        case 'g': return "QDBusSignature";
        case 'v': return "QVariant";
        default:  return sig;
        }
    }
    if (!strcmp(sig, "ay")) return "QByteArray";
    if (!strcmp(sig, "as")) return "QStringList";
    if (!strcmp(sig, "a{sv}")) return "QVariantMap";
    if (!strcmp(sig, "aa{sv}")) return "QList<QVariantMap>";
    if (!strcmp(sig, "au")) return "QList<uint>";
    if (!strcmp(sig, "ai")) return "QList<int>";
    return sig;                       /* 其余容器原样展示签名 */
}
