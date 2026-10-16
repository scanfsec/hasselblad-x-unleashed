/* imgprep.h — 高质量打印图像预处理（HQ 管线 + 3FR 支持）
 *
 * 目标：把任意 JPEG / 哈苏 .3FR RAW（内嵌 preview）转成打印机适用的
 *       目标尺寸 JPEG。本程序为通用打印机程序：目标尺寸一律由调用方
 *       按探测到的纸型物理尺寸 × 打印机分辨率显式传入（px）；
 *       传 0,0 时仅作兜底（1200×1800 / 1800×1200，按 EXIF 方向）。
 *
 * HQ 管线（默认，阶段 §11）：
 *   1) libjpeg-turbo DCT 缩放解码，档位 = 「不小于 2× 目标的最小档」
 *      （两级缩放第一级，避免 100MP 全尺寸展开）
 *   2) EXIF Orientation 1-8 旋转 → 按目标宽高比居中裁切
 *   3) Lanczos3（a=3，核随缩放因子展宽 = 预滤波，边界 clamp）缩到
 *      精确目标尺寸（两级缩放第二级）
 *   4) unsharp mask 锐化（高斯 σ=1.0px@300dpi，amount 0.35，阈值 0）
 *   5) 色彩：EXIF ColorSpace=1 → sRGB 直通；=2/未标且含 AdobeRGB 特征
 *      （ICC "Adobe RGB" / APP14 Adobe）→ AdobeRGB→sRGB 矩阵转换
 *   6) JPEG 编码：默认 q95、4:4:4（无色度抽样）、300dpi 元数据
 *
 * FAST 管线（旧版兼容）：DCT 档位 ≥ 目标即止 + Catmull-Rom 双三次、
 *   默认 q92、4:2:2、无锐化、无色彩转换。
 *
 * 自由裁切/旋转（§18，GUI 编辑页）：
 *   管线顺序定死：解码 → 旋转（EXIF 自动 或 显式 0/90/180/270 覆盖）→
 *   裁切（归一化矩形，定义在**旋转后**的图上 = GUI 预览坐标系）→
 *   纸型比例居中适配 → 缩放 → 锐化 → 编码。
 *   显式 rotation 覆盖 EXIF（不叠加）；裁切做边界钳制与最小尺寸校验
 *   （裁切窗两维像素 < IMGPREP_MIN_CROP_PX 时回 IMGPREP_E_ARG）。
 */
#ifndef X2P_IMGPREP_H
#define X2P_IMGPREP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 无能力信息时的默认兜底目标（4x6in @ 300dpi；按方向取 WxH 或 HxW） */
#define IMGPREP_FALLBACK_W 1200
#define IMGPREP_FALLBACK_H 1800

/* 管线开关（imgprep_prepare_ex 的 flags 位） */
#define IMGPREP_F_FAST 1u   /* 旧管线：DCT 直达 + 双三次 + q92 4:2:2，无锐化/CM */
#define IMGPREP_F_NOCM 2u   /* HQ 下跳过 AdobeRGB→sRGB（逃生开关） */
/* 注：imgprep 内部测试钩子 IMGPREP_F_NOSHRP（4u）= HQ 下关锐化，供清晰度
 * 对照测试用，不属公开接口语义。 */
#define IMGPREP_F_NOSHRP 4u

/* 用户裁切/旋转编辑参数（imgprep_edit_t；NULL = 无编辑 = 全幅 + EXIF 自动）。
 * 坐标系契约（GUI 编辑页照此实现，INTERFACE.md v2.1 §3b）：
 *   - crop 四值归一化 0..1，定义在**旋转后**的图上（= GUI 预览所见画面）；
 *     x/y 为左上角原点，w/h 为宽高占比；默认/全幅 = 0,0,1,1。
 *   - rotation：-1 = EXIF 自动（现行为）；0/90/180/270 = 显式旋转
 *     （顺时针，**覆盖** EXIF Orientation，不叠加）。
 *   - 边界钳制：x+w>1 → w=1-x（y 同理），x/y 负值钳到 0；
 *     钳制后裁切窗任一维源像素 < IMGPREP_MIN_CROP_PX → IMGPREP_E_ARG。 */
/* imgprep_edit 归一化（供调用方在解码前先做范围校验/钳制；不解码）。
 * 返回 0=合法（结构体原地钳制到边界内），IMGPREP_E_ARG=非法
 * （NaN/Inf、w/h≤0、钳制后全零窗等）。 */
#define IMGPREP_MIN_CROP_PX 100

typedef struct {
    double crop_x, crop_y, crop_w, crop_h;   /* 归一化裁切窗（旋转后坐标系） */
    int     rotation;                        /* -1=EXIF 自动; 0/90/180/270 显式 */
} imgprep_edit_t;

int imgprep_edit_validate(imgprep_edit_t *e, int rotated_w, int rotated_h);

