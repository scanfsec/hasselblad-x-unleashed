/* state.h — 服务状态机与属性存储(纯数据层, 不含 D-Bus)
 * 属性表/取值范围与 dbus/INTERFACE.md §2(v2) 一一对应。
 * v2 通用化: paper_size/media_type 等固定枚举删除, media/print_quality/
 * color_mode 改为打印机原生 IPP 关键字字符串; 能力属性(§2b)由 select_printer
 * 探测后经 prn_caps_publish 发布, Set 校验以能力为源。
 * 修改属性必须走 prn_set_*: 命中白名单+能力校验后触发 notify 回调
 * (main.c 安装 → adaptor 发 propertiesChanged)。
 */
#ifndef PRN_STATE_H
#define PRN_STATE_H

#include "ipp.h"                    /* ipp_caps_t(prn_caps_publish 入参, 仅类型无链接) */

#define PRN_MAX_PRINTERS 16   /* discovered_printers 容量 */
#define PRN_MAX_SCAN     32   /* scanned_networks 容量(INTERFACE.md v2.2 §3 Scan_wifi: ≤32 项) */
#define PRN_SLEN         96   /* 字符串属性上限(含\0) */
#define PRN_MAX_LIST     48   /* 能力列表属性容量(supported 系列与 media_size_info/扫描列表) */
#define PRN_LIST_LEN     96   /* 能力列表元素长度上限(兼容 keyword=WxHmm 长关键字) */

/* 属性索引 = GetAll 输出顺序
 * 末尾两项 synced/tx_active 是 odindb-send-printer 兼容面(INTERFACE.md §7b):
 * 来源于补丁槽位(usbhost 等长改名 printer)的固定属性, 只读+notify。 */
enum prn_prop {
    PRN_CONNECTION_STATE = 0,
    PRN_SSID,
    PRN_PRINTER_STATE,
    PRN_PRINTER_NAME,
    PRN_PRINTER_IP,
    PRN_JOB_PROGRESS,
    PRN_JOB_STATUS,
    PRN_COPIES,            /* i 可写: 1..copies_max(caps 未就绪=99) */
    PRN_MEDIA,             /* s 可写: 打印机原生 media 关键字(如 na_index-4x6_4x6in) */
    PRN_PRINT_QUALITY,     /* s 可写: draft|normal|high(RFC 8011 print-quality 关键字) */
    PRN_COLOR_MODE,        /* s 可写: 打印机原生 print-color-mode 关键字 */
    PRN_PRESET,            /* i 可写: 应用层预设(与打印机能力无关) */
    PRN_DOCUMENT_FORMAT,   /* s 只读: 实际使用的 document-format(选中后 image/jpeg) */
    PRN_CAPS_READY,        /* b 只读: 能力探测完成(Select_printer 成功即 true) */
    PRN_COPIES_MAX,        /* i 只读: copies-supported 上限(未探测=99) */
    PRN_SUPPORTED_MEDIA,   /* as 只读: 原生 media 关键字列表 */
    PRN_SUPPORTED_QUALITIES,   /* as 只读: draft/normal/high 子集 */
    PRN_SUPPORTED_COLOR_MODES, /* as 只读 */
    PRN_SUPPORTED_FORMATS,     /* as 只读: document-format-supported */
    PRN_MEDIA_SIZE_INFO,   /* as 只读: 每项 "keyword=WxHmm"(人机两读) */
    PRN_DISCOVERED_PRINTERS,
    PRN_SCANNED_NETWORKS,  /* §20 扫描选择: as 只读 notify, 元素 "SSID\tRSSI(dBm)\tSEC" */
    PRN_SYNCED,            /* odindb 兼容: b, 只读, 初始 true */
    PRN_TX_ACTIVE,         /* odindb 兼容: b, 只读, 初始 false, tx() 执行期间 true */
    PRN_PROP_COUNT
};

/* 属性元信息: type 's'=字符串 'i'=i32 'b'=bool 'A'=as 数组 */
typedef struct prn_prop_info {
    const char *name;
    char        type;
    int         writable;
    const char *desc;
} prn_prop_info;

const prn_prop_info *prn_prop_info_get(int idx); /* idx 越界返回 NULL */
int prn_prop_index(const char *name);             /* 未知返回 -1 */

/* 修改通知回调(idx=变更属性) */
typedef void (*prn_notify_fn)(int idx, void *user);
void prn_state_set_notify(prn_notify_fn fn, void *user);
void prn_prop_changed(int idx); /* 直接改完字段后手动通知(mock 后端内部推进用) */

