/* mdns.c — 最小 mDNS/DNS-SD 查询客户端实现
 *
 * 协议要点（RFC 1035 报文格式 + RFC 6762 mDNS）：
 *  - 组播地址 224.0.0.251:5353；查询 ID 置 0、不置 RD。
 *  - 名字压缩：标签前缀 0xC0 指针指向报文内偏移，只允许向前指，解码需防环。
 *  - 发现流程：查 _ipp._tcp.local 与 _universal._sub._ipp._tcp.local 的 PTR，
 *    从应答各节收集 PTR(实例名)/SRV(端口+主机)/TXT(能力键值)/A(IP)，
 *    缺 SRV/A 的实例再定向补查。
 *
 * 无 avahi、无依赖，纯 POSIX socket + select 超时。
 */
#define _DEFAULT_SOURCE /* Linux/glibc 下启用 IP_ADD_MEMBERSHIP 等扩展声明 */

#include "mdns.h"
#include "netlog.h"

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define MDNS_GROUP "224.0.0.251"
#define MDNS_PORT  5353
#define DNS_MAX_NAME 256
#define PKT_MAX     9000 /* jumbo 安全上限 */

/* ---------------- 基础编解码 ---------------- */

/* 编码 QNAME：点分名字 → 标签序列（不含压缩，查询方向无需压缩） */
static int wr_qname(unsigned char *p, int cap, int pos, const char *name)
{
    const char *s = name;
    while (*s) {
        const char *e = strchr(s, '.');
        int l = e ? (int)(e - s) : (int)strlen(s);
        if (l <= 0 || l > 63 || pos + 1 + l >= cap) return -1;
        p[pos++] = (unsigned char)l;
        memcpy(p + pos, s, (size_t)l);
        pos += l;
        if (!e) break;
        s = e + 1;
    }
    if (pos >= cap) return -1;
    p[pos++] = 0; /* 根标签 */
    return pos;
}

/* 解码（可能压缩的）域名；*consumed 返回顺序读取流的下一字节位置（遇指针则是指针后） */
static int rd_name(const unsigned char *p, int plen, int pos,
                   char *out, int cap, int *consumed)
{
    int jumps = 0, started = 0;
    size_t len = 0;
    *consumed = -1;
    out[0] = '\0';
    for (;;) {
        if (pos < 0 || pos >= plen) return -1;
        unsigned char b = p[pos];
        if ((b & 0xC0) == 0xC0) { /* 压缩指针 */
            if (pos + 1 >= plen) return -1;
            int off = ((b & 0x3F) << 8) | p[pos + 1];
            if (*consumed < 0) *consumed = pos + 2;
            if (off >= pos) return -1;   /* 只允许向前指，防环 */
            pos = off;
            if (++jumps > 8) return -1;  /* 二次保险 */
            continue;
        }
        if (b == 0) { /* 根标签，结束 */
            if (*consumed < 0) *consumed = pos + 1;
            return (int)len;
        }
        int l = b;
        if (pos + 1 + l > plen) return -1;
        if (started) {
            if (len + 1 >= (size_t)cap) return -1;
            out[len++] = '.';
        }
        if (len + l >= (size_t)cap) return -1;
        memcpy(out + len, p + pos + 1, (size_t)l);
        len += (size_t)l;
        out[len] = '\0';
        started = 1;
        pos += 1 + l;
    }
}

/* 构造一条查询报文；rd 置位 Recursion Desired（单播公网查询用，mDNS 不置） */
static int make_query(unsigned char *pkt, int cap, int id, int rd,
                      const char *qname, int qtype)
{
    memset(pkt, 0, 12);
    pkt[0] = (unsigned char)(id >> 8);
    pkt[1] = (unsigned char)id;
    pkt[2] = rd ? 0x01 : 0x00;
    pkt[5] = 1; /* QDCOUNT = 1 */
    int pos = wr_qname(pkt, cap, 12, qname);
    if (pos < 0 || pos + 4 > cap) return -1;
    pkt[pos++] = (unsigned char)(qtype >> 8);
    pkt[pos++] = (unsigned char)qtype;
    pkt[pos++] = 0;
    pkt[pos++] = 1; /* QCLASS = IN */
    return pos;
}

/* ---------------- 应答解析 ---------------- */

