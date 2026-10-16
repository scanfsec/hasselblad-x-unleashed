/* x2nes - NES emulator for the Hasselblad X2D II 100C main screen.
 * Wayland(wl_shm + wl_seat/wl_touch) platform around the LaiNES core.
 * No-arg startup = ROM picker menu over /tmp/roms; tap a row to play,
 * pad EXIT returns to the menu, menu EXIT quits (outer script restores
 * the stock camera GUI).
 */
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>
#include <linux/input.h>
#include <string>
#include <vector>

extern "C" {
#include "wl.h"
#include "util.h"
#include "input.h"
#include "pad.h"
#include "scaler.h"
#include "netpad.h"
}

#include "gui.hpp"
#include "cpu.hpp"
#include "apu.hpp"
#include "cartridge.hpp"

#define DISP_W 1024
#define DISP_H 768
#define NES_PIX (256 * 240)
#define ROMDIR "/data/x2dnes/roms"

/* ---- core-facing state */
static uint32_t frame_px[NES_PIX];
static uint8_t pad_state;
static bool emu_mode;

namespace GUI {

void new_frame(u32* pixels) { memcpy(frame_px, pixels, sizeof frame_px); }
void new_samples(const blip_sample_t* samples, size_t count) {
    (void)samples; (void)count; /* phase 3: tinyalsa */
}
u8 get_joypad_state(int n) { return n == 0 ? pad_state : (u8)0; }
bool is_fast_forward() { return false; }

} // namespace GUI

/* ---- ROM menu ---- */
static std::vector<std::string> menu_names, menu_paths;
static bool in_menu = true;
static int menu_scroll;
static int menu_sel = -1;  /* gamepad-selected row (touch = -1 = no highlight) */

#define MENU_X 132
#define MENU_Y 170
#define MENU_W 760
#define ROW_H 84
#define MAX_ROWS 6

static void menu_load(void) {
    menu_names.clear();
    menu_paths.clear();
    DIR* d = opendir(ROMDIR);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        const char* n = e->d_name;
        size_t len = strlen(n);
        if (len > 4 && !strcasecmp(n + len - 4, ".nes")) {
            std::string base(n, len - 4);
            menu_names.push_back(base);
            menu_paths.push_back(std::string(ROMDIR) + "/" + n);
        }
    }
    closedir(d);
    /* DOOM entry: launches /data/x2dnes/xdoom via fork+exec */
    menu_names.push_back("DOOM");
    menu_paths.push_back("DOOM");
    /* cjmlao (Mario) first */
    for (size_t i = 0; i < menu_paths.size(); i++) {
        if (menu_names[i].find("cjmlao") != std::string::npos && i > 0) {
            menu_names.insert(menu_names.begin(), menu_names[i]);
            menu_paths.insert(menu_paths.begin(), menu_paths[i]);
            menu_names.erase(menu_names.begin() + i + 1);
            menu_paths.erase(menu_paths.begin() + i + 1);
            break;
        }
    }
}

static void start_rom(const char* path) {
    /* reload unconditionally: returning to the menu keeps the old cart
     * in memory, and the old guard made any new pick re-run the old game */
    APU::init();
    Cartridge::load(path);
    emu_mode = Cartridge::loaded();
    fprintf(stderr, "x2nes: rom '%s' loaded=%d\n", path, (int)emu_mode);
}

static void draw_str(uint32_t* px, int w, int h, int x, int y, int cell, uint32_t color, const char* s);

/* 3x5 digit font */
static const uint8_t digits10[10][5] = {
    {0x7, 0x5, 0x5, 0x5, 0x7}, {0x2, 0x6, 0x2, 0x2, 0x7}, {0x7, 0x1, 0x7, 0x4, 0x7},
    {0x7, 0x1, 0x7, 0x1, 0x7}, {0x5, 0x5, 0x7, 0x1, 0x1}, {0x7, 0x4, 0x7, 0x1, 0x7},
    {0x7, 0x4, 0x7, 0x5, 0x7}, {0x7, 0x1, 0x1, 0x1, 0x1}, {0x7, 0x5, 0x7, 0x5, 0x7},
    {0x7, 0x5, 0x7, 0x1, 0x7},
};

