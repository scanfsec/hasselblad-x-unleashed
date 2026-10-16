/* selftest_img.c — 图片预处理联调 CLI（§11 HQ 管线版 + §18 裁切/旋转）
 *
 * 用法：
 *   selftest_img <in.jpg|.3fr> [out.jpg] [--size WxH] [--q N] [--corners]
 *                [--hq|--fast] [--nocm] [--from-3fr] [--no-sharpen]
 *                [--crop x,y,w,h] [--rotate N]
 *                [--extract-preview <file>]
 *
 * 管线：默认 --hq（两级缩放 Lanczos3 + 缩后锐化 + q95 4:4:4 +
 *       AdobeRGB→sRGB）；--fast 切旧管线（DCT 直达 + 双三次 q92 4:2:2）。
 * --nocm 跳过色彩转换（逃生开关）。
 * 目标尺寸：--size 显式给定（px，调用方按纸型×分辨率计算）；
 *   默认兜底 1200×1800（按 EXIF 方向取 WxH/HxW）。
 * 输入 .3fr（或 --from-3fr）：提取内嵌最大 JPEG preview 喂给管线；
 *   --extract-preview <file> 只把 preview 原始字节落盘（fixture 字节级
 *   校验用），不做图像处理。
 * §18 编辑参数：
 *   --crop x,y,w,h  归一化裁切窗（0..1，逗号分隔），定义在**旋转后**的
 *                   图上（= GUI 预览坐标系）；边界钳制，最小尺寸 100px。
 *   --rotate N      -1=EXIF 自动（默认）；0/90/180/270 显式旋转（顺时针，
 *                   覆盖 EXIF，不叠加）。
 * --no-sharpen 为测试钩子（清晰度对照），不属正式接口。
 *
 * 编译：gcc -std=c11 -O2 -Wall -Wextra -o selftest_img selftest_img.c
 *       imgprep.c 3fr.c -ljpeg -lm
 */
#include "imgprep.h"
#include "3fr.h"

#include <stdio.h> /* jpeglib.h 需要 FILE 先声明 */
#include <jpeglib.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp */
#include <time.h>

/* 解码输出图并打印四角 16×16 平均色（旋转方向验证） */
static int dump_corners(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "无法打开 %s\n", path); return -1; }
    struct jpeg_decompress_struct c;
    struct jpeg_error_mgr jerr;
    c.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&c);
    jpeg_stdio_src(&c, f);
    if (jpeg_read_header(&c, TRUE) != JPEG_HEADER_OK) {
        fprintf(stderr, "输出不是有效 JPEG\n");
        jpeg_destroy_decompress(&c);
        fclose(f);
        return -1;
    }
    jpeg_start_decompress(&c);
    int w = (int)c.output_width, h = (int)c.output_height;
    unsigned char *rgb = malloc((size_t)w * h * 3);
    unsigned char *row = malloc((size_t)w * 3);
    if (!rgb || !row) { free(rgb); free(row); jpeg_destroy_decompress(&c); fclose(f); return -1; }
    while (c.output_scanline < c.output_height) {
        int done = (int)c.output_scanline;
        JSAMPROW rp[1] = { row };
        jpeg_read_scanlines(&c, rp, 1);
        memcpy(rgb + (size_t)done * w * 3, row, (size_t)w * 3);
    }
    jpeg_finish_decompress(&c);
    jpeg_destroy_decompress(&c);
    fclose(f);
    free(row);

    /* 四角 16×16 平均 */
    struct { const char *nm; int x, y; } corners[] = {
        { "左上", 0, 0 }, { "右上", w - 16, 0 },
        { "左下", 0, h - 16 }, { "右下", w - 16, h - 16 },
    };
    printf("corners %dx%d:", w, h);
    for (unsigned i = 0; i < sizeof(corners) / sizeof(corners[0]); i++) {
        long r = 0, g = 0, b = 0;
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
                const unsigned char *p = rgb + ((size_t)(corners[i].y + y) * w +
                                                (corners[i].x + x)) * 3;
                r += p[0]; g += p[1]; b += p[2];
            }
        printf(" %s=(%ld,%ld,%ld)", corners[i].nm, r / 256, g / 256, b / 256);
    }
    printf("\n");
    free(rgb);
    return 0;
}

