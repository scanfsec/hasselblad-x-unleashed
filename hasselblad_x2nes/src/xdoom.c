/* xdoom - DOOM for the Hasselblad X2D II (doomgeneric + EGL/Wayland).
 *
 * Controls:
 *   joystick up/down    = move forward / back
 *   joystick left/right = turn
 *   shutter half (KEY_A)= fire
 *   M button (KEY_M)    = use / open doors
 *   wheel               = turn
 *   AEL (38)          = confirm/enter,  AF-D (32) = back/escape
 *   touch: two zones at bottom - LEFT(map) / RIGHT(enter,confirm)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "doomgeneric.h"
#include "doomkeys.h"
#include "wl.h"
#include "input.h"
#include <linux/input.h>
#include "util.h"
#include "netpad.h"

#define DISP_W 1024
#define DISP_H 768

/* ---- key event queue (edge-triggered from wl_key_down state) ---- */
#define KQ_SIZE 64
static unsigned char kq_pressed[KQ_SIZE];
static unsigned char kq_key[KQ_SIZE];
static int kq_read, kq_write;

static void kq_push(int pressed, unsigned char key) {
    kq_pressed[kq_write] = (unsigned char)pressed;
    kq_key[kq_write] = key;
    kq_write = (kq_write + 1) % KQ_SIZE;
}

static const struct { int linux_code; unsigned char doom_key; } hwmap[] = {
    { 30,  KEY_FIRE },      /* KEY_A shutter half-press */
    { 46,  KEY_USE },       /* KEY_M */
    { 34,  KEY_UPARROW },   /* KEY_G joystick up */
    { 48,  KEY_DOWNARROW }, /* KEY_B joystick down */
    { 47,  KEY_LEFTARROW }, /* KEY_V joystick left */
    { 49,  KEY_RIGHTARROW },/* KEY_N joystick right */
    { 105, KEY_LEFTARROW }, /* wheel rev */
    { 106, KEY_RIGHTARROW },/* wheel fwd */
    { 38,  KEY_ENTER },     /* KEY_L (AEL): menu confirm */
    { 32,  KEY_ESCAPE },    /* KEY_D (AF-D): menu back/escape */
    { BTN_SOUTH, KEY_FIRE },   /* cross  */
    { BTN_EAST,  KEY_USE },    /* circle */
    { BTN_WEST,  KEY_USE },    /* square */
    { BTN_TL,    KEY_USE },    /* L1     */
    { BTN_TR,    KEY_FIRE },   /* R1     */
    { BTN_TL2,   KEY_RSHIFT }, /* L2 = run */
    { BTN_START, KEY_ENTER },  /* Options */
    { BTN_NORTH, KEY_ESCAPE }, /* triangle = back */
    { BTN_SELECT, KEY_TAB },   /* create = map */
};
#define HWMAP_N (int)(sizeof hwmap / sizeof hwmap[0])
static int prev_held[HWMAP_N];

/* PC-bridged DualSense buttons (netpad bits -> doom keys) */
static const struct { int bit; unsigned char doom_key; } np_map[] = {
    { NP_CROSS,    KEY_FIRE },
    { NP_CIRCLE,   KEY_USE },
    { NP_SQUARE,   KEY_USE },
    { NP_TRIANGLE, KEY_ESCAPE },
    { NP_L1,       KEY_USE },
    { NP_R1,       KEY_FIRE },
    { NP_L2,       KEY_RSHIFT },
    { NP_R2,       KEY_RSHIFT },
    { NP_OPTIONS,  KEY_ENTER },
    { NP_CREATE,   KEY_TAB },
    { NP_UP,       KEY_UPARROW },
    { NP_DOWN,     KEY_DOWNARROW },
};
#define NPMAP_N (int)(sizeof np_map / sizeof np_map[0])
static int prev_np[NPMAP_N];

