/* main.c — gpsd 守护进程入口(X2D II 地理标记服务, com.hasselblad.gpsd)
 * 流程(x2print printer-service main.c 同构, 注册配方
 * docs/X2DII_PORT_ANALYSIS.md §2.3):
 *   解析参数 → 配置/日志 → 连总线(占名 DO_NOT_QUEUE)→ 注册 adaptor
 *   → 订阅 /camera /storage propertiesChanged(触发层)→ monitor init+seed
 *   → 事件循环(read_write_dispatch 100ms + tick: gps.cur 轮询/TCP 桥/
 *     DCIM 扫描/注入收割)→ SIGTERM 干净退出(flush 日志, 绝不动照片)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <sys/stat.h>
#include <dbus/dbus.h>

#include "log.h"
#include "config.h"
#include "position.h"
#include "nmea.h"
#include "tcpbridge.h"
#include "uartsource.h"
#include "monitor.h"
#include "adaptor.h"

#define GPSD_VERSION "gpsd 1.0 (hasselblad_gps)"
#define DEFAULT_CFG  "/data/hasselblad_gps/etc/gpsd.conf"
/* /tmp/gps.cur 之外的第二位置文件(部署目录内, 跨重启更稳) */
#define SECONDARY_CUR "/data/hasselblad_gps/run/gps.cur"

static volatile sig_atomic_t g_run = 1;