static double now_sec(void)
{
    return (double)clock() / CLOCKS_PER_SEC;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
                "用法: %s <in.jpg|.3fr> [out.jpg] [--size WxH] [--q N] "
                "[--corners]\n"
                "      [--hq|--fast] [--nocm] [--from-3fr] [--no-sharpen] "
                "[--extract-preview <file>]\n"
                "      [--crop x,y,w,h] [--rotate N]   (§18 编辑参数, 见头注释)\n",
                argv[0]);
        return 1;
    }
    const char *in = argv[1];
    char out[512];
    snprintf(out, sizeof(out), "%s", in);
    char *dot = strrchr(out, '.');
    if (dot) *dot = '\0';
    strncat(out, "_print.jpg", sizeof(out) - strlen(out) - 1);

    int tw = 0, th = 0, q = 0, corners = 0;
    unsigned flags = 0;
    int from_3fr = 0, no_sharpen = 0, mode_seen = 0;
    const char *extract_to = NULL;
    imgprep_edit_t edit;
    memset(&edit, 0, sizeof edit);
    edit.crop_x = 0; edit.crop_y = 0; edit.crop_w = 1; edit.crop_h = 1;
    edit.rotation = -1;
    int edit_given = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &tw, &th) != 2 || tw < 1 || th < 1) {
                fprintf(stderr, "--size 格式: WxH（如 1200x1800）\n");
                return 1;
            }
        } else if (!strcmp(argv[i], "--q") && i + 1 < argc) {
            q = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--corners")) {
            corners = 1;
        } else if (!strcmp(argv[i], "--hq")) {
            flags &= ~(unsigned)IMGPREP_F_FAST;
            mode_seen = 1;
        } else if (!strcmp(argv[i], "--fast")) {
            flags |= IMGPREP_F_FAST;
            mode_seen = 1;
        } else if (!strcmp(argv[i], "--nocm")) {
            flags |= IMGPREP_F_NOCM;
        } else if (!strcmp(argv[i], "--from-3fr")) {
            from_3fr = 1;
        } else if (!strcmp(argv[i], "--no-sharpen")) {
            no_sharpen = 1;
        } else if (!strcmp(argv[i], "--crop") && i + 1 < argc) {
            if (sscanf(argv[++i], "%lf,%lf,%lf,%lf", &edit.crop_x, &edit.crop_y,
                       &edit.crop_w, &edit.crop_h) != 4) {
                fprintf(stderr, "--crop 格式: x,y,w,h（归一化 0..1，如 0.5,0,0.5,1）\n");
                return 1;
            }
            edit_given = 1;
        } else if (!strcmp(argv[i], "--rotate") && i + 1 < argc) {
            edit.rotation = atoi(argv[++i]);
            if (edit.rotation != -1 && edit.rotation != 0 &&
                edit.rotation != 90 && edit.rotation != 180 &&
                edit.rotation != 270) {
                fprintf(stderr, "--rotate 取值: -1(EXIF自动)/0/90/180/270\n");
                return 1;
            }
            edit_given = 1;
        } else if (!strcmp(argv[i], "--extract-preview") && i + 1 < argc) {
            extract_to = argv[++i];
        } else if (i == 2 && !extract_to) {
            snprintf(out, sizeof(out), "%s", argv[i]);
        } else {
            fprintf(stderr, "未知参数: %s\n", argv[i]);
            return 1;
        }
    }
    if (!mode_seen) flags &= ~(unsigned)IMGPREP_F_FAST; /* 默认 HQ */
    if (no_sharpen) flags |= IMGPREP_F_NOSHRP;

    /* .3fr 扩展名自动走 3FR 路径（--from-3fr 可强制） */
    size_t inlen = strlen(in);
    if (inlen >= 5 && strcasecmp(in + inlen - 4, ".3fr") == 0)
        from_3fr = 1;

    if (extract_to) {
        unsigned char *jb = NULL;
        size_t jl = 0;
        int e = h3fr_extract_preview(in, &jb, &jl);
        if (e != H3FR_OK) {
            fprintf(stderr, "3FR preview 提取失败: %s\n", h3fr_strerror(e));
            return 1;
        }
        FILE *f = fopen(extract_to, "wb");
        if (!f || fwrite(jb, 1, jl, f) != jl) {
            fprintf(stderr, "写 %s 失败\n", extract_to);
            free(jb);
            if (f) fclose(f);
            return 1;
        }
        fclose(f);
        printf("3FR preview: %zu 字节 -> %s\n", jl, extract_to);
        free(jb);
        return 0;
    }

    printf("== imgprep(%s): %s -> %s（%s，质量 %d）==\n",
           (flags & IMGPREP_F_FAST) ? "FAST" : "HQ", in, out,
           tw ? "指定尺寸" : "兜底 1200x1800(按方向)",
           q ? q : ((flags & IMGPREP_F_FAST) ? 92 : 95));
    imgprep_report_t r;
    memset(&r, 0, sizeof r);
    double t0 = now_sec();
    const imgprep_edit_t *pe = edit_given ? &edit : NULL;
    int e = from_3fr
        ? imgprep_from_3fr_edit(in, out, tw, th, q, flags, pe, &r)
        : imgprep_prepare_edit(in, out, tw, th, q, flags, pe, &r);
    double dt = now_sec() - t0;
    if (e != IMGPREP_OK) {
        fprintf(stderr, "失败: %s\n", imgprep_strerror(e));
        return 1;
    }
    printf("源图         : %d x %d（EXIF Orientation=%d%s，ColorSpace=%d）\n",
           r.src_w, r.src_h, r.orientation,
           r.orientation ? "" : "（无/默认）", r.exif_colorspace);
    printf("旋转         : 生效 %d°（%s）\n", r.rotation,
           pe && pe->rotation >= 0 ? "显式覆盖 EXIF" : "EXIF 自动");
    printf("管线         : %s，两级缩放 DCT 1/%d + %s\n",
           r.hq ? "HQ" : "FAST", r.dct_den,
           r.hq ? "Lanczos3(a=3,核展宽预滤波)" : "双三次(Catmull-Rom)");
    printf("DCT 解码     : %d x %d → 旋转后 %d x %d\n",
           r.dec_w, r.dec_h, r.orient_w, r.orient_h);
    if (pe)
        printf("用户裁切     : (%d,%d) %d x %d（旋转后坐标系，归一化输入 "
               "%.3f,%.3f,%.3f,%.3f）\n",
               r.ucrop_x, r.ucrop_y, r.ucrop_w, r.ucrop_h,
               pe->crop_x, pe->crop_y, pe->crop_w, pe->crop_h);
    printf("居中适配     : (%d,%d) %d x %d\n", r.crop_x, r.crop_y, r.crop_w, r.crop_h);
    printf("色彩         : %s%s\n",
           r.cm_converted ? "AdobeRGB→sRGB(矩阵)" : "sRGB 直通",
           (flags & IMGPREP_F_NOCM) ? "（--nocm）" : "");
    printf("锐化         : %s\n",
           (r.hq && !(flags & IMGPREP_F_NOSHRP)) ? "unsharp σ=1.0 amount=0.35" : "关");
    if (r.from_3fr) printf("输入         : 3FR 内嵌 preview（%ld 字节）\n", r.src_bytes);
    printf("输出         : %d x %d，%ld 字节（质量 %d，%s，300dpi）\n",
           r.out_w, r.out_h, r.out_bytes,
           q ? q : (r.hq ? 95 : 92), r.hq ? "4:4:4" : "4:2:2");
    printf("耗时         : %.3f s\n", dt);
    if (corners) dump_corners(out);
    printf("-- 完成，打印尺寸 %.1f x %.1f mm（300dpi）\n",
           r.out_w * 25.4 / 300.0, r.out_h * 25.4 / 300.0);
    return 0;
}
