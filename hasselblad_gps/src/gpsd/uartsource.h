/* uartsource.h — 位置数据源(d): 本地 UART NMEA(USB 串口, /dev/ttyUSB*)
 * 规格: X1D II 内置 GPS 驱动复刻(X1D II = ttyS3:9600 + GPIO19, 见
 * docs/GPS_LIFECYCLE_X1D_X1DII.md §1; X2D II 无 GPS UART, 经 USB host
 * + ftdi_sio.ko(设备 /system/lib/modules/ 实测在位)枚举 ttyUSB*)。
 * 行协议与 TCP 源共用 on_nmea_line 回调(RMC/GGA 白名单, talker 不校验)。
 */
#ifndef GPS_UARTSOURCE_H
#define GPS_UARTSOURCE_H

typedef void (*gps_uart_line_cb)(void *user, const char *line);

typedef struct {
    int  fd;                       /* <0 = 未连接 */
    char dev[128];
    int  baud;
    char module[256];              /* 可选 insmod 路径(空=跳过) */
    int  module_tried;
    int  open_fail_logged;
    long long next_try_ms;         /* 重连退避 */
    char buf[192];                 /* 行组装(NMEA 上限 82 + 余量, SUC 同款) */
    int  buf_len;
    gps_uart_line_cb on_line;
    void *user;
} gps_uart_t;

/* dev 为空 = 源禁用(init 直接返回)。module 非空时首次尝试 insmod 一次,
 * 失败不致命(模块可能已内建/已加载), 只记日志。 */
void gps_uart_init(gps_uart_t *u, const char *dev, int baud, const char *module,
                   gps_uart_line_cb on_line, void *user);
void gps_uart_tick(gps_uart_t *u, long long now);
void gps_uart_stop(gps_uart_t *u);

#endif
