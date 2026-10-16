/* x2nes - minimal hand-rolled Wayland client (wl_shm only, no libwayland)
 *
 * Target: Hasselblad X2D II 100C (weston 12.0.9 + eagle-shell).
 * eagle-shell rules learned from firmware strings:
 *   - surface must match output size (Back = 1024x768) or it stays in
 *     the "staging" layer and is never shown.
 *   - wl_shell_surface.set_class strcmp list contains "org.qt-project.qmlscene"
 *     -> we reuse that class so the shell treats us as a known citizen.
 */
#ifndef X2NES_WL_H
#define X2NES_WL_H

#include <stdint.h>

/* Connect to $XDG_RUNTIME_DIR/$WAYLAND_DISPLAY, bind wl_compositor/wl_shm/
 * wl_shell/wl_output, create a double-buffered wl_shm surface of the given
 * size and put it fullscreen on the output that reports exactly that mode.
 * Returns 0 on success. */
int wl_init(int width, int height);

/* Pixel buffer (width*height XRGB8888, stride = width*4) for the frame that
 * is safe to draw into now. Valid until wl_commit(). */
uint32_t *wl_framebuffer(void);

/* damage whole surface, attach, commit, request frame callback. */
void wl_commit(void);

/* Block up to timeout_ms processing events; returns 1 when the frame
 * callback fired (i.e. next frame may be drawn), 0 on timeout. */
int wl_wait_frame(int timeout_ms);

/* Non-blocking dispatch of pending events (buffer releases etc.). */
void wl_dispatch(void);

/* ---- input: wl_seat/wl_touch (we are the focused fullscreen client) ---- */
typedef struct {
    int id;   /* wayland touch point id */
    int x, y; /* surface-local pixel coords */
} wl_touch_t;

/* Process pending input events; returns current touch points (<=5). */
const wl_touch_t *wl_touches(int *n);

/* Last known touch count changed since previous call? (for UI) */
int wl_touch_version(void);

/* linux evdev keycode state from wl_keyboard */
int wl_key_down(int code);

/* Last wl_display error string, or NULL. */
const char *wl_error(void);

void wl_shutdown(void);

#endif