static void draw_digit(uint32_t* px, int w, int h, int num, int x, int y, int cell, uint32_t color) {
    if (num < 0 || num > 9) return;
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 3; c++)
            if (digits10[num][r] & (4 >> c))
                u_fill_rect(px, w, h, x + c * cell, y + r * cell, cell, cell, color);
}

static void draw_number(uint32_t* px, int w, int h, int value, int x, int y, int cell, uint32_t color) {
    char buf[12];
    snprintf(buf, sizeof buf, "%d", value);
    int cx = x;
    for (char* p = buf; *p; p++) {
        draw_digit(px, w, h, *p - '0', cx, y, cell, color);
        cx += 4 * cell;
    }
}

/* crude uppercase font for menu labels: rows of 5x7 dots per glyph.
 * We only need A-Z 0-9 and a few symbols; keep it minimal. */
struct glyph { char c; const char* rows[7]; };
static const glyph glyphs[] = {
    {'A', {"01110","10001","10001","11111","10001","10001","10001"}},
    {'B', {"11110","10001","10001","11110","10001","10001","11110"}},
    {'C', {"01111","10000","10000","10000","10000","10000","01111"}},
    {'D', {"11110","10001","10001","10001","10001","10001","11110"}},
    {'E', {"11111","10000","10000","11110","10000","10000","11111"}},
    {'F', {"11111","10000","10000","11110","10000","10000","10000"}},
    {'G', {"01111","10000","10000","10111","10001","10001","01110"}},
    {'H', {"10001","10001","10001","11111","10001","10001","10001"}},
    {'I', {"11111","00100","00100","00100","00100","00100","11111"}},
    {'J', {"00111","00010","00010","00010","00010","10010","01100"}},
    {'K', {"10001","10010","10100","11000","10100","10010","10001"}},
    {'L', {"10000","10000","10000","10000","10000","10000","11111"}},
    {'M', {"10001","11011","10101","10101","10001","10001","10001"}},
    {'N', {"10001","11001","10101","10011","10001","10001","10001"}},
    {'O', {"01110","10001","10001","10001","10001","10001","01110"}},
    {'P', {"11110","10001","10001","11110","10000","10000","10000"}},
    {'Q', {"01110","10001","10001","10001","10101","10011","01111"}},
    {'R', {"11110","10001","10001","11110","10100","10010","10001"}},
    {'S', {"01111","10000","10000","01110","00001","00001","11110"}},
    {'T', {"11111","00100","00100","00100","00100","00100","00100"}},
    {'U', {"10001","10001","10001","10001","10001","10001","01110"}},
    {'V', {"10001","10001","10001","10001","10001","01010","00100"}},
    {'W', {"10001","10001","10001","10101","10101","11011","10001"}},
    {'X', {"10001","10001","01010","00100","01010","10001","10001"}},
    {'Y', {"10001","10001","01010","00100","00100","00100","00100"}},
    {'Z', {"11111","00001","00010","00100","01000","10000","11111"}},
    {'0', {"01110","10001","10011","10101","11001","10001","01110"}},
    {'1', {"00100","01100","00100","00100","00100","00100","01110"}},
    {'2', {"01110","10001","00001","00110","01000","10000","11111"}},
    {'3', {"11110","00001","00001","01110","00001","00001","11110"}},
    {'4', {"00010","00110","01010","10010","11111","00010","00010"}},
    {'5', {"11111","10000","11110","00001","00001","10001","01110"}},
    {'6', {"00110","01000","10000","11110","10001","10001","01110"}},
    {'7', {"11111","00001","00010","00100","01000","01000","01000"}},
    {'8', {"01110","10001","10001","01110","10001","10001","01110"}},
    {'9', {"01110","10001","10001","01111","00001","00010","01100"}},
    {' ', {"00000","00000","00000","00000","00000","00000","00000"}},
    {'.', {"00000","00000","00000","00000","00000","01100","01100"}},
    {'-', {"00000","00000","00000","11111","00000","00000","00000"}},
    {'>', {"01000","00100","00010","00001","00010","00100","01000"}},
    {':', {"00000","01100","01100","00000","01100","01100","00000"}},
    {'/', {"00001","00010","00010","00100","01000","01000","10000"}},
};

