/* netlog.h — x2print 网络层(IPP/mDNS)极简日志钩子
 *
 * 设计：库代码统一走 LOGD/LOGI/LOGW/LOGE 宏；默认输出到 stderr。
 * 后续 printer-service(D-Bus 守护进程)可通过 x2p_netlog_set_hook()
 * 注册钩子，把日志转投 syslog/journal/自身的日志框架。
 *
 * 注意：非线程安全（当前服务为单线程事件循环，足够）。
 */
#ifndef X2P_NETLOG_H
#define X2P_NETLOG_H

#include <stdarg.h>

typedef enum {
    X2P_LOG_DEBUG = 0, /* 协议报文细节 */
    X2P_LOG_INFO  = 1, /* 正常流程节点 */
    X2P_LOG_WARN  = 2, /* 可恢复异常（如 mDNS 组播加入失败退回单播） */
    X2P_LOG_ERROR = 3, /* 操作失败 */
} x2p_log_level_t;

/* 日志钩子原型：service 层实现后经 x2p_netlog_set_hook() 注册；返回后无需再输出 */
typedef void (*x2p_log_fn)(x2p_log_level_t lvl, const char *fmt, va_list ap);

void x2p_netlog_set_hook(x2p_log_fn fn);       /* 传 NULL 恢复默认 stderr 输出 */
void x2p_netlog_set_level(x2p_log_level_t lvl); /* 设置最低输出级别（默认 INFO） */
void x2p_netlog(x2p_log_level_t lvl, const char *fmt, ...);

#define LOGD(...) x2p_netlog(X2P_LOG_DEBUG, __VA_ARGS__)
#define LOGI(...) x2p_netlog(X2P_LOG_INFO,  __VA_ARGS__)
#define LOGW(...) x2p_netlog(X2P_LOG_WARN,  __VA_ARGS__)
#define LOGE(...) x2p_netlog(X2P_LOG_ERROR, __VA_ARGS__)

#endif /* X2P_NETLOG_H */
