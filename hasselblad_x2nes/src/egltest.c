/* egltest - minimal wayland+EGL fullscreen client for the X2D II.
 * Uses libwayland-client + wl_egl_window + EGL/GLES2 (same path as the
 * stock GUI; the stock wl_shm path never lights up the eagle pipeline).
 * Draws a moving color quad: if this shows on screen, x2nes switches to
 * the same rendering path. */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-egl.h>

static struct wl_display *disp;
static struct wl_compositor *comp;
static struct wl_shell *shell;
static int have_comp, have_shell;

static void registry_global(void *data, struct wl_registry *r, uint32_t name,
                            const char *iface, uint32_t ver) {
    (void)data;
    if (!strcmp(iface, "wl_compositor")) comp = wl_registry_bind(r, name, &wl_compositor_interface, 1), have_comp = 1;
    else if (!strcmp(iface, "wl_shell")) shell = wl_registry_bind(r, name, &wl_shell_interface, 1), have_shell = 1;
}
static void registry_global_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d;(void)r;(void)n; }
static const struct wl_registry_listener reg_l = { registry_global, registry_global_remove };

static const char *VS =
    "attribute vec2 p; varying vec2 v;"
    "void main(){ v = p*0.5+0.5; gl_Position = vec4(p,0.0,1.0); }";
static const char *FS =
    "precision mediump float; varying vec2 v; uniform float t;"
    "void main(){ gl_FragColor = vec4(v.x, v.y, 0.5+0.5*sin(t), 1.0); }";

int main(void) {
    disp = wl_display_connect(NULL);
    if (!disp) { fprintf(stderr, "egltest: wl_display_connect failed\n"); return 1; }
    struct wl_registry *reg = wl_display_get_registry(disp);
    wl_registry_add_listener(reg, &reg_l, NULL);
    wl_display_roundtrip(disp);
    if (!have_comp || !shell) { fprintf(stderr, "egltest: missing globals\n"); return 1; }

    struct wl_surface *surf = wl_compositor_create_surface(comp);
    struct wl_shell_surface *ss = wl_shell_get_shell_surface(shell, surf);
    wl_shell_surface_set_fullscreen(ss, 0, 0, NULL);
    wl_shell_surface_set_class(ss, "org.qt-project.qmlscene");

    struct wl_egl_window *win = wl_egl_window_create(surf, 1024, 768);
    EGLDisplay ed = eglGetDisplay((EGLNativeDisplayType)disp);
    if (!eglInitialize(ed, NULL, NULL)) { fprintf(stderr, "egltest: eglInitialize failed 0x%x\n", eglGetError()); return 1; }
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLConfig cfg;
    EGLint nc = 0;
    static const EGLint cfgattr[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
    if (!eglChooseConfig(ed, cfgattr, &cfg, 1, &nc) || nc < 1) { fprintf(stderr, "egltest: no config\n"); return 1; }
    static const EGLint ctxattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(ed, cfg, EGL_NO_CONTEXT, ctxattr);
    EGLSurface es = eglCreateWindowSurface(ed, cfg, (EGLNativeWindowType)win, NULL);
    eglMakeCurrent(ed, es, es, ctx);
    fprintf(stderr, "egltest: EGL up (ctx=%p surf=%p) - drawing\n", ctx, es);

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
    GLint ut = glGetUniformLocation(prog, "t");
    glViewport(0, 0, 1024, 768);

    uint64_t f = 0;
    uint64_t t0 = time(NULL);
    for (;;) {
        glUniform1f(ut, (float)(f % 360));
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        eglSwapBuffers(ed, es);
        wl_display_flush(disp);
        f++;
        if (time(NULL) - t0 >= 30) break; /* auto-exit after 30s */
        usleep(16666);
    }
    fprintf(stderr, "egltest: done after %llu frames\n", (unsigned long long)f);
    return 0;
}
