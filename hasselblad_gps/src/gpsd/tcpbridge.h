/* tcpbridge.h — TCP NMEA 桥客户端(可选数据源, 配置 nmea_tcp=host:port)
 * 规格: docs/X2DII_PORT_ANALYSIS.md §2 数据源(c): 连上后逐行收 NMEA,
 * 解析 RMC/GGA 喂缓存, 整句经 nmea_sentence 信号转发(致敬 X1D II gpsd)。
 * 全非阻塞(主循环 tick 驱动, 不阻塞 DBus dispatch); 断线 5s 重连;
 * 帧规则沿用 X1D: '$' 起 '\n' 止, 缓冲上限 820B(超限清空, gpsd 同款)。
 */
#ifndef GPS_TCPBRIDGE_H
#define GPS_TCPBRIDGE_H

typedef void (*gps_tcp_line_cb)(void *user, const char *line);

typedef struct {
    char host[64];
    int  port;
    char name[24];
    int  state;                     /* 0=off 1=retry-wait 2=connecting 3=connected */
    int  fd;
    long long next_try_ms;
    char buf[820];
    int  buf_len;
    gps_tcp_line_cb cb;
    void *user;
} gps_tcp_t;

void gps_tcp_init(gps_tcp_t *t, const char *hostport, const char *name,
                  gps_tcp_line_cb cb, void *user);
void gps_tcp_tick(gps_tcp_t *t, long long now);   /* 连接机 + 收数 + 行分帧 */
void gps_tcp_stop(gps_tcp_t *t);

#endif
