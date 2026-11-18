/* config.h — 手写最小 ini 配置(无第三方依赖, x2print config 同构)
 * 规格: docs/X2DII_PORT_ANALYSIS.md §4.2(/data/hasselblad_gps/etc/hgpsd.conf:
 * poll 间隔/稳定窗口/HEIC on/off 等); 部署名 gpsd.conf。
 * 语义: `;`/`#` 注释, [节] 仅注释性, key = value 首个 '=' 切分, CRLF 容忍,
 * 未知键忽略(前向兼容), 非法值取默认并告警。首启文件不存在则写默认模板。
 */
#ifndef GPS_CONFIG_H
#define GPS_CONFIG_H

typedef struct {
    int  poll_interval_ms;        /* DCIM 扫描 + gps.cur 轮询周期(默认 2000) */
    int  stable_ms;               /* 新文件尺寸稳定窗口(1.5s 两次一致, 默认 1500) */
    int  event_delay_ms;          /* counter 信号 → 扫描延迟(默认 2000) */
    int  max_age_s;               /* 坐标新鲜度上限(默认 300 = 5min, 超龄跳过注入) */
    int  ibis_compensate;         /* 位置更新时喂 set_gps_position(IBIS, 默认 0) */
    char nmea_tcp[128];           /* TCP NMEA 桥 "host:port"(空=禁用) */
    char uart_dev[128];           /* 本地 UART NMEA 设备(默认 /dev/ttyUSB0, 空=禁用) */
    int  uart_baud;               /* UART 波特率(默认 9600, 8N1 固定) */
    char uart_module[256];        /* UART 前置 insmod 的内核模块(默认 ftdi_sio.ko) */
    char dcim_root[256];          /* 照片监视根(默认 /mnt/media_rw/ssd/DCIM) */
    char inject_cmd[256];         /* 注入器(默认 /data/hasselblad_gps/bin/hginject) */
    int  inject_timeout_s;        /* hginject 超时 kill(默认 60) */
    char gps_cur[256];            /* 兼容位置文件(默认 /tmp/gps.cur) */
    char done_path[256];          /* 已处理清单(默认 /data/hasselblad_gps/run/done.list) */
    char log_path[256];           /* 日志(默认 /data/hasselblad_gps/log/gpsd.log) */
    int  log_max_kb;              /* 日志滚动上限 KB(默认 512) */
    int  log_level;               /* 默认 info */
} gps_cfg_t;

extern gps_cfg_t g_cfg;

void gps_cfg_defaults(gps_cfg_t *c);
/* 加载: 文件不存在→写默认模板并返回 0; 解析失败键→默认值+告警, 返回 0;
 * 致命错误(无法建目录/无法写默认)返回 -1。 */
int  gps_cfg_load(const char *path);

#endif
