#ifndef X2NES_UTIL_H
#define X2NES_UTIL_H

#include <stdint.h>

uint64_t now_ms(void);
void sleep_ms(int ms);

/* simple 2D drawing on XRGB8888 (0x00RRGGBB) buffers */
void u_fill_rect(uint32_t *px, int w, int h, int x, int y, int rw, int rh, uint32_t color);
void u_fill_circle(uint32_t *px, int w, int h, int cx, int cy, int r, uint32_t color);
void u_blend_rect(uint32_t *px, int w, int h, int x, int y, int rw, int rh, uint32_t color, int alpha); /* alpha 0..255 */
void u_blend_circle(uint32_t *px, int w, int h, int cx, int cy, int r, uint32_t color, int alpha);

#endif
