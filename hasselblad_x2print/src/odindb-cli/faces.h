/* faces.h — 原版 odindb-send 槽位面(生成文件, 勿手改)
 * 由 tools/gen_odindb_faces.py 从 qemu 实测原版输出生成:
 *   16 原生模块(原版原件) + printer(补丁副本, usbhost 槽位面)
 * 用途: 已知模块的 --get-all 子集与默认值 / 列表输出 / 方法签名 /
 *       信号过滤 一比一复刻; --any 走自省通用路径(见 README)
 */
#ifndef ODIN_FACES_H
#define ODIN_FACES_H

typedef struct {
    const char *name;    /* 属性名(可重复, 原版基类同名怪癖) */
    const char *block;   /* --get-all 默认输出字面块(整块含 "name = ..." 行) */
} face_prop;

typedef struct {
    const char *name;
    const char *sig;     /* in 参数 D-Bus 签名(枚举=i32) */
    const char *argnames;/* 逗号分隔 in 参数名(编码用) */
    int         n_out;
} face_method;

typedef struct {
    const char *name;
    int         wire;    /* 原版 --signal all 会加 match rule 的线上信号 */
    const char *argnames;
} face_signal;

typedef struct {
    const char *mod;
    const char *pl, *pli, *ml, *mli, *lsig;   /* 列表显示原文(逐字) */
    const face_prop   *props;   int n_props;
    const face_method *methods; int n_methods;
    const face_signal *signals; int n_signals;
} odin_face;

const odin_face *odin_face_find(const char *mod);
int odin_face_known(const char *mod);
#endif