/* 处理报告（selftest 打印、service 日志用）。字段追加只增不删。 */
typedef struct {
    int src_w, src_h;         /* 源图尺寸 */
    int orientation;          /* EXIF Orientation 1..8，0=无 EXIF */
    int orient_w, orient_h;   /* EXIF 旋转后的逻辑尺寸 */
    int dct_num, dct_den;     /* DCT 缩放解码系数（如 1/4） */
    int dec_w, dec_h;         /* DCT 解码实际尺寸（未旋转） */
    int crop_x, crop_y, crop_w, crop_h; /* 居中裁切窗（旋转后坐标系） */
    int out_w, out_h;         /* 输出尺寸 */
    long out_bytes;           /* 输出 JPEG 字节数 */
    /* §11 追加 */
    int hq;                   /* 1=HQ 管线（Lanczos3+锐化+q95 4:4:4） */
    int exif_colorspace;      /* EXIF ColorSpace：1=sRGB 2=未标定 0=无该 tag */
    int cm_converted;         /* 1=执行了 AdobeRGB→sRGB */
    int from_3fr;             /* 1=输入经 3FR preview 提取 */
    long src_bytes;           /* 输入 JPEG/preview 字节数 */
    /* §18 追加（自由裁切/旋转） */
    int rotation;             /* 生效旋转角（度）：0/90/180/270（EXIF 自动时折算） */
    int ucrop_x, ucrop_y;     /* 用户裁切窗（旋转后像素坐标；未裁=0,0） */
    int ucrop_w, ucrop_h;     /* 用户裁切窗尺寸（未裁=全幅） */
} imgprep_report_t;

enum {
    IMGPREP_OK      = 0,
    IMGPREP_E_IO    = -1,  /* 文件打开/读写失败 */
    IMGPREP_E_DECODE= -2,  /* 非法 JPEG / 解码错误 */
    IMGPREP_E_MEM   = -3,  /* 内存不足 */
    IMGPREP_E_ENCODE= -4,  /* JPEG 编码错误 */
    IMGPREP_E_ARG   = -5,  /* 参数非法 */
    IMGPREP_E_SRC   = -6,  /* 输入源不可用（3FR 无 preview 等） */
};

const char *imgprep_strerror(int err);

/* 兼容入口（阶段2 接口不变）：目标尺寸显式传入；0,0 = 兜底 1200×1800 系。
 * 质量传 0 = 默认（HQ:95 / FAST:92）。rep 可为 NULL。 */
int imgprep_prepare(const char *in_path, const char *out_path,
                    int target_w, int target_h, int quality,
                    imgprep_report_t *rep);

/* 带管线开关的入口（flags 见 IMGPREP_F_*）。 */
int imgprep_prepare_ex(const char *in_path, const char *out_path,
                       int target_w, int target_h, int quality,
                       unsigned flags, imgprep_report_t *rep);

/* 内存 JPEG 源（3FR preview 提取后复用整条管线）。jpeg 生命周期仅限调用内。 */
int imgprep_prepare_mem(const unsigned char *jpeg, size_t jpeg_len,
                        const char *out_path, int target_w, int target_h,
                        int quality, unsigned flags, imgprep_report_t *rep);

/* 哈苏 .3FR RAW：提取内嵌最大 JPEG preview（3fr.c）→ 走 prepare_mem 管线。
 * 无 preview / 非 TIFF 返回 IMGPREP_E_SRC。 */
int imgprep_from_3fr(const char *path, const char *out_path,
                     int target_w, int target_h, int quality,
                     unsigned flags, imgprep_report_t *rep);

/* ---------------- §18 带编辑参数（裁切/旋转）的入口 ----------------
 * 语义见 imgprep_edit_t 注释；edit=NULL 等价上面同名旧入口（全幅+EXIF 自动）。 */
int imgprep_prepare_edit(const char *in_path, const char *out_path,
                         int target_w, int target_h, int quality,
                         unsigned flags, const imgprep_edit_t *edit,
                         imgprep_report_t *rep);
int imgprep_prepare_mem_edit(const unsigned char *jpeg, size_t jpeg_len,
                             const char *out_path, int target_w, int target_h,
                             int quality, unsigned flags,
                             const imgprep_edit_t *edit, imgprep_report_t *rep);
int imgprep_from_3fr_edit(const char *path, const char *out_path,
                          int target_w, int target_h, int quality,
                          unsigned flags, const imgprep_edit_t *edit,
                          imgprep_report_t *rep);

/* ---------------- 头部探测（不解码像素；GUI doGet_photo_info 之源） ----------------
 * .3fr/.3FR：提取内嵌 preview 后按其头部填 width/height/orientation，
 * is_3fr=1（has_jpeg=0，file_size=RAW 文件本体大小）。
 * JPEG：has_jpeg=1，is_3fr=0。rotated_* = 按 EXIF Orientation 折算后的
 * 逻辑尺寸（GUI 显式旋转时自行交换宽高）。失败返回 IMGPREP_E_IO/E_DECODE/E_SRC。 */
typedef struct {
    int  width, height;          /* 存储尺寸（3FR=内嵌 preview 尺寸） */
    int  orientation;            /* EXIF Orientation 1..8，0=无 EXIF */
    int  rotated_width, rotated_height; /* EXIF 折算后逻辑尺寸 */
    int  has_jpeg;               /* 1=文件本身是 JPEG（可直接解码） */
    int  is_3fr;                 /* 1=哈苏 RAW（走内嵌 preview） */
    long file_size;              /* 文件字节数（3FR=RAW 本体） */
} imgprep_info_t;

int imgprep_probe(const char *path, imgprep_info_t *info);

#ifdef __cplusplus
}
#endif
#endif /* X2P_IMGPREP_H */
