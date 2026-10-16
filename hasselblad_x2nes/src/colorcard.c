/* colorcard - X2D II 屏幕色卡检测 (egltest 骨架 + evdev 任意键步进)
 *
 * 全屏纯色逐张显示；按任意键(实体键/触摸/拨轮)切下一色；
 * 最后一张(黑)后再按键即退出，由外层 wrapper 恢复 GUI。
 * 屏幕上不画任何文字——色卡必须纯色才查得了坏点/色斑。
 * 颜色名打到 stderr 供 rsh 侧记录。
 */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/input.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#define SCR_W 1024
#define SCR_H 768
#define MAX_DEV 12
#define DEBOUNCE_MS 350
#define MAX_OUT 4

static struct wl_display *disp;
static struct wl_compositor *comp;
static struct wl_shell *shell;
static int have_comp, have_shell;

/* ---- 输出枚举(后屏/EVF) ---- */
struct outinfo {
    struct wl_output *out;
    uint32_t name;
    int32_t pw, ph;          /* 物理尺寸 mm */
    int32_t w, h;            /* 当前模式 */
    int nmode;
    int have_mode;
};
static struct outinfo outs[MAX_OUT];
static int n_out;

static void out_geometry(void *d, struct wl_output *o, int32_t x, int32_t y,
                         int32_t pw, int32_t ph, int32_t sub, const char *make,
                         const char *model, int32_t tr) {
    (void)d;(void)o;(void)x;(void)y;(void)sub;(void)make;(void)model;(void)tr;
    for (int i = 0; i < n_out; i++) if (outs[i].out == o) { outs[i].pw = pw; outs[i].ph = ph; }
}
static void out_mode(void *d, struct wl_output *o, uint32_t flags, int32_t w,
                     int32_t h, int32_t refresh) {
    (void)d;(void)refresh;
    for (int i = 0; i < n_out; i++) if (outs[i].out == o) {
        outs[i].nmode++;
        if (flags & WL_OUTPUT_MODE_CURRENT) { outs[i].w = w; outs[i].h = h; outs[i].have_mode = 1; }
    }
}
static void out_done(void *d, struct wl_output *o) { (void)d;(void)o; }
static void out_scale(void *d, struct wl_output *o, int32_t s) { (void)d;(void)o;(void)s; }
static const struct wl_output_listener out_l = { out_geometry, out_mode, out_done, out_scale };

static void registry_global(void *data, struct wl_registry *r, uint32_t name,
                            const char *iface, uint32_t ver) {
    (void)data; (void)ver;
    if (!strcmp(iface, "wl_compositor")) { comp = wl_registry_bind(r, name, &wl_compositor_interface, 1); have_comp = 1; }
    else if (!strcmp(iface, "wl_shell")) { shell = wl_registry_bind(r, name, &wl_shell_interface, 1); have_shell = 1; }
    else if (!strcmp(iface, "wl_output") && n_out < MAX_OUT) {
        struct outinfo *oi = &outs[n_out];
        oi->out = wl_registry_bind(r, name, &wl_output_interface, 2);
        oi->name = name;
        wl_output_add_listener(oi->out, &out_l, NULL);
        n_out++;
    }
}
static void registry_global_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d;(void)r;(void)n; }
static const struct wl_registry_listener reg_l = { registry_global, registry_global_remove };

static const char *VS =
    "attribute vec2 p;"
    "void main(){ gl_Position = vec4(p, 0.0, 1.0); }";
static const char *FS =
    "precision mediump float; uniform vec3 c;"
    "void main(){ gl_FragColor = vec4(c, 1.0); }";

