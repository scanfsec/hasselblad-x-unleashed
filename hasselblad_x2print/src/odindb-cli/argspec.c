/* argspec.c — CLI 参数 → D-Bus 类型编解码实现 */
#include "argspec.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void seterr(char *err, size_t errsz, const char *fmt, const char *a)
{
    if (err && errsz) snprintf(err, errsz, fmt, a);
}

static void seterr_n(char *err, size_t errsz, const char *msg, long long n)
{
    if (err && errsz) snprintf(err, errsz, "%s (%lld)", msg, n);
}

/* ---------------- 位集枚举 ---------------- */
int argspec_parse_bitset(const char *s, dbus_int32_t *out, char *err, size_t errsz)
{
    dbus_int32_t v = 0;
    char buf[256];
    snprintf(buf, sizeof buf, "%s", s);
    char *save = NULL;
    for (char *tok = strtok_r(buf, "|", &save); tok; tok = strtok_r(NULL, "|", &save)) {
        if (isdigit((unsigned char)tok[0]) ||
            (tok[0] == '-' && isdigit((unsigned char)tok[1]))) {
            char *end = NULL;
            long x = strtol(tok, &end, 0);
            if (!end || *end) {
                seterr(err, errsz, "bad bitset component: %s", tok);
                return -1;
            }
            v |= (dbus_int32_t)x;
        } else {
            /* E_Xxx 名分量: 需设备 HblmTypes 枚举表, 通用版未内置 */
            seterr(err, errsz,
                   "enum name '%s' cannot be resolved (generic client has no "
                   "HblmTypes tables; use numeric values, e.g. E_A|E_B -> 1|2)", tok);
            return -1;
        }
    }
    *out = v;
    return 0;
}

/* ---------------- 基本类型 ---------------- */
static int enc_basic(DBusMessageIter *it, char t, const char *s,
                     char *err, size_t errsz)
{
    switch (t) {
    case DBUS_TYPE_BOOLEAN: {
        dbus_bool_t v;
        if (!strcasecmp(s, "true") || !strcmp(s, "1")) v = TRUE;
        else if (!strcasecmp(s, "false") || !strcmp(s, "0")) v = FALSE;
        else { seterr(err, errsz, "expected true/false, got '%s'", s); return -1; }
        return dbus_message_iter_append_basic(it, t, &v) ? 0 : -1;
    }
    case DBUS_TYPE_BYTE: {
        char *end = NULL;
        unsigned long v = strtoul(s, &end, 0);
        if (!end || (*end && !isspace((unsigned char)*end)) || v > 255) {
            seterr_n(err, errsz, "expected byte 0..255", (long long)v);
            return -1;
        }
        unsigned char b = (unsigned char)v;
        return dbus_message_iter_append_basic(it, t, &b) ? 0 : -1;
    }
    case DBUS_TYPE_INT16:
    case DBUS_TYPE_INT32: {
        dbus_int32_t v;
        if (strchr(s, '|') || (s[0] == 'E' && s[1] == '_')) {
            if (argspec_parse_bitset(s, &v, err, errsz)) return -1; /* 位集枚举 → i32 */
        } else {
            char *end = NULL;
            long long x = strtoll(s, &end, 0);
            if (!end || (*end && !isspace((unsigned char)*end))) {
                seterr(err, errsz, "expected int, got '%s'", s);
                return -1;
            }
            v = (dbus_int32_t)x;
        }
        if (t == DBUS_TYPE_INT16) {
            dbus_int16_t w = (dbus_int16_t)v;
            return dbus_message_iter_append_basic(it, t, &w) ? 0 : -1;
        }
        return dbus_message_iter_append_basic(it, t, &v) ? 0 : -1;
    }
    case DBUS_TYPE_UINT16:
    case DBUS_TYPE_UINT32: {
        char *end = NULL;
        unsigned long long v = strtoull(s, &end, 0);
        if (!end || (*end && !isspace((unsigned char)*end))) {
            seterr(err, errsz, "expected uint, got '%s'", s);
            return -1;
        }
        if (t == DBUS_TYPE_UINT16) {
            dbus_uint16_t w = (dbus_uint16_t)v;
            return dbus_message_iter_append_basic(it, t, &w) ? 0 : -1;
        }
        dbus_uint32_t u = (dbus_uint32_t)v;
        return dbus_message_iter_append_basic(it, t, &u) ? 0 : -1;
    }
    case DBUS_TYPE_INT64:
    case DBUS_TYPE_UINT64: {
        char *end = NULL;
        if (t == DBUS_TYPE_INT64) {
            long long v = strtoll(s, &end, 0);
            if (!end || (*end && !isspace((unsigned char)*end))) {
                seterr(err, errsz, "expected int64, got '%s'", s);
                return -1;
            }
            dbus_int64_t q = (dbus_int64_t)v;
            return dbus_message_iter_append_basic(it, t, &q) ? 0 : -1;
        }
        unsigned long long v = strtoull(s, &end, 0);
        if (!end || (*end && !isspace((unsigned char)*end))) {
            seterr(err, errsz, "expected uint64, got '%s'", s);
            return -1;
        }
        dbus_uint64_t q = (dbus_uint64_t)v;
        return dbus_message_iter_append_basic(it, t, &q) ? 0 : -1;
    }
    case DBUS_TYPE_DOUBLE: {
        char *end = NULL;
        double v = strtod(s, &end);
        if (!end || (*end && !isspace((unsigned char)*end))) {
            seterr(err, errsz, "expected double, got '%s'", s);
            return -1;
        }
        return dbus_message_iter_append_basic(it, t, &v) ? 0 : -1;
    }
    case DBUS_TYPE_STRING:
    case DBUS_TYPE_OBJECT_PATH:
    case DBUS_TYPE_SIGNATURE: {
        const char *v = s;
        return dbus_message_iter_append_basic(it, t, &v) ? 0 : -1;
    }
    default: {
        char tstr[2] = { t, 0 };
        seterr(err, errsz, "unsupported basic type '%s'", tstr);
        return -1;
    }
    }
}

