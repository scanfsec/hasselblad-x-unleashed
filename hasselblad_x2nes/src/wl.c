/* x2nes display/input backend - libwayland-client + EGL/GLES2 edition.
 *
 * The hand-rolled wl_shm path lit up nothing on eagle-weston (buffers
 * accepted but never scanned out - VOP underflow). The stock GUI renders
 * through wl_egl_window + EGL (PVR WSEGL -> eagle_shm dmabuf) and that
 * path works - verified by egltest (animated quad on screen).
 *
 * Public API stays the same as the old hand-rolled version:
 *   wl_init / wl_framebuffer / wl_commit / wl_dispatch / wl_touches ...
 * The framebuffer stays a plain CPU buffer (1024x768 XRGB) so menu, pad
 * overlay and the NES scaler keep working; wl_commit uploads it as a
 * texture and draws a fullscreen quad (GPU does the scaling).
 * Touch input now comes from wl_seat/wl_touch events (standard path -
 * evdev is EVIOCGRABbed by weston so raw reads see nothing).
 */
#include "wl.h"
#include "util.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-egl.h>

#define DISP_W 1024
#define DISP_H 768

static struct wl_display *g_disp;
static struct wl_surface *g_surf;
static struct wl_egl_window *g_win;
static EGLDisplay g_ed;
static EGLContext g_ctx;
static EGLSurface g_es;
static GLuint g_prog, g_tex;
static GLint g_ap;
static uint32_t *g_fb;
static char g_err[256];