/* 文本化 TXT：长度前缀串拼接 "k=v;k=v" */
static void txt_to_text(const unsigned char *rd, int rlen, char *out, int cap)
{
    int p = 0, o = 0;
    while (p < rlen) {
        int l = rd[p++];
        if (l == 0 || p + l > rlen) break;
        if (o && o < cap - 1) out[o++] = ';';
        int c = l;
        if (o + c > cap - 1) c = cap - 1 - o;
        memcpy(out + o, rd + p, (size_t)c);
        o += c;
        p += l;
        if (o >= cap - 1) break;
    }
    out[o] = '\0';
}

int mdns_parse_reply(const unsigned char *pkt, int len, mdns_rr_cb cb, void *ud)
{
    if (!pkt || len < 12) return -1;
    if (!(pkt[2] & 0x80)) return -1; /* 必须 QR=1（应答） */
    int qd = (pkt[4] << 8) | pkt[5];
    int an = (pkt[6] << 8) | pkt[7];
    int ns = (pkt[8] << 8) | pkt[9];
    int ar = (pkt[10] << 8) | pkt[11];
    int pos = 12;
    char name[DNS_MAX_NAME];

    for (int i = 0; i < qd; i++) { /* 跳过 question 区 */
        int used;
        if (rd_name(pkt, len, pos, name, sizeof(name), &used) < 0) return -1;
        pos = used + 4;
        if (pos > len) return -1;
    }

    int total = an + ns + ar;
    for (int i = 0; i < total; i++) {
        int used;
        if (rd_name(pkt, len, pos, name, sizeof(name), &used) < 0) return -1;
        pos = used;
        if (pos + 10 > len) return -1;
        int type = (pkt[pos] << 8) | pkt[pos + 1];
        int ttl  = (pkt[pos + 4] << 24) | (pkt[pos + 5] << 16) |
                   (pkt[pos + 6] << 8)  | pkt[pos + 7];
        int rlen = (pkt[pos + 8] << 8) | pkt[pos + 9];
        pos += 10;
        if (pos + rlen > len) return -1;
        const unsigned char *rd = pkt + pos;

        mdns_rr_t rr;
        memset(&rr, 0, sizeof(rr));
        rr.name  = name;
        rr.type  = type;
        rr.ttl   = ttl;
        rr.rdata = rd;
        rr.rlen  = rlen;
        char aux[DNS_MAX_NAME];
        switch (type) {
        case MDNS_T_A:
            if (rlen == 4) {
                struct in_addr a;
                memcpy(&a, rd, 4);
                if (!inet_ntop(AF_INET, &a, rr.text, sizeof(rr.text))) rr.text[0] = '\0';
            }
            break;
        case MDNS_T_AAAA:
            if (rlen == 16) {
                struct in6_addr a6;
                memcpy(&a6, rd, 16);
                if (!inet_ntop(AF_INET6, &a6, rr.text, sizeof(rr.text))) rr.text[0] = '\0';
            }
            break;
        case MDNS_T_PTR:
        case MDNS_T_CNAME:
            if (rd_name(pkt, len, pos, aux, sizeof(aux), &used) >= 0)
                snprintf(rr.text, sizeof(rr.text), "%s", aux);
            break;
        case MDNS_T_SRV: {
            if (rlen < 7) break;
            int used2;
            rr.srv_port = (rd[4] << 8) | rd[5];
            if (rd_name(pkt, len, pos + 6, aux, sizeof(aux), &used2) >= 0)
                snprintf(rr.text, sizeof(rr.text), "%s", aux);
            break;
        }
        case MDNS_T_TXT:
            txt_to_text(rd, rlen, rr.text, sizeof(rr.text));
            break;
        default:
            break; /* 未知类型：text 留空，不崩 */
        }
        if (cb) cb(ud, &rr);
        pos += rlen;
    }
    return 0;
}

/* ---------------- 单播查询（自测用） ---------------- */