static void draw_str(uint32_t* px, int w, int h, int x, int y, int cell, uint32_t color, const char* s) {
    int cx = x;
    for (const char* p = s; *p; p++) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        const glyph* g = nullptr;
        for (size_t i = 0; i < sizeof glyphs / sizeof glyphs[0]; i++)
            if (glyphs[i].c == c) { g = &glyphs[i]; break; }
        if (g) {
            for (int r = 0; r < 7; r++)
                for (int cc = 0; cc < 5; cc++)
                    if (g->rows[r][cc] == '1')
                        u_fill_rect(px, w, h, cx + cc * cell, y + r * cell, cell, cell, color);
        }
        cx += 6 * cell;
    }
}

static void menu_render(uint32_t* fb) {
    u_fill_rect(fb, DISP_W, DISP_H, 0, 0, DISP_W, DISP_H, 0xff101018);
    u_fill_rect(fb, DISP_W, DISP_H, 0, 0, DISP_W, 110, 0xff181830);
    draw_str(fb, DISP_W, DISP_H, 40, 32, 6, 0xffe8e8f0, "X2D II - GAME");
    draw_str(fb, DISP_W, DISP_H, MENU_X, 140, 3, 0xff606088, "TAP A ROM TO PLAY");

    int rows = (int)menu_paths.size();
    int total = rows + 1; /* + EXIT row */
    if (total > MAX_ROWS && menu_scroll > total - MAX_ROWS)
        menu_scroll = total - MAX_ROWS;
    for (int i = 0; i < MAX_ROWS; i++) {
        int idx = i + menu_scroll;
        if (idx >= total) break;
        int y = MENU_Y + i * (ROW_H + 10);
        bool is_exit = (idx == rows);
        u_blend_rect(fb, DISP_W, DISP_H, MENU_X, y, MENU_W, ROW_H,
                     is_exit ? 0x402020 : 0x1c1c2e, 255);
        u_blend_rect(fb, DISP_W, DISP_H, MENU_X, y, MENU_W, 3,
                     is_exit ? 0xd03838 : 0x7f8fff, 255);
        if (idx == menu_sel)
            u_blend_rect(fb, DISP_W, DISP_H, MENU_X + 6, y + 4, MENU_W - 12, ROW_H - 8, 0x7f8fff, 90);
        if (is_exit) {
            draw_str(fb, DISP_W, DISP_H, MENU_X + 40, y + 28, 5, 0xffff9090, "EXIT - BACK TO CAMERA");
        } else {
            char label[128];
            snprintf(label, sizeof label, "%d. %s", idx + 1, menu_names[idx].c_str());
            draw_str(fb, DISP_W, DISP_H, MENU_X + 40, y + 28, 5, 0xffe0e0f0, label);
            draw_str(fb, DISP_W, DISP_H, MENU_X + MENU_W - 170, y + 28, 5, 0xff38b0d0, "PLAY >");
        }
    }
    if (total > MAX_ROWS) {
        draw_str(fb, DISP_W, DISP_H, MENU_X + MENU_W + 16, MENU_Y, 4, 0xff606088,
                 menu_scroll > 0 ? "<" : " ");
        draw_str(fb, DISP_W, DISP_H, MENU_X + MENU_W + 16,
                 MENU_Y + (MAX_ROWS - 1) * (ROW_H + 10), 4, 0xff606088,
                 menu_scroll + MAX_ROWS < total ? ">" : " ");
    }
}