typedef struct { const char *name; float r, g, b; } card_t;
static const card_t cards[] = {
    { "RED    纯红 255,0,0",       1.0f, 0.0f, 0.0f },
    { "GREEN  纯绿 0,255,0",       0.0f, 1.0f, 0.0f },
    { "BLUE   纯蓝 0,0,255",       0.0f, 0.0f, 1.0f },
    { "CYAN   青 0,255,255",       0.0f, 1.0f, 1.0f },
    { "MAGENTA 品红 255,0,255",    1.0f, 0.0f, 1.0f },
    { "YELLOW 黄 255,255,0",       1.0f, 1.0f, 0.0f },
    { "WHITE  白 255,255,255",     1.0f, 1.0f, 1.0f },
    { "GRAY25 灰 64,64,64",        0.25f, 0.25f, 0.25f },
    { "GRAY50 灰 128,128,128",     0.5f, 0.5f, 0.5f },
    { "GRAY75 灰 191,191,191",     0.75f, 0.75f, 0.75f },
    { "BLACK  黑 0,0,0 (退出色)",  0.0f, 0.0f, 0.0f },
};
#define NCARDS ((int)(sizeof cards / sizeof cards[0]))

/* 灰阶序列: 纯黑 + 8bit 等值RGB 1/2/4/8/16/32(+64 对照) */
static int levels[] = { 0, 1, 2, 4, 8, 16, 32, 64 };
#define NLEV ((int)(sizeof levels / sizeof levels[0]))
static int gray_mode;

static int ifds[MAX_DEV], n_ifd;

static void open_inputs(void) {
    char path[32];
    for (int i = 0; i < MAX_DEV; i++) {
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) ifds[n_ifd++] = fd;
    }
    fprintf(stderr, "colorcard: %d input devices\n", n_ifd);
}

static void drain_inputs(void) {
    struct input_event ev;
    for (int i = 0; i < n_ifd; i++)
        while (read(ifds[i], &ev, sizeof ev) == (int)sizeof ev) {}
}

