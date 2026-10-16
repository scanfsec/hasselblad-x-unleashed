/* argspec.h — CLI 参数 → D-Bus 类型编解码
 * 支持: i/s/b/d/x(64位)/y/u/n/q/t、ay(逗号分隔 hex 字节)、as(JSON 数组)、
 *       a{sv}(JSON 对象)、v(自动)、a{i,u,x,d}(逗号数值, 通用扩展)
 * 位集枚举: "E_A|E_B" 语法 → i32 按位 OR; 数值分量直接 OR,
 *           E_ 名分量因通用版未内置 HblmTypes 表 → 明确报错(README 差异清单)
 */
#ifndef ODIN_ARGSPEC_H
#define ODIN_ARGSPEC_H

#include <dbus/dbus.h>
#include <stddef.h>

/* "E_A|E_B|1|2" → int32; 返回 0/-1(err 填原因) */
int argspec_parse_bitset(const char *s, dbus_int32_t *out, char *err, size_t errsz);

/* 按签名把一个 CLI 字符串编码为一个完整 D-Bus 值并追加到 iter; 0/-1 */
int argspec_encode(DBusMessageIter *iter, const char *sig, const char *s,
                   char *err, size_t errsz);

/* 无签名时的启发式推断, 返回静态签名字符串 */
const char *argspec_guess(const char *s);

/* 值文本 → dumpfmt 之外的机器可读检测(JSON 容器) */
int argspec_is_json_object(const char *s);
int argspec_is_json_array(const char *s);

#endif
