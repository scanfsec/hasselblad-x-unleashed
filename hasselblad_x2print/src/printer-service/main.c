/* main.c — printer-service 守护进程入口
 * 流程: 解析参数 → 日志/配置初始化 → 连总线(占名 com.hasselblad.printer)
 *       → 注册 adaptor + backend → 事件循环(read_write_dispatch + tick)
 *       → SIGTERM/SIGINT 干净退出(存配置)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <dbus/dbus.h>

#include "log.h"
#include "state.h"
#include "config.h"
#include "adaptor.h"
#include "backend.h"

#define PRN_NAME     "com.hasselblad.printer"
#define DEFAULT_CFG  "/data/x2print/config.ini"
#define DEFAULT_LOG  "/data/x2print/log/service.log"

static volatile sig_atomic_t g_run = 1;

static void on_signal(int sig)
{
    (void)sig;
    g_run = 0;                       /* 事件循环 100ms 超时, 很快察觉 */
}

static void usage(void)
{
    fputs(
        "printer-service — X2D II 打印服务 (com.hasselblad.printer)\n"
        "用法: printer-service [选项]\n"
        "  --config <path>   配置文件 (默认 " DEFAULT_CFG ")\n"
        "  --mock            强制 mock 后端\n"
        "  --real            强制真后端(无真后端符号时回落 mock 并告警)\n"
        "  --bus <address>   D-Bus 总线地址 (默认: 环境/系统总线)\n"
        "  --log <file>      日志文件 (默认 " DEFAULT_LOG ")\n"
        "  --loglevel <lvl>  debug|info|warn|error (默认 info)\n"
        "  --daemon          fork 到后台\n"
        "  --version         版本\n"
        "  --help            本帮助\n",
        stderr);
}

static void install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);        /* 对端断开不杀进程 */
}

/* 属性变更通知: state → adaptor 信号; 可写属性同时落盘 */
static const char *g_cfg_path;

static void on_prop_changed(int idx, void *user)
{
    (void)user;
    prn_adaptor_emit_changed(idx);
    if (prn_prop_info_get(idx)->writable)
        prn_config_save(g_cfg_path);
}

int main(int argc, char **argv)
{
    const char *cfg_path = DEFAULT_CFG, *bus_addr = NULL, *log_path = DEFAULT_LOG;
    int level = PRN_LOG_INFO, mock = 0, daemonize = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc)      cfg_path = argv[++i];
        else if (!strcmp(argv[i], "--mock"))                   mock = 1;
        else if (!strcmp(argv[i], "--real"))                   mock = 0;   /* 显式真后端 */
        else if (!strcmp(argv[i], "--bus") && i + 1 < argc)    bus_addr = argv[++i];
        else if (!strcmp(argv[i], "--log") && i + 1 < argc)    log_path = argv[++i];
        else if (!strcmp(argv[i], "--loglevel") && i + 1 < argc) {
            level = prn_log_level_parse(argv[++i]);
            if (level < 0) { fprintf(stderr, "bad --loglevel\n"); return 2; }
        }
        else if (!strcmp(argv[i], "--daemon"))                 daemonize = 1;
        else if (!strcmp(argv[i], "--version")) { puts("printer-service 1.1 (stage2)"); return 0; }
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
        else { fprintf(stderr, "未知参数: %s (试试 --help)\n", argv[i]); return 2; }
    }

    prn_log_init(log_path, level);
    LOGI("printer-service starting (pid=%d, mock=%d, bus=%s)", (int)getpid(), mock,
         bus_addr ? bus_addr : "<env/default>");

    if (daemonize) {
        pid_t pid = fork();
        if (pid < 0) { LOGE("fork: %s", strerror(errno)); return 1; }
        if (pid > 0) return 0;                 /* 父进程退出 */
        setsid();
        if (fork() != 0) _exit(0);             /* 二次 fork 脱离会话 */
        close(0);
        /* stderr 保留(日志), stdin 关闭 */
    }

    install_signals();

    /* 后端选择: --mock > 弱符号真后端 > mock 回落 */
    const prn_backend *be = NULL;
    if (!mock && prn_backend_real) {
        be = prn_backend_real();
        LOGI("using real backend");
    }
    if (!be) {
        if (!mock) LOGW("real backend not present, falling back to mock (--mock to silence)");
        be = prn_backend_mock();
    }

    /* 配置(首启写默认; 此后可写属性一变即落盘) */
    g_cfg_path = cfg_path;
    if (prn_config_load(cfg_path) != 0)
        LOGW("config load failed, continue with defaults (%s)", cfg_path);

    /* ---- 连总线 ---- */
    DBusError e;
    dbus_error_init(&e);
    dbus_connection_set_change_sigpipe(FALSE);
    DBusConnection *conn;
    int is_private = 0;
    if (bus_addr) {
        /* 显式地址(相机: unix:path=/dev/socket/dbus) → 私有连接 + 手动注册 */
        conn = dbus_connection_open_private(bus_addr, &e);
        is_private = 1;
    } else {
        /* 走 libdbus 默认: DBUS_SESSION_BUS_ADDRESS / DBUS_SYSTEM_BUS_ADDRESS / 系统总线 */
        conn = dbus_bus_get(DBUS_BUS_SYSTEM, &e);
    }
    if (!conn) {
        LOGE("bus connect failed: %s", e.message ? e.message : "?");
        return 1;
    }
    if (is_private && !dbus_bus_register(conn, &e)) {
        LOGE("bus register failed: %s", e.message ? e.message : "?");
        return 1;
    }

    /* 占名: DO_NOT_QUEUE —— 失败立即退出(不排队等接管) */
    int ret = dbus_bus_request_name(conn, PRN_NAME, DBUS_NAME_FLAG_DO_NOT_QUEUE, &e);
    if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        LOGE("request_name %s failed ret=%d: %s", PRN_NAME, ret,
             e.message ? e.message : "?");
        return 1;
    }
    LOGI("bus name %s acquired", PRN_NAME);

    /* 属性通知挂接 + adaptor 注册 */
    prn_state_set_notify(on_prop_changed, NULL);
    if (prn_adaptor_start(conn, be) != 0)
        return 1;

    /* 后端一次性启动钩子(§19 崩溃恢复: 清陈旧 wakelock/还原屏超时); mock 无 */
    if (be->boot_init) be->boot_init();

    /* ---- 事件循环 ---- */
    LOGI("service ready (backend=%s)", be->name);
    while (g_run) {
        /* 100ms 超时: 兼顾 SIGTERM 响应与 tick 粒度 */
        if (!dbus_connection_read_write_dispatch(conn, 100)) {
            LOGE("bus disconnected");
            break;
        }
        if (be->tick) be->tick();
    }

    LOGI("shutting down, saving config");
    prn_config_save(cfg_path);
    dbus_connection_flush(conn);
    if (is_private) dbus_connection_close(conn);
    dbus_connection_unref(conn);
    prn_log_close();
    return 0;
}