/* returns: 0 = nothing, 1 = tapped rom (idx out), 2 = exit tapped, 3 = scroll */
static int menu_hit(int x, int y, int* row) {
    int rows = (int)menu_paths.size();
    int total = rows + 1;
    for (int i = 0; i < MAX_ROWS; i++) {
        int idx = i + menu_scroll;
        if (idx >= total) break;
        int ry = MENU_Y + i * (ROW_H + 10);
        if (x >= MENU_X && x < MENU_X + MENU_W && y >= ry && y < ry + ROW_H) {
            *row = idx;
            return idx == rows ? 2 : 1;
        }
    }
    if (x >= MENU_X + MENU_W + 8 && x < MENU_X + MENU_W + 60) {
        if (y >= MENU_Y && y < MENU_Y + 60 && menu_scroll > 0) { menu_scroll--; return 3; }
        int by = MENU_Y + (MAX_ROWS - 1) * (ROW_H + 10);
        if (y >= by && y < by + 60 && menu_scroll + MAX_ROWS < total) { menu_scroll++; return 3; }
    }
    return 0;
}

/* ---- hardware buttons/wheels (codes from x2nes/grab_keys.py, exmcu_input) ----
 * Fill hw_keys[] with the observed EV_KEY codes; REL wheels map to
 * left/right taps already. */
static const struct { int code; uint8_t bit; } hw_keys[] = {
    { KEY_A,     PAD_A },     /* shutter half-press = jump */
    { KEY_M,     PAD_B },     /* M button = run/fire       */
    { KEY_G,     PAD_UP },    /* joystick up    (sampled)  */
    { KEY_B,     PAD_DOWN },  /* joystick down  (sampled)  */
    { KEY_V,     PAD_LEFT },  /* joystick left  (sampled)  */
    { KEY_N,     PAD_RIGHT }, /* joystick right (sampled)  */
    { KEY_RIGHT, PAD_RIGHT }, /* rear wheel fwd (autorepeat) */
    { KEY_LEFT,  PAD_LEFT },  /* rear wheel rev             */
};
/* gamepad buttons (DualSense/standard HID gamepad codes) */
static const struct { int code; uint8_t bit; } gp_keys[] = {
    { BTN_SOUTH, PAD_A },      /* cross   = jump/fire */
    { BTN_EAST,  PAD_B },      /* circle  = run/use   */
    { BTN_TL,    PAD_B },      /* L1 also = B         */
    { BTN_TR,    PAD_A },      /* R1 also = A         */
    { BTN_START, PAD_START },
    { BTN_SELECT, PAD_SELECT },
};
#define STICK_DEAD 154          /* ~30% of 512 */

static uint8_t hw_scan(void) {
    uint8_t p = 0;
    /* camera body keys (via wl_keyboard, weston-forwarded) */
    for (unsigned i = 0; i < sizeof hw_keys / sizeof hw_keys[0]; i++)
        if (wl_key_down(hw_keys[i].code)) p |= hw_keys[i].bit;
    /* gamepad buttons: evdev + weston-forwarded both */
    for (unsigned i = 0; i < sizeof gp_keys / sizeof gp_keys[0]; i++)
        if (input_key(gp_keys[i].code) || wl_key_down(gp_keys[i].code))
            p |= gp_keys[i].bit;
    /* dpad hat */
    if (wl_key_down(BTN_TRIGGER_HAPPY1) || wl_key_down(0x220 /*DPAD_UP*/)) p |= PAD_UP;
    if (wl_key_down(BTN_TRIGGER_HAPPY2) || wl_key_down(0x221)) p |= PAD_DOWN;
    if (wl_key_down(BTN_TRIGGER_HAPPY3) || wl_key_down(0x222)) p |= PAD_LEFT;
    if (wl_key_down(BTN_TRIGGER_HAPPY4) || wl_key_down(0x223)) p |= PAD_RIGHT;
    int hx = input_abs(ABS_HAT0X), hy = input_abs(ABS_HAT0Y);
    if (hx < -STICK_DEAD) p |= PAD_LEFT;
    if (hx >  STICK_DEAD) p |= PAD_RIGHT;
    if (hy < -STICK_DEAD) p |= PAD_UP;
    if (hy >  STICK_DEAD) p |= PAD_DOWN;
    /* analog left stick -> d-pad (menu + games) */
    int lx = input_abs(ABS_X), ly = input_abs(ABS_Y);
    if (lx < -STICK_DEAD) p |= PAD_LEFT;
    if (lx >  STICK_DEAD) p |= PAD_RIGHT;
    if (ly < -STICK_DEAD) p |= PAD_UP;
    if (ly >  STICK_DEAD) p |= PAD_DOWN;
    /* rear wheel */
    int rx = input_rel_consumed(0 /*REL_X*/);
    if (rx < 0) p |= PAD_LEFT;
    if (rx > 0) p |= PAD_RIGHT;
    /* PC-bridged DualSense (netpad, UDP 7777) */
    if (netpad_alive()) {
        unsigned nb = netpad_buttons();
        if (nb & (1u << NP_CROSS))   p |= PAD_A;
        if (nb & (1u << NP_CIRCLE))  p |= PAD_B;
        if (nb & (1u << NP_TRIANGLE)) p |= PAD_B;
        if (nb & (1u << NP_L1))      p |= PAD_B;
        if (nb & (1u << NP_R1))      p |= PAD_A;
        if (nb & (1u << NP_OPTIONS)) p |= PAD_START;
        if (nb & (1u << NP_CREATE))  p |= PAD_SELECT;
        if (nb & (1u << NP_UP))    p |= PAD_UP;
        if (nb & (1u << NP_DOWN))  p |= PAD_DOWN;
        int nlx = netpad_norm(0), nly = netpad_norm(1);
        if (nlx < -STICK_DEAD) p |= PAD_LEFT;
        if (nlx >  STICK_DEAD) p |= PAD_RIGHT;
        if (nly < -STICK_DEAD) p |= PAD_UP;
        if (nly >  STICK_DEAD) p |= PAD_DOWN;
    }
    return p;
}