int mdns_query_unicast(const char *server_ip, int port, const char *qname,
                       int qtype, unsigned char *ans, int ans_max, int timeout_ms)
{
    if (!server_ip || !qname || !ans || ans_max < 512) return -1;
    if (port <= 0) port = 53;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { LOGE("socket: %s", strerror(errno)); return -1; }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, server_ip, &dst.sin_addr) != 1) {
        LOGE("非法服务器地址: %s", server_ip);
        close(fd);
        return -1;
    }

    unsigned char q[512], id = (unsigned char)(getpid() & 0xFF);
    int qlen = make_query(q, sizeof(q), id, 1 /*RD*/, qname, qtype);
    if (qlen < 0) { close(fd); return -1; }

    struct timeval tv = { .tv_sec = timeout_ms / 1000,
                          .tv_usec = (timeout_ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (sendto(fd, q, (size_t)qlen, 0, (struct sockaddr *)&dst, sizeof(dst)) != qlen) {
        LOGE("sendto: %s", strerror(errno));
        close(fd);
        return -1;
    }

    int n = (int)recv(fd, ans, (size_t)ans_max, 0);
    close(fd);
    if (n < 0) {
        LOGE("recv 超时/失败: %s", strerror(errno));
        return -2;
    }
    if (n < 12) return -1;
    if ((ans[0] << 8 | ans[1]) != id) return -1; /* 应答 ID 不匹配 */
    LOGD("单播应答 %d 字节 (rcode=%d)", n, ans[3] & 0x0F);
    return n;
}

/* ---------------- 组播发现 ---------------- */

static uint64_t now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}

typedef struct {
    mdns_service_t *svc;
    int n, max;
} col_t;

/* 按实例名找槽位，没有则新增；满/异常返回 -1 */
static int slot_of(col_t *c, const char *instance)
{
    if (!instance || !instance[0]) return -1;
    for (int i = 0; i < c->n; i++)
        if (strcmp(c->svc[i].instance, instance) == 0) return i;
    if (c->n >= c->max) return -1;
    memset(&c->svc[c->n], 0, sizeof(mdns_service_t));
    snprintf(c->svc[c->n].instance, sizeof(c->svc[c->n].instance), "%s", instance);
    return c->n++;
}

static void txt_merge(mdns_service_t *s, const char *kv)
{
    if (!kv || !kv[0]) return;
    if (strstr(s->txt, kv)) return; /* 粗去重 */
    if (s->txt[0]) strncat(s->txt, ";", sizeof(s->txt) - strlen(s->txt) - 1);
    strncat(s->txt, kv, sizeof(s->txt) - strlen(s->txt) - 1);
}

/* 应答收集器：PTR 建槽，SRV/TXT 补槽，A 反查主机 */
static void col_cb(void *ud, const mdns_rr_t *rr)
{
    col_t *c = (col_t *)ud;
    switch (rr->type) {
    case MDNS_T_PTR:
        /* 只关心 _ipp._tcp 族（含 _universal._sub._ipp._tcp 子类型） */
        if (strstr(rr->name, "_ipp._tcp") && rr->text[0]) {
            int i = slot_of(c, rr->text);
            if (i >= 0 && strstr(rr->name, "_universal._sub")) c->svc[i].universal = 1;
        }
        break;
    case MDNS_T_SRV: {
        int i = slot_of(c, rr->name);
        if (i >= 0 && rr->srv_port > 0) {
            c->svc[i].port = rr->srv_port;
            snprintf(c->svc[i].host, sizeof(c->svc[i].host), "%.159s", rr->text);
        }
        break;
    }
    case MDNS_T_TXT: {
        if (!strstr(rr->name, "_ipp._tcp")) break;
        int i = slot_of(c, rr->name);
        if (i >= 0) txt_merge(&c->svc[i], rr->text);
        break;
    }
    case MDNS_T_A:
        if (rr->text[0]) {
            for (int i = 0; i < c->n; i++)
                if (c->svc[i].host[0] && strcmp(c->svc[i].host, rr->name) == 0 &&
                    !c->svc[i].ip[0])
                    snprintf(c->svc[i].ip, sizeof(c->svc[i].ip), "%.63s", rr->text);
        }
        break;
    default:
        break;
    }
}

/* ---------------- 组播接口选择(真机 ENODEV 修复, NOTES §15 遗留) ----------------
 * 相机多接口(wlan0/rndis0/lo)且默认路由不指向热点时, imr_interface=INADDR_ANY
 * 由内核自行挑口——挑到无组播路由的接口即 ENODEV("No such device")。
 * 修法: SIOCGIFCONF 显式枚举 IPv4 接口(不用 getifaddrs——老 bionic 没有),
 * 按优先级逐个 IP_ADD_MEMBERSHIP:
 *   显式 ifname(-i/调用方指定) > wlan0 > 其它非环回已配置接口 > 环回
 * 加入成功同时设 IP_MULTICAST_IF —— 否则默认路由在 rndis0 时组播查询从
 * rndis0 发出, 热点里的打印机永远收不到。全部失败退回 legacy 单播应答模式,
 * 仅告警不中止(与 WSL 行为一致)。 */
