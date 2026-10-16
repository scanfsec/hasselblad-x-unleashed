#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "liveview_bridge.c"
int main(void)
{
    uint8_t *shared = calloc(1, LV_FILE_BYTES);
    uint8_t source[24] = {10, 20, 30, 40, 50, 60, 70, 80,
                          90, 100, 110, 120, 130, 140, 150, 160,
                          101, 201, 102, 202, 103, 203, 104, 204};
    struct liveview_header *h = (void *)shared;
    assert(liveview_publish(shared, source, sizeof source, 4, 4, 4, 16, 77) == 0);
    assert(h->sequence == 2 && h->frame_id == 77);
    uint8_t *p = shared + LV_OFFSET;
    assert(p[0] == 10 && p[1] == 101 && p[2] == 10 && p[3] == 201);
    assert(p[LV_BYTES - 4] == 160 && p[LV_BYTES - 3] == 104);
    assert(p[LV_BYTES - 2] == 160 && p[LV_BYTES - 1] == 204);
    h->sequence = 3;
    assert(liveview_publish(shared, source, sizeof source, 4, 4, 4, 16, 88) == 0);
    assert(h->sequence == 4 && h->frame_id == 88);
    assert(liveview_convert(source, 23, 4, 4, 4, 16, p) < 0);
    assert(liveview_convert(source, 24, 3, 4, 4, 16, p) < 0);
    assert(liveview_convert(source, 24, 4, 4, 3, 16, p) < 0);
    assert(liveview_convert(source, 24, 4, 4, 4, 15, p) < 0);
    assert(liveview_convert(source, 24, 4098, 4, 4098, 16392, p) < 0);
    /* Match the padded SBPC layout; verify every sampled output byte. */
    const uint32_t w = 1300, ht = 970, stride = 1792, uv = 2150400, size = 3227648;
    uint8_t *padded = malloc(size);
    assert(padded);
    for (uint32_t i = 0; i < size; i++) padded[i] = (uint8_t)(i * 17 + i / 4096);
    assert(liveview_convert(padded, size, w, ht, stride, uv, p) == 0);
    for (uint32_t y = 0; y < LV_HEIGHT; y++) {
        uint32_t sy = (uint64_t)y * ht / LV_HEIGHT;
        for (uint32_t x = 0; x < LV_WIDTH; x += 2) {
            uint32_t a = (uint64_t)x * w / LV_WIDTH;
            uint32_t b = (uint64_t)(x + 1) * w / LV_WIDTH;
            uint32_t o = (y * LV_WIDTH + x) * 2;
            assert(p[o] == padded[sy * stride + a]);
            assert(p[o+1] == padded[uv + sy/2 * stride + (a & ~1u)]);
            assert(p[o+2] == padded[sy * stride + b]);
            assert(p[o+3] == padded[uv + sy/2 * stride + (a & ~1u) + 1]);
        }
    }
    free(padded);
    uint8_t *scratch = malloc(LV_BYTES), *latest = malloc(LV_BYTES);
    assert(scratch && latest);
    liveview_blank(latest);
    uint32_t id = 0;
    h->sequence = 0;
    assert(liveview_snapshot(h, scratch, latest, &id) == 0);
    assert(latest[0] == 16 && latest[1] == 128);
    memset(p, 37, LV_BYTES);
    h->sequence = 6;
    h->frame_id = 99;
    assert(liveview_snapshot(h, scratch, latest, &id) == 1 && id == 99);
    /* A writer in progress must never cause an older queued buffer to return. */
    h->sequence = 7;
    memset(p, 91, LV_BYTES);
    assert(liveview_snapshot(h, scratch, latest, &id) == 0 && id == 99);
    for (uint32_t i = 0; i < LV_BYTES; ++i) assert(latest[i] == 37);
    h->sequence = 8;
    h->frame_id = 100;
    assert(liveview_snapshot(h, scratch, latest, &id) == 1 && id == 100);
    for (uint32_t i = 0; i < LV_BYTES; ++i) assert(latest[i] == 91);
    free(scratch);
    free(latest);
    free(shared);
    return 0;
}