int main(int argc, char** argv) {
    const char* rom = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--selftest")) { rom = (const char*)1; }
        else rom = argv[i];
    }
    bool selftest = (rom == (const char*)1);

    if (wl_init(DISP_W, DISP_H) < 0) {
        fprintf(stderr, "x2nes: wayland init failed: %s\n", wl_error());
        return 1;
    }
    input_init();
    input_dump_devices();
    if (netpad_init() == 0)
        fprintf(stderr, "x2nes: netpad listening on udp %d (PC bridge)\n", NETPAD_PORT);
    scaler_init(DISP_W, DISP_H);

    menu_load();
    fprintf(stderr, "x2nes: %zu roms in %s\n", menu_paths.size(), ROMDIR);
    if (selftest) {

        in_menu = false;
        emu_mode = false;
    } else if (rom) {
        start_rom(rom);
        in_menu = !emu_mode;
    }

    /* eagle-weston sends frame callbacks at ~6.6Hz only, so we pace
     * ourselves (60Hz NES rate) and never block on the callback. */
    struct timespec next_frame;
    clock_gettime(CLOCK_MONOTONIC, &next_frame);
    const long FRAME_NS = 16639447; /* 60.0988 Hz NTSC NES */

    uint64_t frames = 0, fps_frames0 = 0, fps_t0 = now_ms();
    int fps = 0;
    bool running = true;
    while (running) {
        input_poll();
        netpad_poll();
        wl_dispatch();

        int tn;
        const wl_touch_t* wt = wl_touches(&tn);

        if (in_menu) {
            /* tap-to-select (a fresh touch triggers on the NEXT frame's
             * down event; we act on points present now) */
            static int prev_tn = 0;
            uint8_t hwp = hw_scan();
            /* gamepad menu navigation: dpad/stick move, START/A confirm */
            {
                static int sel_row, prev_up, prev_dn, prev_ok, prev_back;
                int now_up = !!(hwp & (PAD_UP)), now_dn = !!(hwp & (PAD_DOWN));
                int now_ok = !!(hwp & (PAD_START | PAD_A));
                int now_back = !!(hwp & (PAD_SELECT | PAD_B));
                int total = (int)menu_paths.size() + 1;
                if (now_up && !prev_up && sel_row > 0) sel_row--;
                if (now_dn && !prev_dn && sel_row < total - 1) sel_row++;
                if (now_back && !prev_back) running = false; /* SELECT/B = EXIT row */
                if (now_ok && !prev_ok) {
                    if (sel_row == (int)menu_paths.size()) running = false;
                    else {
                        if (menu_paths[sel_row].size() < 5 ||
                            menu_paths[sel_row].compare(menu_paths[sel_row].size() - 4, 4, ".nes") != 0) {
                            fprintf(stderr, "x2nes: launching DOOM\n");
                            pid_t p = fork();
                            if (p == 0) {
                                setsid();
                                execl("/data/x2dnes/xdoom", "xdoom", (char*)NULL);
                                _exit(1);
                            }
                            _exit(0);
                        }
                        start_rom(menu_paths[sel_row].c_str());
                        if (emu_mode) in_menu = false;
                    }
                }
                prev_up = now_up; prev_dn = now_dn; prev_ok = now_ok; prev_back = now_back;
                menu_sel = sel_row;
            }
            if (tn > 0 && prev_tn == 0) {
                int row;
                int hit = menu_hit(wt[0].x, wt[0].y, &row);
                if (hit == 1) {
                    if (menu_paths[row].size() < 5 || menu_paths[row].compare(menu_paths[row].size() - 4, 4, ".nes") != 0) {
                        fprintf(stderr, "x2nes: launching DOOM\n");
                        pid_t p = fork();
                        if (p == 0) {
                            setsid();
                            execl("/data/x2dnes/xdoom", "xdoom", (char*)NULL);
                            _exit(1);
                        }
                        _exit(0); /* x2nes steps aside; game.sh watches both */
                    }
                    start_rom(menu_paths[row].c_str());
                    if (emu_mode) in_menu = false;
                } else if (hit == 2) {
                    running = false;
                }
            }
            prev_tn = tn;
            uint32_t* fb = wl_framebuffer();
            menu_render(fb);
            draw_number(fb, DISP_W, DISP_H, fps, DISP_W - 90, 16, 6, 0xff707088);
            wl_commit();
        } else if (emu_mode) {
            int xs[10], ys[10];
            for (int i = 0; i < tn && i < 10; i++) { xs[i] = wt[i].x; ys[i] = wt[i].y; }
            pad_state = pad_scan(xs, ys, tn) | hw_scan();
            if (pad_exit_pressed()) {
                in_menu = true;  /* pad EXIT returns to the menu */
                emu_mode = false; /* allow the next pick to load a fresh cart */
            } else {
                CPU::run_frame();
                uint32_t* fb = wl_framebuffer();
                scaler_blit(fb, frame_px);
                for (int i = 0; i < tn && i < 10; i++)
                    u_blend_circle(fb, DISP_W, DISP_H, xs[i], ys[i], 36, 0xffff00, 170);
                pad_draw(fb, DISP_W, DISP_H, pad_state);
                draw_number(fb, DISP_W, DISP_H, fps, DISP_W - 90, 16, 6, 0xff707088);
                wl_commit();
            }
        } else {
            /* selftest animation */
            uint32_t* fb = wl_framebuffer();
            (void)fb;
            static uint32_t st[NES_PIX];
            int fx = (int)(frames % 256);
            for (int y = 0; y < 240; y++)
                for (int x = 0; x < 256; x++) {
                    int v = (x + y + (int)(frames % 512)) & 0xff;
                    st[(size_t)y * 256 + x] = 0xff000000u |
                        ((uint32_t)((x + fx) & 0xff) << 16) | ((uint32_t)(v & 0x7f) << 8);
                }
            scaler_blit(wl_framebuffer(), st);
            wl_commit();
        }

        next_frame.tv_nsec += FRAME_NS;
        while (next_frame.tv_nsec >= 1000000000L) {
            next_frame.tv_sec++;
            next_frame.tv_nsec -= 1000000000L;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_frame, NULL);

        frames++;
        uint64_t now = now_ms();
        if (now - fps_t0 >= 1000) {
            fps = (int)(frames - fps_frames0);
            fps_frames0 = frames;
            fps_t0 = now;
            fprintf(stderr, "[fps] %d touch=%d mode=%s\n", fps, tn, in_menu ? "menu" : (emu_mode ? "emu" : "selftest"));
        }
    }
    wl_shutdown();
    fprintf(stderr, "x2nes: exit after %llu frames\n", (unsigned long long)frames);
    return 0;
}
