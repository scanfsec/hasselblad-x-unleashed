#include "pad.h"
#include "util.h"

#include <stdio.h>

/* layout for 1024x768 (matches the touchscreen ABS range) */
#define DPAD_CX 215
#define DPAD_CY 540
#define DPAD_R 130
#define DPAD_TH 42
#define BTN_A_X 905
#define BTN_A_Y 505
#define BTN_B_X 790
#define BTN_B_Y 625
#define BTN_R 75
#define SEL_X 415
#define SEL_Y 715
#define SEL_W 100
#define SEL_H 40
#define STA_X 545
#define STA_Y 715
#define STA_W 100
#define STA_H 40
#define EXIT_X 962
#define EXIT_Y 26
#define EXIT_W 54
#define EXIT_H 40

static int inside_circle(int x, int y, int cx, int cy, int r) {
    int dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= r * r;
}

static int inside_rect(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static int exit_held;

uint8_t pad_scan(const int *xs, const int *ys, int n) {
    uint8_t p = 0;
    exit_held = 0;
    for (int i = 0; i < n; i++) {
        int x = xs[i], y = ys[i];
        if (inside_rect(x, y, SEL_X - 15, SEL_Y - 15, SEL_W + 30, SEL_H + 30)) p |= PAD_SELECT;
        if (inside_rect(x, y, STA_X - 15, STA_Y - 15, STA_W + 30, STA_H + 30)) p |= PAD_START;
        if (inside_rect(x, y, EXIT_X - 8, EXIT_Y - 8, EXIT_W + 16, EXIT_H + 16)) exit_held = 1;
    }
    return p;
}

int pad_exit_pressed(void) { return exit_held; }

static const uint32_t COL_BASE = 0x202028;
static const uint32_t COL_ON = 0x7f8fff;
static const uint32_t COL_BTN_BASE = 0x50282828u | 0x00303030;
static const uint32_t COL_A = 0xd03838;
static const uint32_t COL_B = 0x3858d0;

static void draw_dpad(uint32_t *px, int w, int h, uint8_t p) {
    int cw = 46, cl = 118; /* half sizes */
    u_blend_rect(px, w, h, DPAD_CX - cw, DPAD_CY - cl, cw * 2, cl * 2 - cw, p & PAD_UP ? COL_ON : COL_BASE, 150);
    u_blend_rect(px, w, h, DPAD_CX - cw, DPAD_CY + cw, cw * 2, cl * 2 - cw, p & PAD_DOWN ? COL_ON : COL_BASE, 150);
    u_blend_rect(px, w, h, DPAD_CX - cl, DPAD_CY - cw, cl * 2 - cw, cw * 2, p & PAD_LEFT ? COL_ON : COL_BASE, 150);
    u_blend_rect(px, w, h, DPAD_CX + cw, DPAD_CY - cw, cl * 2 - cw, cw * 2, p & PAD_RIGHT ? COL_ON : COL_BASE, 150);
    u_blend_rect(px, w, h, DPAD_CX - cw, DPAD_CY - cw, cw * 2, cw * 2, 0x101014, 150);
}

static const char x5[5] = {0x19, 0x0a, 0x04, 0x0a, 0x19}; /* 5x5 X pattern rows */
/* rows as 5-bit: 11001,10101? simpler: hardcode */
static const uint8_t xpat[5][5] = {
    {1,0,0,0,1},
    {0,1,0,1,0},
    {0,0,1,0,0},
    {0,1,0,1,0},
    {1,0,0,0,1},
};

static void draw_exit(uint32_t *px, int w, int h) {
    u_blend_rect(px, w, h, EXIT_X, EXIT_Y, EXIT_W, EXIT_H, 0xb02020, 170);
    u_blend_rect(px, w, h, EXIT_X, EXIT_Y, EXIT_W, 3, 0xffffff, 200);
    u_blend_rect(px, w, h, EXIT_X, EXIT_Y + EXIT_H - 3, EXIT_W, 3, 0xffffff, 200);
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 5; c++)
            if (xpat[r][c])
                u_fill_rect(px, w, h, EXIT_X + 12 + c * 6, EXIT_Y + 6 + r * 6, 5, 5, 0xffffff);
    (void)x5;
}

void pad_draw(uint32_t *px, int w, int h, uint8_t pressed) {
    /* joystick/shutter/M hardware keys drive gameplay; on screen only
     * SELECT/START (menu confirm) and EXIT remain */
    u_blend_rect(px, w, h, SEL_X, SEL_Y, SEL_W, SEL_H, (pressed & PAD_SELECT) ? 0x7f8fff : COL_BASE, 200);
    u_blend_rect(px, w, h, STA_X, STA_Y, STA_W, STA_H, (pressed & PAD_START) ? 0x7f8fff : COL_BASE, 200);
    draw_exit(px, w, h);
}