static void on_signal(int sig)
{
    (void)sig;
    g_run = 0;                       /* 事件循环 100ms 超时, 很快察觉 */
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

static void usage(void)
{
    fputs(
        "gpsd — X2D II GPS 服务 (com.hasselblad.gpsd /gpsd)\n"
        "用法: gpsd [选项]\n"
        "  --config <path>    配置文件 (默认 " DEFAULT_CFG ")\n"
        "  --bus <address>    D-Bus 总线地址 (默认: 环境/系统总线)\n"
        "  --log <file>       日志文件 (默认取配置 log_path)\n"
        "  --loglevel <lvl>   debug|info|warn|error (默认 info)\n"
        "  --no-seed          跳过启动存量 seed(危险: 旧照片会被当前坐标重标)\n"
        "  --version          版本\n"
        "  --help             本帮助\n",
        stderr);
}

/* ---------- 兼容数据源(b): /tmp/gps.cur 文件轮询 ----------
 * 协议(x2gps gps_push.py): 一行 "lat lon [alt]"。内容与上次不同才喂缓存。 */
static char s_cur_last[256] = "";

static void poll_gps_cur(void)
{
    const char *paths[2] = { g_cfg.gps_cur, SECONDARY_CUR };
    for (int i = 0; i < 2; i++) {
        if (!paths[i][0]) continue;
        FILE *f = fopen(paths[i], "r");
        if (!f) continue;
        char line[256] = "";
        if (!fgets(line, sizeof line, f)) { fclose(f); continue; }
        fclose(f);
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = 0;
        if (!line[0]) continue;
        if (!strcmp(line, s_cur_last)) continue;      /* 未变化 */
        snprintf(s_cur_last, sizeof s_cur_last, "%s", line);

        double lat = 0, lon = 0, alt = 0;
        int n = sscanf(line, "%lf %lf %lf", &lat, &lon, &alt);
        if (n < 2 || isnan(lat) || isnan(lon) ||
            lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
            LOGW("gps.cur 内容非法(忽略): '%s'", line);
            continue;
        }
        g_pos.lat = lat;
        g_pos.lon = lon;
        if (n >= 3 && !isnan(alt) && !isinf(alt)) { g_pos.alt = alt; g_pos.have_alt = 1; }
        else                                     { g_pos.alt = 0;   g_pos.have_alt = 0; }
        /* 文件协议无这些字段: 清零防跨源残留 */
        g_pos.dop = 0; g_pos.speed = 0; g_pos.track = 0; g_pos.nsat = 0;
        g_pos.fix_valid = 1;
        g_pos.utc[0] = 0;
        gps_pos_stamp("file");
        LOGI("gps.cur 喂入: lat=%.6f lon=%.6f%s", lat, lon,
             g_pos.have_alt ? " alt 有" : "");
        gps_adaptor_on_position_updated();
    }
}

/* ---------- 兼容数据源(c): TCP NMEA 桥 ---------- */
static gps_tcp_t s_tcp;

/* ---------- 数据源(d): 本地 UART NMEA(USB 串口, X1D II 内置 GPS 同款驱动) ---------- */
static gps_uart_t s_uart;

static void on_nmea_line(void *user, const char *line)
{
    (void)user;
    gps_adaptor_emit_nmea(line);                     /* 整句转发(致敬 X1D II) */
    gps_pos_t snap = g_pos;
    unsigned fields = 0;
    if (nmea_parse(line, &snap, &fields) == 0) {
        int meaningful = (fields & (NMEA_F_LATLON | NMEA_F_FIX)) != 0;
        g_pos = snap;
        gps_pos_stamp("nmea");
        if (meaningful) {
            LOGD("nmea 喂入: lat=%.6f lon=%.6f fix=%d", g_pos.lat, g_pos.lon,
                 g_pos.fix_valid);
            gps_adaptor_on_position_updated();
        }
    } else {
        LOGD("nmea 句丢弃(非 RMC/GGA 或校验失败): %.32s", line);
    }
}

int main(int argc, char **argv)
{
    const char *cfg_path = DEFAULT_CFG, *bus_addr = NULL, *log_path = NULL;
    int level = -1, no_seed = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc)     cfg_path = argv[++i];
        else if (!strcmp(argv[i], "--bus") && i + 1 < argc)   bus_addr = argv[++i];
        else if (!strcmp(argv[i], "--log") && i + 1 < argc)   log_path = argv[++i];
        else if (!strcmp(argv[i], "--loglevel") && i + 1 < argc) {
            level = gps_log_level_parse(argv[++i]);
            if (level < 0) { fprintf(stderr, "bad --loglevel\n"); return 2; }
        }
        else if (!strcmp(argv[i], "--no-seed"))               no_seed = 1;
        else if (!strcmp(argv[i], "--version"))               { puts(GPSD_VERSION); return 0; }
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
        else { fprintf(stderr, "未知参数: %s (试试 --help)\n", argv[i]); return 2; }
    }

    if (gps_cfg_load(cfg_path) != 0)
        fprintf(stderr, "警告: 配置加载/生成失败(%s), 用内置默认\n", cfg_path);
    if (level < 0) level = g_cfg.log_level;
    if (!log_path) log_path = g_cfg.log_path;
    gps_log_init(log_path, level, g_cfg.log_max_kb * 1024);
    LOGI(GPSD_VERSION " starting (pid=%d, cfg=%s, bus=%s)", (int)getpid(), cfg_path,
         bus_addr ? bus_addr : "<env/default>");

    gps_pos_reset();
    install_signals();

    /* ---- 连总线(§2.3 配方) ---- */
    DBusError e;
    dbus_error_init(&e);
    dbus_connection_set_change_sigpipe(FALSE);
    DBusConnection *conn;
    int is_private = 0;
    if (bus_addr) {
        conn = dbus_connection_open_private(bus_addr, &e);
        is_private = 1;
    } else {
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
    int ret = dbus_bus_request_name(conn, GPSD_NAME, DBUS_NAME_FLAG_DO_NOT_QUEUE, &e);
    if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        LOGE("request_name %s failed ret=%d: %s", GPSD_NAME, ret,
             e.message ? e.message : "?");
        return 1;
    }
    LOGI("bus name %s acquired", GPSD_NAME);

    if (gps_adaptor_start(conn) != 0) return 1;
    if (gps_adaptor_install_subscriptions(conn) != 0)
        LOGW("信号订阅失败 —— 事件层退化, 仅 2s 兜底轮询可用");

    /* ---- 照片监视 + 存量 seed(防历史照片被当前坐标重标) ---- */
    gps_monitor_init();
    if (!no_seed) gps_monitor_seed();
    else LOGW("--no-seed: 跳过存量 seed(仅调试用!)");

    /* ---- 可选 TCP NMEA 桥 + 本地 UART NMEA 源(共用 on_nmea_line) ---- */
    gps_tcp_init(&s_tcp, g_cfg.nmea_tcp, "nmea", on_nmea_line, NULL);
    gps_uart_init(&s_uart, g_cfg.uart_dev, g_cfg.uart_baud, g_cfg.uart_module,
                  on_nmea_line, NULL);

    /* ---- 事件循环 ---- */
    LOGI("gpsd ready (dcim=%s inject=%s ibis=%d nmea_tcp=%s uart=%s@%d)",
         g_cfg.dcim_root, g_cfg.inject_cmd, g_cfg.ibis_compensate,
         g_cfg.nmea_tcp[0] ? g_cfg.nmea_tcp : "off",
         g_cfg.uart_dev[0] ? g_cfg.uart_dev : "off", g_cfg.uart_baud);
    long long last_cur_ms = -1;
    while (g_run) {
        if (!dbus_connection_read_write_dispatch(conn, 100)) {
            LOGE("bus disconnected");
            break;
        }
        long long now = gps_now_ms();
        if (last_cur_ms < 0 || now - last_cur_ms >= g_cfg.poll_interval_ms) {
            last_cur_ms = now;
            poll_gps_cur();
        }
        gps_tcp_tick(&s_tcp, now);
        gps_uart_tick(&s_uart, now);
        gps_monitor_tick(now);
    }

    /* ---- 优雅退出: done.list 已即时落盘; 杀注入子进程; flush 日志 ---- */
    LOGI("shutting down (SIGTERM), state persisted, photos untouched");
    gps_tcp_stop(&s_tcp);
    gps_uart_stop(&s_uart);
    gps_monitor_shutdown();
    dbus_connection_flush(conn);
    if (is_private) dbus_connection_close(conn);
    dbus_connection_unref(conn);
    gps_log_close();
    return 0;
}
