#include "scaler.h"

#include <stdlib.h>

#define NES_W 256
#define NES_H 240

static int *rowmap; /* dst_y -> src_y */
static int *colmap; /* dst_x -> src_x */
static int DW, DH;
static uint32_t *tmp; /* optional pillarbox/letterbox background */

void scaler_init(int dst_w, int dst_h) {
    DW = dst_w;
    DH = dst_h;
    rowmap = malloc(sizeof(int) * DH);
    colmap = malloc(sizeof(int) * DW);
    for (int y = 0; y < DH; y++) rowmap[y] = y * NES_H / DH;
    for (int x = 0; x < DW; x++) colmap[x] = x * NES_W / DW;
    tmp = NULL;
}

void scaler_blit(uint32_t *dst, const uint32_t *src) {
    for (int y = 0; y < DH; y++) {
        const uint32_t *s = src + (size_t)rowmap[y] * NES_W;
        uint32_t *d = dst + (size_t)y * DW;
        for (int x = 0; x < DW; x++) {
            d[x] = s[colmap[x]] | 0xff000000u;
        }
    }
}
