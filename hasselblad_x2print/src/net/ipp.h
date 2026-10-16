/* ipp.h — 最小 IPP 客户端 + HTTP/1.1 封装（RFC 8010/8011，手写编解码，不依赖 libcups）
 *
 * 通用 IPP 打印机客户端（能力一切自探测，零硬编码打印机属性）：
 *   端点默认 ipp://<ip>:631/ipp/print（mDNS TXT rp= 或 ipp_set_path 可覆盖路径），
 *   明文 HTTP，无鉴权。
 *
 * 四个核心操作：
 *   ipp_get_printer_attributes → 能力（纸张+物理尺寸/格式/色彩模式/质量/份数/分辨率/缩放）
 *   ipp_print_jpeg             → Print-Job 发送 JPEG，返回 job_id
 *   ipp_get_job_attributes     → 轮询任务状态
 *   ipp_cancel_job             → Cancel-Job(0x0008) 取消远端任务
 */
#ifndef X2P_IPP_H
#define X2P_IPP_H

#include <stddef.h>
#include <string.h> /* 头内联换算辅助用 */

#ifdef __cplusplus
extern "C" {
#endif

#define IPP_DEF_PORT 631
#define IPP_DEF_PATH "/ipp/print"
#define IPP_MAX_FORMATS 24
#define IPP_MAX_MEDIA   48
#define IPP_MAX_MODES   16
#define IPP_MAX_QUALS   8
#define IPP_MAX_SCAL    8

/* ---- 错误码（全部 API 返回 0=成功，其余见此枚举） ---- */
enum {
    IPP_OK          = 0,
    IPP_E_ARG       = -100, /* 参数非法 */
    IPP_E_SOCKET    = -101, /* socket 创建失败 */
    IPP_E_CONNECT   = -102, /* 连接失败/超时 */
    IPP_E_SEND      = -103, /* 发送失败 */
    IPP_E_TIMEOUT   = -104, /* 响应超时 */
    IPP_E_TRUNC     = -105, /* 响应不完整 */
    IPP_E_HTTP      = -106, /* HTTP 非 200 / 应答畸形 */
    IPP_E_PARSE     = -107, /* IPP 报文解析失败 */
    IPP_E_STATUS    = -108, /* IPP 状态码非 ok（见 ipp_last_status） */
    IPP_E_NOMEM     = -109, /* 内存不足 */
};
const char *ipp_strerror(int err);
unsigned int ipp_last_status(void); /* IPP_E_STATUS 时的 IPP status-code（RFC 8011 §4.1.8/§4.2）*/
const char *ipp_last_detail(void);  /* 人类可读错误细节（HTTP 状态行/畸形位置等） */

/* ---- 打印机能力(v2: 通用化, 一切自探测) ---- */
typedef struct {
    char document_formats[IPP_MAX_FORMATS][48]; /* document-format-supported */
    int  n_formats;
    char media_supported[IPP_MAX_MEDIA][64];    /* media-supported + media-col 集合提炼 */
    int  n_media;
    int  media_native[IPP_MAX_MEDIA];           /* 1=打印机原生关键字(可直接下发 media) */
    int  media_x_um[IPP_MAX_MEDIA];             /* 物理宽 µm(media-col x-dimension×10; 0=未知) */
    int  media_y_um[IPP_MAX_MEDIA];             /* 物理高 µm(同上) */
    char print_color_modes[IPP_MAX_MODES][48];  /* print-color-mode-supported */
    int  n_modes;
    int  qualities[IPP_MAX_QUALS];              /* print-quality-supported: 3草稿/4正常/5高 */
    int  n_quals;
    int  quality_default;                       /* print-quality-default 枚举(0=未通告) */
    char color_mode_default[48];                /* print-color-mode-default(空=未通告) */
    char print_scalings[IPP_MAX_SCAL][32];      /* print-scaling-supported */
    int  n_scalings;
    char print_scaling_default[32];             /* print-scaling-default */
    int  copies_min, copies_max;                /* copies-supported(rangeOfInteger; 默认 1,1=未通告) */
    int  res_default_x, res_default_y;          /* printer-resolution-default dpi(0=未通告) */
    int  jpeg_max_w, jpeg_max_h;                /* 派生: 最大 media×默认分辨率的像素上限(finish 填) */
    int  printer_state;                         /* 3=idle 4=processing 5=stopped */
    char printer_state_reasons[128];
    char printer_name[96];
    char printer_make_and_model[128];
    char media_default[64];
    /* 内部状态（media-col 集合提炼用），调用方勿动 */
    int _lx, _ly, _szname;
} ipp_caps_t;

/* ---- 打印任务状态 ---- */
typedef struct {
    int  job_id;
    int  job_state;               /* 3 pending / 5 processing / 7 canceled / 8 aborted / 9 completed */
    char state_reasons[128];      /* job-state-reasons（空格分隔多值） */
} ipp_job_t;

/* ---- Print-Job 选项（字段为 0/NULL 表示不发送该属性，交打印机默认） ---- */
typedef struct {
    int         copies;           /* 默认 1 */
    const char *color_mode;       /* print-color-mode 原生关键字, 如 "color"/"monochrome"/"auto" */
    const char *paper;            /* media 原生关键字(取自能力探测) */
    int         quality;          /* print-quality 枚举 3 draft / 4 normal / 5 high */
    const char *print_scaling;    /* print-scaling 原生关键字(如 "fit"/"auto") */
    const char *job_name;         /* 默认 "x2print" */
    const char *document_format;  /* 默认 "image/jpeg" */
} ipp_print_opts_t;

/* ---- 对外 API（ip 为点分 IPv4；port 传 0 = 631） ---- */
int ipp_get_printer_attributes(const char *ip, int port, ipp_caps_t *caps);
int ipp_print_jpeg(const char *ip, int port, const unsigned char *jpeg, size_t len,
                   const ipp_print_opts_t *opts, int *job_id);
int ipp_get_job_attributes(const char *ip, int port, int job_id, ipp_job_t *job);

/* ---- Cancel-Job(RFC 8011 operation 0x0008) ----
 * 操作目标走 job-id 整数属性(与 Get-Job-Attributes 同法, printer-uri 仍必填);
 * 返回 IPP_OK=0x0000 successful-ok; 任务不存在/已终态等 4xx/5xx → IPP_E_STATUS
 * (具体码见 ipp_last_status, 细节见 ipp_last_detail)。 */
int ipp_cancel_job(const char *ip, int port, int job_id);

/* ---- 端点路径覆盖(mDNS TXT rp= / 非 /ipp/print 打印机用) ----
 * path 传 NULL 恢复默认 IPP_DEF_PATH; 影响 HTTP POST 路径与 printer-uri。 */
void ipp_set_path(const char *path);
const char *ipp_get_path(void);            /* 当前生效路径(静态串) */

/* ---- 超时参数面(调用方按场景收紧; P0 黑洞端点快速失败) ----
 * 影响后续所有事务: connect_ms=TCP 连接超时, recv_ms=SO_RCVTIMEO(单次读)。
 * 缺省 connect=3000/recv=20000(ms); 传 0/负值恢复缺省。
 * 服务层用法(NOTES §22): 探测/轮询走紧档(3000,5000), 取消环走快档(2000,2000),
 * Print-Job 数据面保持缺省(recv 20s——大任务应答生成可慢)。 */
void ipp_set_timeouts(int connect_ms, int recv_ms);

/* ---- 能力派生/换算辅助 ---- */
/* 探测完成后收尾: 由 media 尺寸×默认分辨率派生 jpeg_max_w/h。
 * ipp_get_printer_attributes 已自动调用; raw 回调自行 feed 的调用方需手动调。 */
void ipp_caps_finish(ipp_caps_t *caps);
/* 按 media 原生关键字算目标像素: 尺寸(µm)×printer-resolution-default(缺失兜底 300dpi)。
 * 返回 0=ok; -1=关键字不在能力列表; -2=该 media 无尺寸信息。 */
int  ipp_media_target_px(const ipp_caps_t *caps, const char *media, int *w, int *h);

/* print-quality 枚举↔RFC 8011 关键字名(头内联, 无链接依赖) */
static inline const char *ipp_quality_kw(int q)
{
    return q == 3 ? "draft" : q == 4 ? "normal" : q == 5 ? "high" : "";
}
static inline int ipp_quality_enum(const char *kw)
{
    if (!kw) return 0;
    if (!strcmp(kw, "draft"))  return 3;
    if (!strcmp(kw, "normal")) return 4;
    if (!strcmp(kw, "high"))   return 5;
    return 0;
}

/* ---- 通用属性回调（--probe 全量打印用） ----
 * 每个属性值回调一次；IPP 1.1 集合(beginCollection 0x34)成员被摊平为独立
 * 属性名（memberAttrName 0x4A 的值），对人工阅读友好。
 * 参数: group_tag 0x01/0x02/0x04，value_tag RFC 8010 表 3，name 为有效属性名，
 * val/vlen 为值字节。 */
typedef void (*ipp_attr_cb)(void *ud, int group_tag, int value_tag,
                            const char *name, const unsigned char *val, int vlen);

/* 拉取全部打印机属性并逐值回调（probe 工具用）；返回 IPP_OK 或错误码 */
int ipp_get_printer_attributes_raw(const char *ip, int port, ipp_attr_cb cb, void *ud);

/* 能力提炼器：把单个属性值喂进 ipp_caps_t（配合 raw 回调可边打印边提炼，
 * 一次网络往返同时得到全量清单与结构化能力） */
void ipp_caps_feed(ipp_caps_t *caps, int group, int tag, const char *name,
                   const unsigned char *val, int vlen);

/* ---- 展示辅助（selftest 用） ---- */
const char *ipp_tag_name(int tag);       /* 值标签 → 名称 */
const char *ipp_group_name(int g);       /* 组标签 → 名称 */
const char *ipp_printer_state_name(int s);
const char *ipp_job_state_name(int s);

#ifdef __cplusplus
}
#endif
#endif /* X2P_IPP_H */