/* 字符串列表(能力属性存储; 定长二维, 避免链式分配) */
typedef struct {
    char items[PRN_MAX_LIST][PRN_LIST_LEN];
    int  n;
} prn_strlist_t;

typedef struct prn_state {
    char connection_state[PRN_SLEN]; /* disconnected|joining|joined|discovering|ready */
    char ssid[PRN_SLEN];
    char printer_state[PRN_SLEN];    /* idle|busy|error */
    char printer_name[PRN_SLEN];
    char printer_ip[PRN_SLEN];
    int  job_progress;               /* 0..100 */
    char job_status[PRN_SLEN];       /* idle|preparing|sending|printing|done|failed|canceled */
    int  copies;                     /* 1..copies_max */
    char media[PRN_SLEN];            /* 原生 media 关键字(探测后=打印机默认) */
    char print_quality[PRN_SLEN];    /* draft|normal|high */
    char color_mode[PRN_SLEN];       /* 原生 print-color-mode 关键字 */
    int  preset;                     /* 0 默认 1 证件照 2 拍立得(应用层) */
    char document_format[PRN_SLEN];  /* 实际下发 document-format */
    int  caps_ready;                 /* 能力探测完成 */
    int  copies_max;                 /* 未探测=99 */
    prn_strlist_t sup_media;         /* supported_media */
    prn_strlist_t sup_qualities;
    prn_strlist_t sup_color_modes;
    prn_strlist_t sup_formats;
    prn_strlist_t media_size_info;   /* "keyword=WxHmm" */
    char *discovered[PRN_MAX_PRINTERS]; /* 元素 "名字\tIP" */
    int  discovered_n;
    prn_strlist_t scanned;          /* §20: 最近一次 Scan_wifi 结果(≤PRN_MAX_SCAN) */
    int  synced;                    /* odindb 兼容(§7b): 1=true */
    int  tx_active;                 /* odindb 兼容(§7b): tx() 执行期间 1 */
} prn_state;

prn_state *prn_state_get(void);     /* 单例, 已初始化为默认值 */

/* Set 入口(带校验+notify): 0=成功, -1=未知/不可写, -2=值非法 */
int prn_set_string(int idx, const char *val);
int prn_set_int(int idx, int val);
/* Get 便捷函数 */
const char *prn_get_string(int idx);
int         prn_get_int(int idx);
const prn_strlist_t *prn_get_list(int idx); /* 非 'A' 属性返回空表(NULL 不返回) */

/* ---- 能力发布(backend_real Select_printer 探测成功后调用; 逐属性 notify) ----
 * 内容: 能力列表/上限回填 + 打印机默认值归一(media/print_quality/color_mode)
 * + caps_ready=1 + document_format; copies 超限自动钳到上限。 */
void prn_caps_publish(const ipp_caps_t *caps, const char *document_format);
/* 能力失效(断开/重选失败): caps_ready=0, 清空能力列表, copies_max 回 99 */
void prn_caps_invalidate(void);

/* 列表成员查询(caps_ready 校验/Set 透传用): 1=命中 */
int prn_list_contains(const prn_strlist_t *l, const char *v);

/* discovered_printers 数组操作(自动 notify) */
void prn_discovered_clear(void);
int  prn_discovered_add(const char *name_ip); /* -1=满 */

/* scanned_networks 列表操作(§20, 自动 notify; 元素 "SSID\tRSSI(dBm)\tSEC") */
void prn_scanned_clear(void);
int  prn_scanned_add(const char *ssid_rssi_sec); /* -1=满/超长 */

/* 状态机便捷跳转(带 notify); 值不校验, 仅限服务内部使用 */
void prn_set_connection_state(const char *v);
void prn_set_printer_state(const char *v);
void prn_set_job_status(const char *v);
void prn_set_printer_name(const char *v);
void prn_set_printer_ip(const char *v);
void prn_set_job_progress(int v);

/* odindb 兼容属性(§7b)便捷翻转(带 notify; 值规整为 0/1) */
void prn_set_synced(int v);
void prn_set_tx_active(int v);

/* 任务是否进行中(preparing/sending/printing) */
int prn_job_active(void);
/* connection_state 便捷判断 */
int prn_state_is(const char *connection_state);
int prn_job_status_is(const char *job_status);

#endif