typedef struct {
    char           name[IF_NAMESIZE];
    struct in_addr addr;   /* 该接口 IPv4 地址(network 序) */
    int            score;  /* 越大越优先: 3=显式指定 2=wlan0 1=非环回 0=环回 */
} mdns_ifc_t;

static int enum_v4_ifs(int sock, const char *want, mdns_ifc_t *out, int max)
{
    /* Linux/Android 的 ifreq 定长(sizeof=32, 无 sa_len 变长), 逐条步进 */
    static char buf[4096];
    struct ifconf ifc;
    memset(&ifc, 0, sizeof ifc);
    ifc.ifc_len = (int)sizeof buf;
    ifc.ifc_buf = buf;
    if (ioctl(sock, SIOCGIFCONF, &ifc) != 0) {
        LOGW("SIOCGIFCONF: %s", strerror(errno));
        return 0;
    }
    int n = 0;
    char *end = buf + ifc.ifc_len;
    for (char *p = buf; p + (int)sizeof(struct ifreq) <= end; p += sizeof(struct ifreq)) {
        struct ifreq *r = (struct ifreq *)p;
        struct sockaddr_in *sa = (struct sockaddr_in *)&r->ifr_addr;
        if (sa->sin_family != AF_INET) continue;
        if (sa->sin_addr.s_addr == 0) continue;             /* 未配置地址的口(如 DHCP 前) */
        if (n >= max) break;
        snprintf(out[n].name, sizeof out[n].name, "%s", r->ifr_name);
        out[n].addr = sa->sin_addr;
        out[n].score = !strcmp(r->ifr_name, "wlan0") ? 2
                     : sa->sin_addr.s_addr == htonl(INADDR_LOOPBACK) ? 0 : 1;
        if (want && want[0] && !strcmp(r->ifr_name, want)) out[n].score = 3;
        if (out[n].score >= 2)
            LOGI("mDNS 组播候选接口: %s %s (prio=%d)", out[n].name,
                 inet_ntoa(out[n].addr), out[n].score);
        n++;
    }
    if (want && want[0]) {
        int seen = 0;
        for (int i = 0; i < n; i++) if (!strcmp(out[i].name, want)) seen = 1;
        if (!seen) LOGW("mDNS: 指定接口 %s 无 IPv4 地址(未连网?), 按优先级回落", want);
    }
    return n;
}

/* 按优先级试加组播组; 0=成功(chosen=接口名), -1=全部失败 */
static int join_mcast_group(int fd, const char *want, char *chosen, size_t chosen_cap)
{
    mdns_ifc_t ifs[24];
    int n = enum_v4_ifs(fd, want, ifs, 24);
    for (int i = 1; i < n; i++) {                           /* score 稳定降序(n 很小) */
        mdns_ifc_t t = ifs[i];
        int j = i - 1;
        while (j >= 0 && ifs[j].score < t.score) { ifs[j + 1] = ifs[j]; j--; }
        ifs[j + 1] = t;
    }
    struct ip_mreq mr;
    memset(&mr, 0, sizeof mr);
    mr.imr_multiaddr.s_addr = inet_addr(MDNS_GROUP);
    for (int i = 0; i < n; i++) {
        mr.imr_interface = ifs[i].addr;
        if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof mr) == 0) {
            /* 查询报文也从这个口发(默认路由不在热点网段时的关键一步) */
            if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF,
                           &ifs[i].addr, sizeof(struct in_addr)) != 0)
                LOGW("IP_MULTICAST_IF(%s) 失败: %s(查询可能走错出口)",
                     ifs[i].name, strerror(errno));
            snprintf(chosen, chosen_cap, "%s", ifs[i].name);
            return 0;
        }
        LOGD("mDNS 经 %s 加入组播失败: %s", ifs[i].name, strerror(errno));
    }
    /* 兜底: INADDR_ANY(单接口/默认路由明确的常规环境, 旧版行为) */
    mr.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof mr) == 0) {
        snprintf(chosen, chosen_cap, "any");
        return 0;
    }
    return -1;
}

