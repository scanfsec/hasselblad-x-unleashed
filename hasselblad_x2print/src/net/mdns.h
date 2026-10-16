/* mdns.h — 最小 mDNS/DNS-SD 查询客户端（纯 POSIX socket，无 avahi 依赖）
 *
 * 用途：在打印机热点网络里发现 IPP 打印服务（AirPrint 打印机会应答
 *       _ipp._tcp.local 的 PTR/SRV/TXT/A 记录）。
 *
 * 同一套 DNS 报文编解码也可单播查询公网 DNS（mdns_query_unicast）——
 * 在组播不可用的环境（WSL/公司网）用它验证编解码正确性。
 */
#ifndef X2P_MDNS_H
#define X2P_MDNS_H

#ifdef __cplusplus
extern "C" {
#endif

#define MDNS_TXT_MAX 896 /* TXT 键值对拼接上限 */

typedef struct {
    char instance[160];   /* 服务实例名（PTR rdata，含 ._ipp._tcp.local 后缀） */
    char host[160];       /* SRV target 主机名（通常为 xxxx.local） */
    char ip[64];          /* A 记录 IPv4 点分串 */
    int  port;            /* SRV 端口（IPP 通常 631） */
    char txt[MDNS_TXT_MAX]; /* TXT 记录 "k=v;k=v" 拼接（含 rp=/ty=/URF 等 AirPrint 键） */
    int  universal;       /* 1 = 经 _universal._sub._ipp._tcp 应答（AirPrint 特征） */
} mdns_service_t;

/* 组播发现 _ipp._tcp.local（同时查询 _universal._sub._ipp._tcp.local 子类型）。
 * total_ms：总扫描时长（建议 10000）。期间每秒重发查询，并对已发现但缺
 * SRV/A 的实例发送定向补充查询。返回写入 out 的条数（0=没发现），<0=严重错误。
 * 接口选择（ENODEV 修复）：优先级 = 显式 ifname > wlan0 > 其它非环回 > 环回，
 * 加入组播组成功后同步设 IP_MULTICAST_IF（查询/收包同口）。 */
int mdns_discover_ipp(mdns_service_t *out, int max_out, int total_ms);

/* 同上, ifname 显式指定组播出口接口（NULL=自动按优先级挑, 一般传 "wlan0"） */
int mdns_discover_ipp_if(mdns_service_t *out, int max_out, int total_ms, const char *ifname);

/* 单播 DNS 查询（联调/编解码单元自测用，如 server="8.8.8.8", port=53）。
 * qtype: 1=A 28=AAAA 12=PTR 33=SRV 16=TXT。
 * 返回应答长度（>0 成功），<0 错误码：-1 参数/网络，-2 超时，-3 应答过长截断。 */
int mdns_query_unicast(const char *server_ip, int port, const char *qname,
                       int qtype, unsigned char *ans, int ans_max, int timeout_ms);

/* ---------- DNS 报文解析（组播/单播应答通用） ---------- */

typedef struct {
    const char *name;              /* RR 的 NAME（已解压缩，如 "_ipp._tcp.local"） */
    int  type;                     /* RR TYPE：1/28/12/33/16/5... */
    int  ttl;
    const unsigned char *rdata;    /* 原始 RDATA */
    int  rlen;
    char text[256];                /* 文本化：A/AAAA→IP，PTR/CNAME/NS/SRV→名字，TXT→k=v;... */
    int  srv_port;                 /* SRV 专用：解出的端口 */
} mdns_rr_t;

/* 逐条回调应答里 answer/authority/additional 的资源记录；返回 0=解析成功，
 * <0=报文畸形。cb 可为 NULL。 */
typedef void (*mdns_rr_cb)(void *ud, const mdns_rr_t *rr);
int mdns_parse_reply(const unsigned char *pkt, int len, mdns_rr_cb cb, void *ud);

/* 常量 */
#define MDNS_T_A     1
#define MDNS_T_PTR   12
#define MDNS_T_TXT   16
#define MDNS_T_AAAA  28
#define MDNS_T_SRV   33
#define MDNS_T_CNAME 5

#ifdef __cplusplus
}
#endif
#endif /* X2P_MDNS_H */
