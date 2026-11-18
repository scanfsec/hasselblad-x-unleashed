/* config.c — ini 解析 + 默认值落盘(见 config.h 头注释与规格 §4.2) */
#include "config.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

gps_cfg_t g_cfg;

void gps_cfg_defaults(gps_cfg_t *c)
{
    memset(c, 0, sizeof *c);
    c->poll_interval_ms = 2000;
    c->stable_ms        = 1500;
    c->event_delay_ms   = 2000;
    c->max_age_s        = 300;
    c->ibis_compensate  = 0;
    strcpy(c->nmea_tcp,   "");
    strcpy(c->uart_dev,   "/dev/ttyUSB0");
    c->uart_baud = 9600;
    strcpy(c->uart_module, "/system/lib/modules/ftdi_sio.ko");
    strcpy(c->dcim_root,  "/mnt/media_rw/ssd/DCIM");
    strcpy(c->inject_cmd, "/data/hasselblad_gps/bin/hginject");
    c->inject_timeout_s = 60;
    strcpy(c->gps_cur,    "/tmp/gps.cur");
    strcpy(c->done_path,  "/data/hasselblad_gps/run/done.list");
    strcpy(c->log_path,   "/data/hasselblad_gps/log/gpsd.log");
    c->log_max_kb = 512;
    c->log_level  = GPS_LOG_INFO;
}

static const char *DEFAULT_INI =
    "; hasselblad_gps gpsd 配置(自动生成, 可手改; 未知键忽略)\n"
    "[gpsd]\n"
    "; DCIM 扫描与 gps.cur 轮询周期(ms)\n"
    "poll_interval_ms = 2000\n"
    "; 新文件尺寸稳定窗口(ms, 两次一致才注入)\n"
    "stable_ms = 1500\n"
    "; exposures/storage processing counter 信号 → 扫描延迟(ms)\n"
    "event_delay_ms = 2000\n"
    "; 坐标最大年龄(s), 超龄照片跳过注入\n"
    "max_age_s = 300\n"
    "; 位置更新时同时喂 com.hasselblad.camera set_gps_position(IBIS 星野补偿)\n"
    "ibis_compensate = 0\n"
    "; TCP NMEA 桥(host:port, 连上后解析 RMC/GGA 喂缓存; 留空禁用)\n"
    "nmea_tcp =\n"
    "; 本地 UART NMEA 源(USB host + ftdi_sio → /dev/ttyUSB*; X1D GPS 模块经\n"
    "; FTDI 3.3V 小板接 USB-C 即此形态, 复刻 X1D II 内置 GPS 的 ttyS3:9600 驱动;\n"
    "; 留空禁用; 热插拔容忍, 未连接每 5s 重试)\n"
    "uart_dev = /dev/ttyUSB0\n"
    "uart_baud = 9600\n"
    "; 打开设备前尝试 insmod 一次(失败非致命: 可能已内建/已加载)\n"
    "uart_module = /system/lib/modules/ftdi_sio.ko\n"
    "; 照片监视根目录\n"
    "dcim_root = /mnt/media_rw/ssd/DCIM\n"
    "; 注入器命令(hginject 由并行开发线产出; 用法 hginject <file> --lat F --lon F [--alt F])\n"
    "inject_cmd = /data/hasselblad_gps/bin/hginject\n"
    "; 注入器超时(s), 超时 SIGKILL\n"
    "inject_timeout_s = 60\n"
    "; 兼容位置文件(x2gps gps_push.py 协议 \"lat lon [alt]\")\n"
    "gps_cur = /tmp/gps.cur\n"
    "done_path = /data/hasselblad_gps/run/done.list\n"
    "log_path = /data/hasselblad_gps/log/gpsd.log\n"
    "log_max_kb = 512\n"
    "; debug|info|warn|error\n"
    "log_level = info\n";

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = 0;
    return s;
}

static void apply(gps_cfg_t *c, const char *k, const char *v)
{
    if (!strcmp(k, "poll_interval_ms"))        c->poll_interval_ms = atoi(v);
    else if (!strcmp(k, "stable_ms"))          c->stable_ms = atoi(v);
    else if (!strcmp(k, "event_delay_ms"))     c->event_delay_ms = atoi(v);
    else if (!strcmp(k, "max_age_s"))          c->max_age_s = atoi(v);
    else if (!strcmp(k, "ibis_compensate"))    c->ibis_compensate = atoi(v);
    else if (!strcmp(k, "nmea_tcp"))           snprintf(c->nmea_tcp, sizeof c->nmea_tcp, "%s", v);
    else if (!strcmp(k, "uart_dev"))           snprintf(c->uart_dev, sizeof c->uart_dev, "%s", v);
    else if (!strcmp(k, "uart_baud"))          c->uart_baud = atoi(v);
    else if (!strcmp(k, "uart_module"))        snprintf(c->uart_module, sizeof c->uart_module, "%s", v);
    else if (!strcmp(k, "dcim_root"))          snprintf(c->dcim_root, sizeof c->dcim_root, "%s", v);
    else if (!strcmp(k, "inject_cmd"))         snprintf(c->inject_cmd, sizeof c->inject_cmd, "%s", v);
    else if (!strcmp(k, "inject_timeout_s"))   c->inject_timeout_s = atoi(v);
    else if (!strcmp(k, "gps_cur"))            snprintf(c->gps_cur, sizeof c->gps_cur, "%s", v);
    else if (!strcmp(k, "done_path"))          snprintf(c->done_path, sizeof c->done_path, "%s", v);
    else if (!strcmp(k, "log_path"))           snprintf(c->log_path, sizeof c->log_path, "%s", v);
    else if (!strcmp(k, "log_max_kb"))         c->log_max_kb = atoi(v);
    else if (!strcmp(k, "log_level")) {
        int l = gps_log_level_parse(v);
        if (l >= 0) c->log_level = l;
    }
    /* 未知键忽略(前向兼容) */
}

int gps_cfg_load(const char *path)
{
    gps_cfg_defaults(&g_cfg);

    FILE *f = fopen(path, "r");
    if (!f) {
        /* 首启: 写默认模板(目录可能不存在 → 建) */
        char dir[512];
        snprintf(dir, sizeof dir, "%s", path);
        char *slash = strrchr(dir, '/');
        if (slash && slash != dir) {
            *slash = 0;
            gps_mkdir_p(dir);
        }
        FILE *w = fopen(path, "w");
        if (!w) return -1;
        fputs(DEFAULT_INI, w);
        fclose(w);
        return 0;
    }

    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (!*s || *s == ';' || *s == '#') continue;
        if (*s == '[') continue;                      /* 节名仅注释性 */
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(s), *v = trim(eq + 1);
        if (*k) apply(&g_cfg, k, v);
    }
    fclose(f);

    /* 下限钳制(防手滑写 0 把服务变成忙轮询) */
    if (g_cfg.poll_interval_ms < 250)  g_cfg.poll_interval_ms = 250;
    if (g_cfg.stable_ms < 500)         g_cfg.stable_ms = 500;
    if (g_cfg.event_delay_ms < 0)      g_cfg.event_delay_ms = 0;
    if (g_cfg.max_age_s < 10)          g_cfg.max_age_s = 10;
    if (g_cfg.inject_timeout_s < 5)    g_cfg.inject_timeout_s = 5;
    if (g_cfg.log_max_kb < 32)         g_cfg.log_max_kb = 32;
    return 0;
}
