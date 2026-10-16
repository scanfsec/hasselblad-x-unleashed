/* dumpfmt.h — QVariant 风格输出格式化(对齐原版实测输出)
 * true/false; 字符串裸打(空串 <empty>); 列表 [a, b]; 字节数组小写 hex 连写;
 * dict 打印 Item {k = v, ...}; 枚举位 i32 → 原版带枚举名(需表, 通用版仅数值)
 */
#ifndef ODIN_DUMPFMT_H
#define ODIN_DUMPFMT_H

#include <dbus/dbus.h>
#include <stdio.h>

/* 打印 iter 所指的一个值(variant 已 recurse 过) */
void dumpfmt_value(FILE *out, DBusMessageIter *v);

/* "name = value" 一行(-p 读 / --get-all / 信号参数) */
void dumpfmt_prop_line(FILE *out, const char *name, DBusMessageIter *v);

/* 信号帧: "member:" + 每参数 "     name = value" + 空行(原版 5 空格缩进) */
void dumpfmt_signal_frame(FILE *out, const char *member,
                          const char *const *argnames, int n_names,
                          DBusMessage *args);

/* D-Bus 签名 → Qt 类型名(int/QByteArray/QStringList...) */
const char *dumpfmt_qttype(const char *sig);

#endif