static uint64_t now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "gray")) gray_mode = 1;

    disp = wl_display_connect(NULL);
    if (!disp) { fprintf(stderr, "colorcard: wl_display_connect failed\n"); return 1; }
    struct wl_registry *reg = wl_display_get_registry(disp);
    wl_registry_add_listener(reg, &reg_l, NULL);
    wl_display_roundtrip(disp);
    wl_display_roundtrip(disp);   /* 输出 geometry/mode 二段到达 */
    if (!have_comp || !shell) { fprintf(stderr, "colorcard: missing globals\n"); return 1; }

    fprintf(stderr, "colorcard: %d wl_output(s)\n", n_out);
    /* 每个输出建一个全屏 surface(后屏+EVF 同时同色) */
    struct { struct wl_surface *surf; struct wl_shell_surface *ss;
             struct wl_egl_window *win; EGLSurface es; int w, h; } S[MAX_OUT] = {{0}};
    int n_surf = 0;
    for (int i = 0; i < n_out && n_surf < MAX_OUT; i++) {
        int w = outs[i].have_mode ? outs[i].w : SCR_W;
        int h = outs[i].have_mode ? outs[i].h : SCR_H;
        fprintf(stderr, "  out[%d] name=%u %dx%d phys=%dx%dmm modes=%d\n",
                i, outs[i].name, outs[i].w, outs[i].h, outs[i].pw, outs[i].ph, outs[i].nmode);
        S[n_surf].surf = wl_compositor_create_surface(comp);
        S[n_surf].ss = wl_shell_get_shell_surface(shell, S[n_surf].surf);
        wl_shell_surface_set_fullscreen(S[n_surf].ss, 0, 0, outs[i].out);
        wl_shell_surface_set_class(S[n_surf].ss, "org.qt-project.qmlscene");
        S[n_surf].win = wl_egl_window_create(S[n_surf].surf, w, h);
        S[n_surf].w = w; S[n_surf].h = h;
        n_surf++;
    }
    if (n_surf == 0) {   /* 没枚举到输出(异常兜底): 无目标全屏 */
        S[0].surf = wl_compositor_create_surface(comp);
        S[0].ss = wl_shell_get_shell_surface(shell, S[0].surf);
        wl_shell_surface_set_fullscreen(S[0].ss, 0, 0, NULL);
        wl_shell_surface_set_class(S[0].ss, "org.qt-project.qmlscene");
        S[0].win = wl_egl_window_create(S[0].surf, SCR_W, SCR_H);
        S[0].w = SCR_W; S[0].h = SCR_H;
        n_surf = 1;
    }

    EGLDisplay ed = eglGetDisplay((EGLNativeDisplayType)disp);
    if (!eglInitialize(ed, NULL, NULL)) { fprintf(stderr, "colorcard: eglInitialize failed 0x%x\n", eglGetError()); return 1; }
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLConfig cfg; EGLint nc = 0;
    static const EGLint cfgattr[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
    if (!eglChooseConfig(ed, cfgattr, &cfg, 1, &nc) || nc < 1) { fprintf(stderr, "colorcard: no config\n"); return 1; }
    static const EGLint ctxattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(ed, cfg, EGL_NO_CONTEXT, ctxattr);
    for (int i = 0; i < n_surf; i++)
        S[i].es = eglCreateWindowSurface(ed, cfg, (EGLNativeWindowType)S[i].win, NULL);
    eglMakeCurrent(ed, S[0].es, S[0].es, ctx);

    GLuint prog = glCreateProgram();
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &VS, NULL); glCompileShader(vs); glAttachShader(prog, vs);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &FS, NULL); glCompileShader(fs); glAttachShader(prog, fs);
    glLinkProgram(prog); glUseProgram(prog);
    static const float quad[] = { -1,-1, 1,-1, -1,1, 1,1 };
    GLint ap = glGetAttribLocation(prog, "p");
    glEnableVertexAttribArray(ap);
    glVertexAttribPointer(ap, 2, GL_FLOAT, 0, 0, quad);
    GLint uc = glGetUniformLocation(prog, "c");

    open_inputs();

    const int total = gray_mode ? NLEV : NCARDS;
    int idx = 0;
    uint64_t t0 = now_ms();
    for (;;) {
        float r, g, b; const char *nm;
        if (gray_mode) {
            int v = levels[idx];
            nm = (v == 0) ? "BLACK  黑" : "GRAY";
            r = g = b = v / 255.0f;
            fprintf(stderr, "colorcard: [%d/%d] %s level=%d (8bit %.4f) t=%llums\n",
                    idx + 1, total, nm, v, r, (unsigned long long)(now_ms() - t0));
        } else {
            const card_t *c = &cards[idx];
            nm = c->name; r = c->r; g = c->g; b = c->b;
            fprintf(stderr, "colorcard: [%d/%d] %s t=%llums\n",
                    idx + 1, total, nm, (unsigned long long)(now_ms() - t0));
        }
        fflush(stderr);
        /* 持续渲染当前色到所有输出(~30fps)直到按键 */
        uint64_t t_last = now_ms();
        for (;;) {
            for (int i = 0; i < n_surf; i++) {
                eglMakeCurrent(ed, S[i].es, S[i].es, ctx);
                glViewport(0, 0, S[i].w, S[i].h);
                glUniform3f(uc, r, g, b);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                eglSwapBuffers(ed, S[i].es);
            }
            wl_display_flush(disp);
            /* 非阻塞收一次输入 */
            struct pollfd pf[MAX_DEV];
            struct input_event ev;
            int hit = 0;
            for (int i = 0; i < n_ifd; i++) { pf[i].fd = ifds[i]; pf[i].events = POLLIN; }
            if (poll(pf, n_ifd, 33) > 0) {
                for (int i = 0; i < n_ifd && !hit; i++) {
                    if (!(pf[i].revents & POLLIN)) continue;
                    while (read(ifds[i], &ev, sizeof ev) == (int)sizeof ev) {
                        if (now_ms() - t_last < DEBOUNCE_MS) continue;
                        if (ev.type == EV_KEY && ev.code != KEY_RESERVED &&
                            (ev.value == 1 || ev.code == BTN_TOUCH)) { hit = 1; break; }
                        if (ev.type == EV_REL && ev.code == REL_WHEEL && ev.value != 0) { hit = 1; break; }
                    }
                }
            }
            if (hit) {
                drain_inputs();
                usleep(120 * 1000);   /* 等按键松开的事件也被吞掉 */
                drain_inputs();
                break;
            }
        }
        idx++;
        if (idx >= total) break;   /* 最后一张显示完、按键 → 退出 */
    }
    fprintf(stderr, "colorcard: all %d done, exit\n", total);
    eglMakeCurrent(ed, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    return 0;
}
