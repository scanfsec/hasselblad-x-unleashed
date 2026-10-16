/* backend.h — 后端接缝: 真实打印核心(阶段2, src/net/)与 mock 后端实现同一张函数表
 * 约定:
 *  - 返回值: 0=受理/成功, 负数=错误码(见 PRN_BE_*)
 *  - 后端通过 state.c 的 prn_set_ 系列与 prn_discovered_ 系列接口推进属性,
 *    notify 链自动广播 propertiesChanged —— 后端不碰 D-Bus
 *  - tick() 由主循环每次迭代调用(mock 用它推进模拟时钟; 真后端可为 NULL)
 *  - photo_list/start_print_job 返回的字符串数组/字符串由调用方 free
 */
#ifndef PRN_BACKEND_H
#define PRN_BACKEND_H

/* 错误码 → adaptor 映射到 com.hasselblad.error.* */
#define PRN_BE_FAILED  (-1)   /* com.hasselblad.error.failed */
#define PRN_BE_PARAM   (-2)   /* com.hasselblad.error.parametererror */
#define PRN_BE_BUSY    (-3)   /* com.hasselblad.error.busy */

/* Start_print_job 选项(v2: 键=IPP 关键字, 值=打印机原生值; 0/NULL=用服务属性当前值)
 * adaptor 从 a{sv} opts 解析; 真后端透传前按能力校验。字符串为借用指针
 * (adaptor 保证调用期间有效)。
 * §18 编辑参数(INTERFACE.md v2.1 §3b, 裁切/旋转; 同样进 Preview_print):
 *   crop_*  归一化 0..1, 定义在**旋转后**的图上(GUI 预览坐标系), 默认 0,0,1,1=全幅;
 *   rotation -1=EXIF 自动(默认), 0/90/180/270 显式覆盖(不叠加 EXIF)。 */
typedef struct prn_job_opts {
    int         copies;          /* ≤0 = 用 copies 属性 */
    const char *media;           /* NULL = 用 media 属性 */
    const char *print_quality;   /* NULL = 用 print_quality 属性; draft|normal|high */
    const char *color_mode;      /* NULL = 用 color_mode 属性 */
    const char *print_scaling;   /* NULL/"" = 不下发(交打印机默认) */
    double      crop_x, crop_y, crop_w, crop_h;   /* 默认 0,0,1,1(全幅) */
    int         rotation;                           /* 默认 -1(EXIF 自动) */
} prn_job_opts_t;

/* doGet_photo_info 返回(源头 imgprep_probe, 不解码像素) */
typedef struct prn_photo_info {
    int  width, height;          /* 存储尺寸(3FR=内嵌 preview 尺寸) */
    int  orientation;            /* EXIF Orientation 1..8, 0=无 */
    int  rotated_width, rotated_height; /* EXIF 折算后逻辑尺寸 */
    int  has_jpeg;               /* 1=文件本身是 JPEG */
    int  is_3fr;                 /* 1=哈苏 RAW(预览/打印走内嵌 preview) */
    long file_size;              /* 文件字节数(3FR=RAW 本体) */
} prn_photo_info_t;

typedef struct prn_backend {
    const char *name;                                        /* "mock" / "real" */
    void (*tick)(void);                                      /* 可 NULL */
    void (*boot_init)(void);                                 /* 可 NULL: 总线就绪后一次性启动钩子
                                                                (§19 崩溃恢复 + P1② 全残留清理) */
    int  (*connect_to_ap)(const char *ssid, const char *psk);/* 受理即返回0, 异步走状态机 */
    /* §20 扫描选择(INTERFACE.md v2.2): Wi-Fi 站点扫描;
     * 仅 disconnected 态可扫(joining/joined/ready/discovering/扫描中 → PRN_BE_BUSY);
     * 元素 "SSID\tRSSI(dBm)\tSEC"(SEC=wpa2|open, 隐藏网络跳过, 同 SSID 去重取最强,
     * 按 RSSI 降序, ≤32 项), 调用方 free; 结果同步进 scanned_networks 属性(notify)。
     * 返回: 0=同步完成(*out 与 *n 填好); **1=异步受理**(真后端——执行层在
     * tick 机推进, adaptor 挂起方法回复至完成, prn_adaptor_scan_complete 补发,
     * 线协议契约不变); 负=PRN_BE_*。 */
    int  (*scan_wifi)(char ***out, int *n);
    int  (*disconnect_restore)(void);
    /* 发现(v2.2 P0 语义): **受理即返** —— 返回 0 且 *out=NULL、*n=0(空 as),
     * L1→L4 级联在 tick 状态机推进(单端点/窗口子进程 + 60s 总预算), 结果经
     * discovered_printers 逐项 notify, 完成=connection_state 离开 discovering。 */
    int  (*start_discovery)(char ***out, int *n);            /* 受理即返; 元素 "名字\tIP" */
    int  (*select_printer)(const char *name);                /* 匹配 discovered 元素或名字部分;
                                                                真后端: 探测能力+格式协商+发布能力属性 */
    int  (*photo_list)(const char *path, char ***out, int *n);/* jpg/jpeg, ≤64 项 */
    int  (*start_print_job)(char **paths, int npaths,
                            const prn_job_opts_t *opts,
                            char **out_job_id);              /* 0=ok(*out_job_id=malloc 串), 负=PRN_BE_* */
    int  (*cancel_job)(void);                                /* 1=已取消 0=无活动任务 */
    /* ---- §18 预览/裁切/旋转(GUI 编辑页) ---- */
    int  (*photo_info)(const char *path, prn_photo_info_t *out);        /* 头部探测 */
    int  (*photo_preview)(const char *path, int max_w, int max_h,
                          char **out_path);                 /* 快速预览(带缓存); *out_path malloc */
    int  (*preview_print)(const char *path, const prn_job_opts_t *opts,
                          char **out_path);                 /* WYSIWYG: 与打印同管线; *out_path malloc */
} prn_backend;

/* 后端失败详情(最近一次 PRN_BE_* 返回的人类解释, 空=无):
 * backend_real 强定义, mock_backend 弱定义兜底; adaptor 错误回复正文带出 */
const char *prn_be_last_error(void)
#if defined(__GNUC__)
__attribute__((weak))
#endif
;

/* mock 后端(永远链接在服务里) */
const prn_backend *prn_backend_mock(void);

/* 真后端(阶段2 src/net/ 提供; 弱符号, 未实现时链接为 NULL → 自动回落 mock) */
const prn_backend *prn_backend_real(void)
#if defined(__GNUC__)
    __attribute__((weak))
#endif
;

#endif
