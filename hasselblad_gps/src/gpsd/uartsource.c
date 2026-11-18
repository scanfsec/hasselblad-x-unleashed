/* uartsource.c — 本地 UART NMEA 源(非阻塞 tick, 主循环轮询; 无线程)
 * termios 配方照抄 X1D II 工厂工具 test_gps 的 8N1 raw 配置
 * (c_iflag=IGNPAR, c_oflag=0, c_lflag=0, c_cflag|=CS8|CREAD|CLOCAL;
 *  VMIN=0/VTIME=0 + O_NONBLOCK — 我们在 tick 里批量读, 不阻塞总线)。
 * 行组装: '$' 起不必强求(容忍裸流), 以 '\n' 断句, 上限 82+余量,
 * 超限丢弃重建 —— 语义对齐 X1D SUC 的 frameposoverflow。
 */
#include "uartsource.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <stdlib.h>

#define UART_RETRY_MS 5000        /* 未连接重试周期(热插拔容忍) */
#define UART_LINE_MAX 100         /* NMEA 82 上限 + CRC/余量 */

static int speed_of(int baud)
{
    switch (baud) {
    case 4800:   return B4800;
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 115200: return B115200;
    default:     return -1;
    }
}

void gps_uart_init(gps_uart_t *u, const char *dev, int baud, const char *module,
                   gps_uart_line_cb on_line, void *user)
{
    memset(u, 0, sizeof *u);
    u->fd = -1;
    if (!dev || !*dev) return;                       /* 源禁用 */
    snprintf(u->dev, sizeof u->dev, "%s", dev);
    u->baud = baud > 0 ? baud : 9600;
    if (module && *module) snprintf(u->module, sizeof u->module, "%s", module);
    u->on_line = on_line;
    u->user = user;
    if (speed_of(u->baud) < 0) {
        LOGW("uart 源波特率 %d 不支持(用 4800/9600/19200/38400/57600/115200), 按 9600", u->baud);
        u->baud = 9600;
    }
}

static void uart_close(gps_uart_t *u)
{
    if (u->fd >= 0) close(u->fd);
    u->fd = -1;
}

static void uart_try_open(gps_uart_t *u, long long now)
{
    /* 可选内核模块(FTDI): 每会话试 insmod 一次, 失败继续(内建/已载/缺件都可能导致) */
    if (u->module[0] && !u->module_tried) {
        u->module_tried = 1;
        char cmd[360];
        snprintf(cmd, sizeof cmd, "insmod %s 2>/dev/null", u->module);
        int rc = system(cmd);
        LOGI("uart 源: insmod %s rc=%d(非致命: 可能已内建/已加载)", u->module, rc);
    }
    int fd = open(u->dev, O_RDONLY | O_NONBLOCK | O_NOCTTY);
    if (fd < 0) {
        if (!u->open_fail_logged) {                  /* 每次掉线只啰嗦一回 */
            LOGI("uart 源未就绪(%s: %s), 将每 %ds 重试(热插拔容忍)",
                 u->dev, strerror(errno), UART_RETRY_MS / 1000);
            u->open_fail_logged = 1;
        }
        u->next_try_ms = now + UART_RETRY_MS;
        return;
    }
    struct termios t;
    if (tcgetattr(fd, &t) == 0) {
        t.c_iflag = IGNPAR;
        t.c_oflag = 0;
        t.c_lflag = 0;                               /* raw */
        t.c_cflag = CS8 | CREAD | CLOCAL;
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;
        int sp = speed_of(u->baud);
        cfsetispeed(&t, (speed_t)sp);
        cfsetospeed(&t, (speed_t)sp);
        tcflush(fd, TCIFLUSH);
        if (tcsetattr(fd, TCSANOW, &t) != 0)
            LOGW("uart 源 tcsetattr 失败(%s), 沿用默认项", strerror(errno));
    } else {
        LOGW("uart 源 tcgetattr 失败(%s) —— 设备可能非 tty, 继续裸读", strerror(errno));
    }
    u->fd = fd;
    u->open_fail_logged = 0;
    u->buf_len = 0;
    LOGI("uart 源已连接: %s @%d 8N1", u->dev, u->baud);
}

static void uart_feed_byte(gps_uart_t *u, char c)
{
    if (c == '\r') return;
    if (c == '\n') {
        if (u->buf_len > 0) {
            u->buf[u->buf_len] = '\0';
            if (u->on_line) u->on_line(u->user, u->buf);
            u->buf_len = 0;
        }
        return;
    }
    if (u->buf_len >= UART_LINE_MAX) {               /* 溢出丢弃(对齐 SUC) */
        u->buf_len = 0;
        return;
    }
    u->buf[u->buf_len++] = c;
}

void gps_uart_tick(gps_uart_t *u, long long now)
{
    if (!u->dev[0]) return;
    if (u->fd < 0) {
        if (now >= u->next_try_ms) uart_try_open(u, now);
        if (u->fd < 0) return;
    }
    char chunk[512];
    for (;;) {
        ssize_t n = read(u->fd, chunk, sizeof chunk);
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) uart_feed_byte(u, chunk[i]);
            continue;                                /* 一次 tick 排空 */
        }
        if (n == 0) return;                          /* 无数据(O_NONBLOCK) */
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        /* 真错误: 设备拔出等 → 关闭转重连 */
        LOGI("uart 源断开(%s: %s), 转重连", u->dev, strerror(errno));
        uart_close(u);
        u->next_try_ms = now + UART_RETRY_MS;
        return;
    }
}

void gps_uart_stop(gps_uart_t *u)
{
    if (!u->dev[0]) return;
    uart_close(u);
}
