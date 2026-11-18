/* tcpbridge.c — TCP NMEA 桥实现(见 tcpbridge.h)
 * 连接序列: getaddrinfo(建议填 IP, DNS 可能短阻塞) → socket → O_NONBLOCK →
 * connect(EINPROGRESS → 下一 tick 查 SO_ERROR) → recv 循环至 EAGAIN。
 * 行分帧: 找 '\n', 截 '\r', '$' 开头才回调(解析器再校验)。
 */
#include "tcpbridge.h"
#include "log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define TCP_RETRY_MS 5000

enum { TS_OFF = 0, TS_RETRY = 1, TS_CONNECTING = 2, TS_CONNECTED = 3 };

static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

void gps_tcp_init(gps_tcp_t *t, const char *hostport, const char *name,
                  gps_tcp_line_cb cb, void *user)
{
    memset(t, 0, sizeof *t);
    t->fd = -1;
    t->state = TS_OFF;
    t->cb = cb;
    t->user = user;
    snprintf(t->name, sizeof t->name, "%s", name ? name : "nmea");

    if (!hostport || !*hostport) return;
    char buf[80];
    snprintf(buf, sizeof buf, "%s", hostport);
    char *colon = strrchr(buf, ':');
    if (!colon || colon == buf || !colon[1]) {
        LOGW("nmea_tcp 配置非法('%s', 要 host:port), 桥禁用", hostport);
        return;
    }
    *colon = 0;
    snprintf(t->host, sizeof t->host, "%s", buf);
    t->port = atoi(colon + 1);
    if (t->port <= 0 || t->port > 65535) {
        LOGW("nmea_tcp 端口非法('%s'), 桥禁用", hostport);
        t->port = 0;
        return;
    }
    t->state = TS_RETRY;
    t->next_try_ms = 0;                                /* 首 tick 即连 */
    LOGI("nmea 桥就绪: %s:%d (tick 驱动连接)", t->host, t->port);
}

void gps_tcp_stop(gps_tcp_t *t)
{
    if (t->fd >= 0) close(t->fd);
    t->fd = -1;
    t->state = TS_OFF;
}

static void try_connect(gps_tcp_t *t, long long now)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[8];
    snprintf(portstr, sizeof portstr, "%d", t->port);
    int rc = getaddrinfo(t->host, portstr, &hints, &res);
    if (rc != 0 || !res) {
        LOGW("nmea 桥 %s:%d 解析失败(%s), %ds 后重试", t->host, t->port,
             gai_strerror(rc), TCP_RETRY_MS / 1000);
        t->state = TS_RETRY;
        t->next_try_ms = now + TCP_RETRY_MS;
        return;
    }
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        t->state = TS_RETRY;
        t->next_try_ms = now + TCP_RETRY_MS;
        return;
    }
    set_nonblock(fd);
    rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc == 0) {
        t->fd = fd;
        t->state = TS_CONNECTED;
        t->buf_len = 0;
        LOGI("nmea 桥已连接 %s:%d", t->host, t->port);
        return;
    }
    if (errno == EINPROGRESS) {
        t->fd = fd;
        t->state = TS_CONNECTING;
        return;
    }
    close(fd);
    t->state = TS_RETRY;
    t->next_try_ms = now + TCP_RETRY_MS;
}

static void drop(gps_tcp_t *t, long long now, const char *why)
{
    LOGW("nmea 桥断开(%s), %ds 后重试", why, TCP_RETRY_MS / 1000);
    if (t->fd >= 0) close(t->fd);
    t->fd = -1;
    t->buf_len = 0;
    t->state = TS_RETRY;
    t->next_try_ms = now + TCP_RETRY_MS;
}

static void dispatch_lines(gps_tcp_t *t)
{
    /* 以 '\n' 切行; 行内去掉 '\r'; 交回调前不拷贝(buf 内就地 NUL 终止) */
    int start = 0;
    for (int i = 0; i < t->buf_len; i++) {
        if (t->buf[i] != '\n') continue;
        int len = i - start;
        if (len > 0 && t->buf[start + len - 1] == '\r') len--;
        t->buf[start + len] = 0;
        if (len > 0 && t->buf[start] == '$' && t->cb)
            t->cb(t->user, &t->buf[start]);
        start = i + 1;
    }
    if (start > 0) {
        memmove(t->buf, t->buf + start, (size_t)(t->buf_len - start));
        t->buf_len -= start;
    }
    if (t->buf_len >= (int)sizeof t->buf) {
        /* 缓冲满仍无换行(X1D gpsd 同款处置): 清空 */
        LOGW("nmea 桥缓冲溢出(无换行), 清空");
        t->buf_len = 0;
    }
}

void gps_tcp_tick(gps_tcp_t *t, long long now)
{
    if (t->state == TS_OFF) return;

    if (t->state == TS_RETRY) {
        if (now >= t->next_try_ms) try_connect(t, now);
        return;
    }
    if (t->state == TS_CONNECTING) {
        int err = 0;
        socklen_t elen = sizeof err;
        if (getsockopt(t->fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err != 0) {
            drop(t, now, err ? strerror(err) : "getsockopt");
            return;
        }
        t->state = TS_CONNECTED;
        t->buf_len = 0;
        LOGI("nmea 桥已连接 %s:%d", t->host, t->port);
        return;
    }

    /* connected: 收到 EAGAIN 为止 */
    for (;;) {
        ssize_t n = recv(t->fd, t->buf + t->buf_len, sizeof t->buf - (size_t)t->buf_len, 0);
        if (n > 0) {
            t->buf_len += (int)n;
            dispatch_lines(t);
            continue;
        }
        if (n == 0) { drop(t, now, "对端关闭"); return; }
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        if (errno == EINTR) continue;
        drop(t, now, strerror(errno));
        return;
    }
}
