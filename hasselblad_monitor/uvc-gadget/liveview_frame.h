#ifndef LIVEVIEW_FRAME_H
#define LIVEVIEW_FRAME_H
#include <stdint.h>
#include <string.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
#define LV_WIDTH 640u
#define LV_HEIGHT 480u
#define LV_BYTES (LV_WIDTH * LV_HEIGHT * 2u)
#define LV_OFFSET 4096u
#define LV_FILE_BYTES (LV_OFFSET + LV_BYTES)
#define LV_MAGIC 0x31564355u
struct liveview_header {
    uint32_t magic, version, width, height;
    volatile uint32_t sequence;
    uint32_t frame_id, source_width, source_height;
};
static inline void liveview_barrier(void)
{
#if defined(__aarch64__)
    __asm__ volatile("dmb ish" ::: "memory");
#else
    __sync_synchronize();
#endif
}
static inline void liveview_copy_row(uint8_t *destination, const uint8_t *source, uint32_t length)
{
#if defined(__aarch64__)
    /* Keep the camera library self-contained: its vendor loader cannot
     * safely resolve a Linux-built libc PLT dependency. */
    while (length >= 16) {
        vst1q_u8(destination, vld1q_u8(source));
        destination += 16;
        source += 16;
        length -= 16;
    }
    while (length--) *destination++ = *source++;
#else
    memcpy(destination, source, length);
#endif
}
/* Failed snapshots must preserve the latest complete image, not the older
 * image left in whichever V4L2 buffer happens to be dequeued this time. */
static inline void liveview_blank(uint8_t *image)
{
    for (uint32_t i = 0; i < LV_BYTES; i += 4) {
        image[i] = image[i + 2] = 16;
        image[i + 1] = image[i + 3] = 128;
    }
}
static inline int liveview_snapshot(struct liveview_header *header,
                                    uint8_t *scratch, uint8_t *latest,
                                    uint32_t *frame_id)
{
    for (unsigned int attempt = 0; attempt < 3; ++attempt) {
        uint32_t sequence = header->sequence;
        if (!sequence || (sequence & 1)) continue;
        liveview_barrier();
        memcpy(scratch, (uint8_t *)header + LV_OFFSET, LV_BYTES);
        uint32_t id = header->frame_id;
        liveview_barrier();
        if (sequence != header->sequence) continue;
        memcpy(latest, scratch, LV_BYTES);
        *frame_id = id;
        return 1;
    }
    return 0;
}
/* Bound every plane access before sampling a CPU-mapped NV12 frame. */
static int liveview_convert(const uint8_t *source, uint32_t size,
                            uint32_t width, uint32_t height, uint32_t stride,
                            uint32_t uv_offset, uint8_t *output)
{
    if (!source || !output || width < 2 || width > 4096 || height < 2 ||
        (width & 1) || (height & 1) || stride < width ||
        (uint64_t)stride * height > uv_offset ||
        (uint64_t)uv_offset + (uint64_t)stride * (height / 2) > size)
        return -1;
    /* DSP buffers may be uncached. Read each row contiguously through
     * vector loads before scattered downsampling; avoid hundreds of
     * thousands of individual uncached byte loads on the camera pipeline. */
    uint8_t luma_row[4096], chroma_row[4096];
    uint32_t previous_chroma = UINT32_MAX;
    uint32_t x0[LV_WIDTH / 2], x1[LV_WIDTH / 2];
    for (uint32_t x = 0; x < LV_WIDTH; x += 2) {
        x0[x / 2] = (uint64_t)x * width / LV_WIDTH;
        x1[x / 2] = (uint64_t)(x + 1) * width / LV_WIDTH;
    }
    for (uint32_t y = 0; y < LV_HEIGHT; ++y) {
        uint32_t sy = (uint64_t)y * height / LV_HEIGHT;
        liveview_copy_row(luma_row, source + sy * stride, width);
        if (sy / 2 != previous_chroma) {
            liveview_copy_row(chroma_row, source + uv_offset + (sy / 2) * stride, width);
            previous_chroma = sy / 2;
        }
        for (uint32_t x = 0; x < LV_WIDTH; x += 2) {
            uint32_t sx0 = x0[x / 2], sx1 = x1[x / 2];
            uint32_t uvx = sx0 & ~1u;
            *output++ = luma_row[sx0];
            *output++ = chroma_row[uvx];
            *output++ = luma_row[sx1];
            *output++ = chroma_row[uvx + 1];
        }
    }
    return 0;
}
#endif