/* ---------------- JSON/列表小工具 ---------------- */
static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

/* 去掉首尾成对引号 */
static void unquote(char *s)
{
    size_t n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        s[n - 1] = 0;
        memmove(s, s + 1, n - 1);
    }
}

int argspec_is_json_array(const char *s)
{
    return s[0] == '[';
}

int argspec_is_json_object(const char *s)
{
    return s[0] == '{';
}

/* 在 s 内按顶层逗号切分(跳过引号串), 写 n 个起止偏移; 返回段数 */
static int split_top(const char *s, size_t off[64][2], int max)
{
    int n = 0, depth = 0, inq = 0;
    size_t start = 0;
    for (size_t i = 0; s[i]; i++) {
        char c = s[i];
        if (inq) { if (c == '"' && s[i - 1] != '\\') inq = 0; continue; }
        if (c == '"') inq = 1;
        else if (c == '[' || c == '{') depth++;
        else if (c == ']' || c == '}') depth--;
        else if (c == ',' && depth == 0) {
            if (n < max) { off[n][0] = start; off[n][1] = i; n++; }
            start = i + 1;
        }
    }
    if (n < max) { off[n][0] = start; off[n][1] = strlen(s); n++; }
    return n;
}

/* ay: 逗号/空白分隔的十六进制字节 */
static int enc_ay(DBusMessageIter *it, const char *s, char *err, size_t errsz)
{
    DBusMessageIter arr;
    if (!dbus_message_iter_open_container(it, DBUS_TYPE_ARRAY,
                                          DBUS_TYPE_BYTE_AS_STRING, &arr))
        return -1;
    if (!s[0]) goto done;             /* 空串 → 空数组 */
    char buf[8192];
    snprintf(buf, sizeof buf, "%s", s);
    for (char *p = strtok(buf, ", \t"); p; p = strtok(NULL, ", \t")) {
        char *end = NULL;
        unsigned long v = strtoul(p, &end, 16);
        if (!end || *end || v > 255) {
            seterr(err, errsz, "bad hex byte: %s", p);
            dbus_message_iter_abandon_container(it, &arr);
            return -1;
        }
        unsigned char b = (unsigned char)v;
        dbus_message_iter_append_basic(&arr, DBUS_TYPE_BYTE, &b);
    }
done:
    return dbus_message_iter_close_container(it, &arr) ? 0 : -1;
}