int mdns_discover_ipp_if(mdns_service_t *out, int max_out, int total_ms, const char *ifname)
{
    if (!out || max_out <= 0) return -1;
    memset(out, 0, (size_t)max_out * sizeof(mdns_service_t));
    if (total_ms <= 0) total_ms = 10000;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { LOGE("socket: %s", strerror(errno)); return -1; }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif

    /* 尽量绑 5353（打印机常把应答发往 5353）；被 avahi 占用时退回临时端口，
     * 此时按 RFC 6762 legacy 查询规则打印机会单播应答到我们的源端口。 */
    struct sockaddr_in la;
    memset(&la, 0, sizeof(la));
    la.sin_family = AF_INET;
    la.sin_addr.s_addr = htonl(INADDR_ANY);
    la.sin_port = htons(MDNS_PORT);
    if (bind(fd, (struct sockaddr *)&la, sizeof(la)) != 0) {
        LOGW("绑定 :5353 失败(%s)，退回临时端口(legacy 单播应答模式)", strerror(errno));
        la.sin_port = 0;
        if (bind(fd, (struct sockaddr *)&la, sizeof(la)) != 0) {
            LOGE("bind: %s", strerror(errno));
            close(fd);
            return -1;
        }
    } else {
        char chosen[IF_NAMESIZE] = "any";
        if (join_mcast_group(fd, ifname, chosen, sizeof chosen) == 0)
            LOGI("mDNS 组播加入成功: 经接口 %s (查询/收包同口)", chosen);
        else
            LOGW("mDNS 组播加入全部失败(%s)——退化 legacy 单播应答模式",
                 strerror(errno));
    }
    int noloop = 0;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &noloop, sizeof(noloop));

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(MDNS_PORT);
    dst.sin_addr.s_addr = inet_addr(MDNS_GROUP);

    col_t col = { out, 0, max_out };
    unsigned char q[512], pktbuf[PKT_MAX];
    uint64_t t0 = now_ms();
    int64_t last_ptr = -10000, last_extra = -10000;

    for (;;) {
        int64_t el = (int64_t)(now_ms() - t0);
        if (el >= total_ms) break;

        /* 每 ~1s 重发 PTR 查询（mDNS 丢包常见） */
        if (el - last_ptr >= 1000) {
            int qlen = make_query(q, sizeof(q), 0, 0, "_ipp._tcp.local", MDNS_T_PTR);
            if (qlen > 0) sendto(fd, q, (size_t)qlen, 0, (struct sockaddr *)&dst, sizeof(dst));
            qlen = make_query(q, sizeof(q), 0, 0, "_universal._sub._ipp._tcp.local", MDNS_T_PTR);
            if (qlen > 0) sendto(fd, q, (size_t)qlen, 0, (struct sockaddr *)&dst, sizeof(dst));
            last_ptr = el;
        }
        /* 对已发现但缺 SRV/TXT/A 的服务定向补查 */
        if (el - last_extra >= 1200) {
            for (int i = 0; i < col.n; i++) {
                mdns_service_t *s = &col.svc[i];
                if (s->port == 0) {
                    int qlen = make_query(q, sizeof(q), 0, 0, s->instance, MDNS_T_SRV);
                    if (qlen > 0) sendto(fd, q, (size_t)qlen, 0, (struct sockaddr *)&dst, sizeof(dst));
                    qlen = make_query(q, sizeof(q), 0, 0, s->instance, MDNS_T_TXT);
                    if (qlen > 0) sendto(fd, q, (size_t)qlen, 0, (struct sockaddr *)&dst, sizeof(dst));
                } else if (s->host[0] && !s->ip[0]) {
                    int qlen = make_query(q, sizeof(q), 0, 0, s->host, MDNS_T_A);
                    if (qlen > 0) sendto(fd, q, (size_t)qlen, 0, (struct sockaddr *)&dst, sizeof(dst));
                }
            }
            last_extra = el;
        }

        int wait = total_ms - (int)el;
        if (wait > 300) wait = 300; /* 小步等待，保证重发计时器及时 */
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(fd, &rf);
        struct timeval tv = { .tv_sec = wait / 1000, .tv_usec = (wait % 1000) * 1000 };
        int r = select(fd + 1, &rf, NULL, NULL, &tv);
        if (r > 0) {
            int n = (int)recv(fd, pktbuf, sizeof(pktbuf), 0);
            if (n >= 12 && mdns_parse_reply(pktbuf, n, col_cb, &col) == 0)
                LOGD("mDNS 应答 %d 字节，已知服务 %d 个", n, col.n);
        }
    }
    close(fd);
    return col.n;
}

int mdns_discover_ipp(mdns_service_t *out, int max_out, int total_ms)
{
    return mdns_discover_ipp_if(out, max_out, total_ms, NULL);
}
