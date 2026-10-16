#include "util.h"

#include <errno.h>
#include <time.h>

uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

void sleep_ms(int ms) {
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {}
}

static void clamp_rect(int w, int h, int *x, int *y, int *rw, int *rh) {
    if (*x < 0) { *rw += *x; *x = 0; }
    if (*y < 0) { *rh += *y; *y = 0; }
    if (*x + *rw > w) *rw = w - *x;
    if (*y + *rh > h) *rh = h - *y;
}

void u_fill_rect(uint32_t *px, int w, int h, int x, int y, int rw, int rh, uint32_t color) {
    clamp_rect(w, h, &x, &y, &rw, &rh);
    for (int j = 0; j < rh; j++) {
        uint32_t *row = px + (size_t)(y + j) * w + x;
        for (int i = 0; i < rw; i++) row[i] = color;
    }
}

void u_fill_circle(uint32_t *px, int w, int h, int cx, int cy, int r, uint32_t color) {
    int x0 = cx - r, x1 = cx + r, y0 = cy - r, y1 = cy + r;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= w) x1 = w - 1;
    if (y1 >= h) y1 = h - 1;
    for (int y = y0; y <= y1; y++) {
        uint32_t *row = px + (size_t)y * w;
        for (int x = x0; x <= x1; x++) {
            int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy <= r * r) row[x] = color;
        }
    }
}

static inline uint32_t blend(uint32_t dst, uint32_t src, int a) {
    int dr = (dst >> 16) & 0xff, dg = (dst >> 8) & 0xff, db = dst & 0xff;
    int sr = (src >> 16) & 0xff, sg = (src >> 8) & 0xff, sb = src & 0xff;
    int r = dr + ((sr - dr) * a) / 255;
    int g = dg + ((sg - dg) * a) / 255;
    int b = db + ((sb - db) * a) / 255;
    return 0xff000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

void u_blend_rect(uint32_t *px, int w, int h, int x, int y, int rw, int rh, uint32_t color, int alpha) {
    clamp_rect(w, h, &x, &y, &rw, &rh);
    for (int j = 0; j < rh; j++) {
        uint32_t *row = px + (size_t)(y + j) * w + x;
        for (int i = 0; i < rw; i++) row[i] = blend(row[i], color, alpha);
    }
}

void u_blend_circle(uint32_t *px, int w, int h, int cx, int cy, int r, uint32_t color, int alpha) {
    int x0 = cx - r, x1 = cx + r, y0 = cy - r, y1 = cy + r;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= w) x1 = w - 1;
    if (y1 >= h) y1 = h - 1;
    for (int y = y0; y <= y1; y++) {
        uint32_t *row = px + (size_t)y * w;
        for (int x = x0; x <= x1; x++) {
            int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy <= r * r) row[x] = blend(row[x], color, alpha);
        }
    }
}