/* as: ["a","b"] JSON 风格(原版 usage 示例) */
static int enc_as(DBusMessageIter *it, const char *s, char *err, size_t errsz)
{
    DBusMessageIter arr;
    if (!dbus_message_iter_open_container(it, DBUS_TYPE_ARRAY,
                                          DBUS_TYPE_STRING_AS_STRING, &arr))
        return -1;
    if (s[0] != '[') {
        seterr(err, errsz, "expected JSON array like [\"a\",\"b\"], got '%s'", s);
        dbus_message_iter_abandon_container(it, &arr);
        return -1;
    }
    char buf[8192];
    size_t n = strlen(s);
    if (n >= sizeof buf) n = sizeof buf - 1;
    memcpy(buf, s, n);
    buf[n] = 0;
    memmove(buf, buf + 1, n);         /* 去 '[' */
    char *rb = buf + strlen(buf);
    while (rb > buf && (rb[-1] == ']' || isspace((unsigned char)rb[-1]))) *--rb = 0;
    if (*buf) {
        size_t off[64][2];
        int cnt = split_top(buf, off, 64);
        for (int i = 0; i < cnt; i++) {
            char piece[512];
            size_t len = off[i][1] - off[i][0];
            if (len >= sizeof piece) len = sizeof piece - 1;
            memcpy(piece, buf + off[i][0], len);
            piece[len] = 0;
            char *t = trim(piece);
            unquote(t);
            dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &t);
        }
    }
    return dbus_message_iter_close_container(it, &arr) ? 0 : -1;
}

/* a{sv}: {"k":"v","n":42} JSON 风格(原版 usage 示例): 值 s/b/i/d */
static int enc_asv(DBusMessageIter *it, const char *s, char *err, size_t errsz)
{
    DBusMessageIter arr;
    if (!dbus_message_iter_open_container(it, DBUS_TYPE_ARRAY, "{sv}", &arr))
        return -1;
    if (s[0] != '{') {
        seterr(err, errsz, "expected JSON object like {\"k\":\"v\"}, got '%s'", s);
        dbus_message_iter_abandon_container(it, &arr);
        return -1;
    }
    char buf[8192];
    size_t n = strlen(s);
    if (n >= sizeof buf) n = sizeof buf - 1;
    memcpy(buf, s, n);
    buf[n] = 0;
    memmove(buf, buf + 1, n);
    char *rb = buf + strlen(buf);
    while (rb > buf && (rb[-1] == '}' || isspace((unsigned char)rb[-1]))) *--rb = 0;
    if (*buf) {
        size_t off[64][2];
        int cnt = split_top(buf, off, 64);
        for (int i = 0; i < cnt; i++) {
            char piece[512];
            size_t len = off[i][1] - off[i][0];
            if (len >= sizeof piece) len = sizeof piece - 1;
            memcpy(piece, buf + off[i][0], len);
            piece[len] = 0;
            char *colon = strchr(piece, ':');
            if (!colon) {
                seterr(err, errsz, "bad JSON entry: %s", piece);
                dbus_message_iter_abandon_container(it, &arr);
                return -1;
            }
            *colon = 0;
            char *k = trim(piece);
            unquote(k);
            char *v = trim(colon + 1);
            DBusMessageIter entry, var;
            dbus_message_iter_open_container(&arr, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
            dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &k);
            if (v[0] == '"') {
                unquote(v);
                dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
                                                 DBUS_TYPE_STRING_AS_STRING, &var);
                dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &v);
            } else if (!strcasecmp(v, "true") || !strcasecmp(v, "false")) {
                dbus_bool_t b = !strcasecmp(v, "true");
                dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
                                                 DBUS_TYPE_BOOLEAN_AS_STRING, &var);
                dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &b);
            } else if (strchr(v, '.')) {
                double d = strtod(v, NULL);
                dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
                                                 DBUS_TYPE_DOUBLE_AS_STRING, &var);
                dbus_message_iter_append_basic(&var, DBUS_TYPE_DOUBLE, &d);
            } else {
                dbus_int32_t x = (dbus_int32_t)strtol(v, NULL, 0);
                dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
                                                 DBUS_TYPE_INT32_AS_STRING, &var);
                dbus_message_iter_append_basic(&var, DBUS_TYPE_INT32, &x);
            }
            dbus_message_iter_close_container(&entry, &var);
            dbus_message_iter_close_container(&arr, &entry);
        }
    }
    return dbus_message_iter_close_container(it, &arr) ? 0 : -1;
}