static void seterr(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(g_err, sizeof g_err, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[wl] %s\n", g_err);
}
const char *wl_error(void) { return g_err[0] ? g_err : NULL; }

struct ts_pt { int id, x, y; };
static struct ts_pt ts_pts[10];
static int n_ts_pts;
static int ts_dirty;

static void ts_up(int id) {
    for (int i = 0; i < n_ts_pts; i++)
        if (ts_pts[i].id == id) { ts_pts[i] = ts_pts[--n_ts_pts]; ts_dirty = 1; return; }
}
static void ts_move(int id, int x, int y) {
    for (int i = 0; i < n_ts_pts; i++)
        if (ts_pts[i].id == id) { ts_pts[i].x = x; ts_pts[i].y = y; return; }
}
static void ts_dn(int id, int x, int y) {
    ts_up(id);
    if (n_ts_pts < 10) {
        ts_pts[n_ts_pts].id = id; ts_pts[n_ts_pts].x = x; ts_pts[n_ts_pts].y = y;
        n_ts_pts++; ts_dirty = 1;
    }
}

const wl_touch_t *wl_touches(int *n) { *n = n_ts_pts; return (const wl_touch_t *)ts_pts; }

/* keyboard state via wl_keyboard (weston forwards joystick/hardware keys
 * to the focused client - the same path the stock GUI receives JstkUp etc).
 * Key codes are plain linux evdev codes. */
static uint8_t kb_state[512];
int wl_key_down(int code) {
    if (code < 0 || code >= 512) return 0;
    return (kb_state[code >> 3] >> (code & 7)) & 1;
}
int wl_touch_version(void) { int d = ts_dirty; ts_dirty = 0; return d; }

static void touch_down_ev(void *d, struct wl_touch *t, uint32_t ser, uint32_t tm,
                       struct wl_surface *s, int32_t id, wl_fixed_t sx, wl_fixed_t sy) {
    (void)d;(void)t;(void)ser;(void)tm;(void)s;
    ts_dn(id, wl_fixed_to_int(sx), wl_fixed_to_int(sy));
}
static void touch_up_ev(void *d, struct wl_touch *t, uint32_t ser, uint32_t tm, int32_t id) {
    (void)d;(void)t;(void)ser;(void)tm; ts_up(id);
}
static void touch_motion_ev(void *d, struct wl_touch *t, uint32_t tm, int32_t id,
                         wl_fixed_t sx, wl_fixed_t sy) {
    (void)d;(void)t;(void)tm;
    ts_move(id, wl_fixed_to_int(sx), wl_fixed_to_int(sy));
}
static void touch_frame_ev(void *d, struct wl_touch *t) { (void)d;(void)t; }
static void touch_cancel_ev(void *d, struct wl_touch *t) { (void)d;(void)t; n_ts_pts = 0; ts_dirty = 1; }
static const struct wl_touch_listener touch_l =
    { touch_down_ev, touch_up_ev, touch_motion_ev, touch_frame_ev, touch_cancel_ev };

static void seat_caps_ev(void *d, struct wl_seat *s, uint32_t caps);
static void seat_name_ev(void *d, struct wl_seat *s, const char *n) { (void)d;(void)s;(void)n; }
static const struct wl_seat_listener seat_l = { seat_caps_ev, seat_name_ev };

static void kb_keymap_ev(void *d, struct wl_keyboard *k, uint32_t fmt, int32_t fd, uint32_t size) {
    (void)d;(void)k;(void)fmt; close(fd); (void)size;
}
static void kb_key_ev(void *d, struct wl_keyboard *k, uint32_t ser, uint32_t tm,
                      uint32_t key, uint32_t state) {
    (void)d;(void)k;(void)ser;(void)tm;
    if (getenv("XDOOM_KEYLOG")) fprintf(stderr, "[key] code=%d state=%d\n", key, state);
    if (key < 512) {
        if (state) kb_state[key >> 3] |= (uint8_t)(1 << (key & 7));
        else kb_state[key >> 3] &= (uint8_t)~(1 << (key & 7));
    }
}
static void kb_enter_ev(void *d, struct wl_keyboard *k, uint32_t ser, struct wl_surface *s, struct wl_array *keys) { (void)d;(void)k;(void)ser;(void)s;(void)keys; }
static void kb_leave_ev(void *d, struct wl_keyboard *k, uint32_t ser, struct wl_surface *s) { (void)d;(void)k;(void)ser;(void)s; }
static void kb_repeat_ev(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay) { (void)d;(void)k;(void)rate;(void)delay; }
static void kb_mods_ev(void *d, struct wl_keyboard *k, uint32_t a, uint32_t b, uint32_t c, uint32_t e, uint32_t f) { (void)d;(void)k;(void)a;(void)b;(void)c;(void)e;(void)f; }
static const struct wl_keyboard_listener kb_l =
    { kb_keymap_ev, kb_enter_ev, kb_leave_ev, kb_key_ev, kb_mods_ev, kb_repeat_ev };

static void seat_caps_ev(void *d, struct wl_seat *seat, uint32_t caps) {
    (void)d;
    if (caps & WL_SEAT_CAPABILITY_KEYBOARD) {
        struct wl_keyboard *kb = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(kb, &kb_l, NULL);
        fprintf(stderr, "[wl] wl_keyboard acquired via libwayland\n");
    }
    if (caps & WL_SEAT_CAPABILITY_TOUCH) {
        struct wl_touch *t = wl_seat_get_touch(seat);
        wl_touch_add_listener(t, &touch_l, NULL);
        fprintf(stderr, "[wl] wl_touch acquired via libwayland\n");
    }
}

static struct wl_compositor *g_comp;
static struct wl_shell *g_shell;
static struct wl_seat *g_seat;

static void reg_global_ev(void *d, struct wl_registry *r, uint32_t name,
                       const char *iface, uint32_t ver) {
    (void)d; (void)ver;
    if (!strcmp(iface, "wl_compositor")) g_comp = wl_registry_bind(r, name, &wl_compositor_interface, 1);
    else if (!strcmp(iface, "wl_shell")) g_shell = wl_registry_bind(r, name, &wl_shell_interface, 1);
    else if (!strcmp(iface, "wl_seat")) g_seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
}
static void reg_remove_ev(void *d, struct wl_registry *r, uint32_t n) { (void)d;(void)r;(void)n; }
static const struct wl_registry_listener reg_l = { reg_global_ev, reg_remove_ev };

static const char *VS =
    "attribute vec2 p; varying vec2 v;"
    "void main(){ v = p*0.5+0.5; v.y = 1.0 - v.y; gl_Position = vec4(p,0.0,1.0); }";
static const char *FS =
    "precision mediump float; varying vec2 v; uniform sampler2D t;"
    "void main(){ gl_FragColor = texture2D(t, v).bgra; }";

int wl_init(int width, int height) {
    (void)width; (void)height;
    g_disp = wl_display_connect(NULL);
    if (!g_disp) { seterr("wl_display_connect failed"); return -1; }
    struct wl_registry *reg = wl_display_get_registry(g_disp);
    wl_registry_add_listener(reg, &reg_l, NULL);
    wl_display_roundtrip(g_disp);
    if (!g_comp || !g_shell) { seterr("missing compositor/shell globals"); return -1; }
    if (g_seat) {
        wl_seat_add_listener(g_seat, &seat_l, NULL);
        wl_display_roundtrip(g_disp);
    }

    g_surf = wl_compositor_create_surface(g_comp);
    struct wl_shell_surface *ss = wl_shell_get_shell_surface(g_shell, g_surf);
    wl_shell_surface_set_fullscreen(ss, 0, 0, NULL);
    wl_shell_surface_set_class(ss, "org.qt-project.qmlscene");

    g_win = wl_egl_window_create(g_surf, DISP_W, DISP_H);
    g_ed = eglGetDisplay((EGLNativeDisplayType)g_disp);
    if (!eglInitialize(g_ed, NULL, NULL)) { seterr("eglInitialize 0x%x", eglGetError()); return -1; }
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLConfig cfg; EGLint nc = 0;
    static const EGLint cfga[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
    if (!eglChooseConfig(g_ed, cfga, &cfg, 1, &nc) || nc < 1) { seterr("eglChooseConfig"); return -1; }
    static const EGLint ctxa[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    g_ctx = eglCreateContext(g_ed, cfg, EGL_NO_CONTEXT, ctxa);
    g_es = eglCreateWindowSurface(g_ed, cfg, (EGLNativeWindowType)g_win, NULL);
    if (!eglMakeCurrent(g_ed, g_es, g_es, g_ctx)) { seterr("eglMakeCurrent 0x%x", eglGetError()); return -1; }

    g_prog = glCreateProgram();
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &VS, NULL); glCompileShader(vs); glAttachShader(g_prog, vs);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &FS, NULL); glCompileShader(fs); glAttachShader(g_prog, fs);
    glLinkProgram(g_prog); glUseProgram(g_prog);
    static const float quad[] = { -1,-1, 1,-1, -1,1, 1,1 };
    g_ap = glGetAttribLocation(g_prog, "p");
    glEnableVertexAttribArray(g_ap);
    glVertexAttribPointer(g_ap, 2, GL_FLOAT, 0, 0, quad);
    GLint utex = glGetUniformLocation(g_prog, "t");
    glUniform1i(utex, 0);
    glViewport(0, 0, DISP_W, DISP_H);

    glGenTextures(1, &g_tex);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    g_fb = malloc((size_t)DISP_W * DISP_H * 4);
    memset(g_fb, 0, (size_t)DISP_W * DISP_H * 4);
    fprintf(stderr, "[wl] EGL path up (libwayland + WSEGL -> eagle_shm)\n");
    return 0;
}

uint32_t *wl_framebuffer(void) { return g_fb; }

void wl_commit(void) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, DISP_W, DISP_H, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, g_fb);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    eglSwapBuffers(g_ed, g_es);
}

void wl_dispatch(void) { wl_display_dispatch_pending(g_disp); }
int wl_wait_frame(int timeout_ms) {
    (void)timeout_ms;
    wl_display_dispatch_pending(g_disp);
    return 1;
}

void wl_shutdown(void) {
    if (g_ed) eglMakeCurrent(g_ed, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (g_disp) wl_display_disconnect(g_disp);
    g_disp = NULL;
}