static void pump_input(void) {
    wl_dispatch();
    netpad_poll();
    for (int i = 0; i < HWMAP_N; i++) {
        int held = wl_key_down(hwmap[i].linux_code) || input_key(hwmap[i].linux_code);
        if (held != prev_held[i]) {
            kq_push(held, hwmap[i].linux_code == BTN_TL2 ? KEY_RSHIFT : hwmap[i].doom_key);
            prev_held[i] = held;
        }
    }
    if (netpad_alive()) {
        unsigned nb = netpad_buttons();
        for (int i = 0; i < NPMAP_N; i++) {
            int held = (nb >> np_map[i].bit) & 1;
            if (held != prev_np[i]) {
                kq_push(held, np_map[i].doom_key);
                fprintf(stderr, "np bit=%d key=%d %d\n", np_map[i].bit, np_map[i].doom_key, held);
                prev_np[i] = held;
            }
        }
    }
    /* analog sticks -> movement (edge-emulated) */
    {
        #define DOOM_DEAD 154
        static int px, py;  /* prev stick dir states */
        int cx = input_abs(ABS_X), cy = input_abs(ABS_Y);
        int hx = input_abs(ABS_HAT0X), hy = input_abs(ABS_HAT0Y);
        if (hx) cx = hx;   /* hat overrides */
        if (hy) cy = hy;
        if (netpad_alive()) {   /* bridged stick overrides when pushed */
            int nx = netpad_norm(0), ny = netpad_norm(1);
            if (nx) cx = nx;
            if (ny) cy = ny;
        }
        int dx = cx < -DOOM_DEAD ? -1 : (cx > DOOM_DEAD ? 1 : 0);
        int dy = cy < -DOOM_DEAD ? -1 : (cy > DOOM_DEAD ? 1 : 0);
        if (dx != (px & 3) - 1) {
            if ((px & 3) - 1 == -1) kq_push(0, KEY_LEFTARROW);
            if ((px & 3) - 1 == 1) kq_push(0, KEY_RIGHTARROW);
            if (dx == -1) kq_push(1, KEY_LEFTARROW);
            if (dx == 1) kq_push(1, KEY_RIGHTARROW);
            px = (dx + 1) | (py & ~3);
        }
        if (dy != ((py >> 2) & 3) - 1) {
            if (((py >> 2) & 3) - 1 == -1) kq_push(0, KEY_UPARROW);
            if (((py >> 2) & 3) - 1 == 1) kq_push(0, KEY_DOWNARROW);
            if (dy == -1) kq_push(1, KEY_UPARROW);
            if (dy == 1) kq_push(1, KEY_DOWNARROW);
            py = ((dy + 1) << 2) | (px & 3);
        }
    }
    /* touch zones: bottom-left = map toggle, bottom-right = enter/use */
    int tn;
    const wl_touch_t *ts = wl_touches(&tn);
    static int prev_l, prev_r;
    int l = 0, r = 0;
    for (int i = 0; i < tn; i++) {
        if (ts[i].x < 130 && ts[i].y < 90) {
            /* EXIT: back to the x2nes menu */
            fprintf(stderr, "xdoom: EXIT tapped\n");
            pid_t p = fork();
            if (p == 0) {
                setsid();
                execl("/data/x2dnes/x2nes", "x2nes", (char*)NULL);
                _exit(1);
            }
            _exit(0);
        }
        if (ts[i].x < 512 && ts[i].y > 660) l = 1;
        else if (ts[i].x >= 512 && ts[i].y > 660) r = 1;
    }
    if (l != prev_l) { kq_push(l, KEY_TAB); prev_l = l; }      /* map */
    if (r != prev_r) { kq_push(r, KEY_ENTER); prev_r = r; }     /* confirm */
}

/* ---- scaled blit 640x400 -> 1024x768 (nearest, LUT) ---- */
static int rowmap[DISP_H], colmap[DISP_W];
static void blit_scale(uint32_t *dst, const uint32_t *src, int sw, int sh) {
    for (int y = 0; y < DISP_H; y++) {
        const uint32_t *s = src + (size_t)rowmap[y] * sw;
        uint32_t *d = dst + (size_t)y * DISP_W;
        for (int x = 0; x < DISP_W; x++) d[x] = s[colmap[x]] | 0xff000000u;
    }
}

/* ---- doomgeneric platform hooks ---- */
void DG_Init(void) {
    wl_init(DISP_W, DISP_H);
    if (netpad_init() == 0)
        fprintf(stderr, "xdoom: netpad listening on udp %d (PC bridge)\n", NETPAD_PORT);
    for (int y = 0; y < DISP_H; y++) rowmap[y] = y * 400 / DISP_H;
    for (int x = 0; x < DISP_W; x++) colmap[x] = x * 640 / DISP_W;
    fprintf(stderr, "xdoom: DG_Init done\n");
}

/* top-left EXIT button: red pad + white X */
static void draw_exit_btn(uint32_t *fb) {
    u_blend_rect(fb, DISP_W, DISP_H, 12, 12, 110, 64, 0xb02020, 190);
    u_blend_rect(fb, DISP_W, DISP_H, 12, 12, 110, 3, 0xffffff, 220);
    u_blend_rect(fb, DISP_W, DISP_H, 12, 73, 110, 3, 0xffffff, 220);
    for (int i = 0; i < 8; i++) {
        u_fill_rect(fb, DISP_W, DISP_H, 38 + i * 6, 22 + i * 5, 5, 5, 0xffffff);
        u_fill_rect(fb, DISP_W, DISP_H, 80 - i * 6, 22 + i * 5, 5, 5, 0xffffff);
    }
}

void DG_DrawFrame(void) {
    extern pixel_t* DG_ScreenBuffer;
    blit_scale(wl_framebuffer(), (const uint32_t *)DG_ScreenBuffer, 640, 400);
    draw_exit_btn(wl_framebuffer());
    wl_commit();
}

void DG_SleepMs(uint32_t ms) { sleep_ms((int)ms); }
uint32_t DG_GetTicksMs(void) { return (uint32_t)now_ms(); }

int DG_GetKey(int *pressed, unsigned char *doomKey) {
    /* D_DoomLoop never returns - this is the only place we can pump */
    pump_input();
    if (kq_read == kq_write) return 0;
    *pressed = kq_pressed[kq_read];
    *doomKey = kq_key[kq_read];
    kq_read = (kq_read + 1) % KQ_SIZE;
    return 1;
}

void DG_SetWindowTitle(const char *title) { (void)title; }

int main(int argc, char **argv) {
    /* default iwad if not given */
    static char *defargv[] = { "xdoom", "-iwad", "/data/x2dnes/doom1.wad", "-nosound", NULL };
    if (argc < 2) { argv = defargv; argc = 3; }
    doomgeneric_Create(argc, argv);
    fprintf(stderr, "xdoom: entering loop\n");
    for (;;) {
        doomgeneric_Tick();
    }
    return 0;
}