/* a{i,u,x,d,n,q,t,y}: 逗号分隔数值(通用扩展) */
static int enc_anum(DBusMessageIter *it, char elem, const char *s,
                    char *err, size_t errsz)
{
    DBusMessageIter arr;
    char esig[2] = { elem, 0 };
    if (!dbus_message_iter_open_container(it, DBUS_TYPE_ARRAY, esig, &arr))
        return -1;
    if (!s[0]) goto done;
    char buf[8192];
    snprintf(buf, sizeof buf, "%s", s);
    for (char *p = strtok(buf, ","); p; p = strtok(NULL, ",")) {
        char *t = trim(p);
        if (enc_basic(&arr, elem, t, err, errsz)) {
            dbus_message_iter_abandon_container(it, &arr);
            return -1;
        }
    }
done:
    return dbus_message_iter_close_container(it, &arr) ? 0 : -1;
}

/* v: 按文本形态自动判型 */
static int enc_variant(DBusMessageIter *it, const char *s, char *err, size_t errsz);

int argspec_encode(DBusMessageIter *it, const char *sig, const char *s,
                   char *err, size_t errsz)
{
    if (!sig[0]) { seterr(err, errsz, "empty signature for '%s'", s); return -1; }
    if (sig[0] == 'a') {
        if (sig[1] == '{' && sig[2] == 's' && sig[3] == 'v' && sig[4] == '}')
            return enc_asv(it, s, err, errsz);
        if (sig[1] == DBUS_TYPE_BYTE) return enc_ay(it, s, err, errsz);
        if (sig[1] == DBUS_TYPE_STRING) return enc_as(it, s, err, errsz);
        if (strchr("inouxtdq", sig[1]) && sig[2] == '\0')
            return enc_anum(it, sig[1], s, err, errsz);
        seterr(err, errsz, "unsupported container signature '%s'", sig);
        return -1;
    }
    if (sig[0] == DBUS_TYPE_VARIANT) return enc_variant(it, s, err, errsz);
    return enc_basic(it, sig[0], s, err, errsz);
}

static int enc_variant(DBusMessageIter *it, const char *s, char *err, size_t errsz)
{
    const char *gs = argspec_guess(s);
    DBusMessageIter var;
    if (!dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, gs, &var))
        return -1;
    if (argspec_encode(&var, gs, s, err, errsz)) {
        dbus_message_iter_abandon_container(it, &var);
        return -1;
    }
    return dbus_message_iter_close_container(it, &var) ? 0 : -1;
}

/* ---------------- 启发式类型推断(无自省签名时兜底) ---------------- */
static int all_digits(const char *s)
{
    if (*s == '-' || *s == '+') s++;
    if (!*s) return 0;
    while (*s) {
        if (!isdigit((unsigned char)*s)) return 0;
        s++;
    }
    return 1;
}

static int looks_hexlist(const char *s)
{
    int n = 0;
    const char *p = s;
    while (*p) {
        if (!isxdigit((unsigned char)*p)) return 0;
        p++;
        n++;
        if (*p == ',') p++;
        else if (*p) return 0;
    }
    return n > 0;
}

const char *argspec_guess(const char *s)
{
    if (!strcasecmp(s, "true") || !strcasecmp(s, "false")) return "b";
    if (s[0] == 'E' && s[1] == '_') return "i";        /* 位集/枚举 → i32 */
    if (strchr(s, '|')) return "i";
    if (argspec_is_json_array(s)) return "as";
    if (argspec_is_json_object(s)) return "a{sv}";
    if (looks_hexlist(s) && strchr(s, ',')) return "ay";
    if (all_digits(s)) return "i";
    {
        char *end = NULL;
        strtod(s, &end);
        if (end && *end == 0 && end != s) return "d";
    }
    return "s";
}
