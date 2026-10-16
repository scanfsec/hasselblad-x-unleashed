/* heif_thumb.c — HEIC/HEIF → JPEG 提取子进程(x2print §26, 上游 libheif 版)
 * 上游 API, 无系统库 ABI 依赖: read → 缩略图(无则主图) → heif_decode_image(RGB)
 * → 静态 libjpeg q92 编码 → stdout。链: libheif.a(内置 de265 HEVC 解码) + libde265.a
 * + libjpeg.a。内存: 缩略图 3888×2918 RGB ≈ 34MB(远低于 217MB 上限); 主图 HEVC
 * 软解有 60s alarm 预算, 服务端 child_run 预算+SIGKILL 兜底。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <jpeglib.h>
#include <libheif/heif.h>

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: heif_thumb input.heic > out.jpg\n"); return 2; }
    alarm(60);

    struct heif_context *ctx = heif_context_alloc();
    if (!ctx) { fprintf(stderr, "ctx alloc\n"); return 3; }
    struct heif_error e = heif_context_read_from_file(ctx, argv[1], NULL);
    if (e.code) { fprintf(stderr, "read: %s\n", e.message); return 4; }

    struct heif_image_handle *h = NULL;
    e = heif_context_get_primary_image_handle(ctx, &h);
    if (e.code) { fprintf(stderr, "primary: %s\n", e.message); return 4; }

    /* 优先内嵌缩略图(同 3FR preview 源, 打印质量足够且秒级); 无则主图(HEVC 软解) */
    heif_item_id tids[4];
    int nt = heif_image_handle_get_number_of_thumbnails(h);
    if (nt > 0) {
        heif_image_handle *th = NULL;
        int got = heif_image_handle_get_list_of_thumbnail_IDs(h, tids, 4);
        if (got > 0 && (e = heif_image_handle_get_thumbnail(h, tids[0], &th)).code == 0) {
            heif_image_handle_release(h);
            h = th;
        } else if (got > 0) {
            fprintf(stderr, "thumbnail get: %s\n", e.message);
        }
    }

    struct heif_image *img = NULL;
    e = heif_decode_image(h, &img, heif_colorspace_RGB, heif_chroma_interleaved_RGB, NULL);
    heif_image_handle_release(h);
    if (e.code) { fprintf(stderr, "decode: %s\n", e.message); return 5; }

    int stride = 0, w = heif_image_get_primary_width(img), hgt = heif_image_get_primary_height(img);
    const unsigned char *px = heif_image_get_plane_readonly(img, heif_channel_interleaved, &stride);
    if (!px || w <= 0 || hgt <= 0) { fprintf(stderr, "plane\n"); return 6; }

    struct jpeg_compress_struct jc;
    struct jpeg_error_mgr jerr;
    jc.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&jc);
    jpeg_stdio_dest(&jc, stdout);
    jc.image_width = (JDIMENSION)w;
    jc.image_height = (JDIMENSION)hgt;
    jc.input_components = 3;
    jc.in_color_space = JCS_RGB;
    jpeg_set_defaults(&jc);
    jpeg_set_quality(&jc, 92, TRUE);
    jpeg_start_compress(&jc, TRUE);
    for (int y = 0; y < hgt; y++) {
        const unsigned char *row = px + (size_t)y * stride;
        jpeg_write_scanlines(&jc, (JSAMPROW *)&row, 1);
    }
    jpeg_finish_compress(&jc);
    jpeg_destroy_compress(&jc);
    heif_image_release(img);
    heif_context_free(ctx);
    return 0;
}
