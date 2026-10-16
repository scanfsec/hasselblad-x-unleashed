#ifndef X2NES_SCALER_H
#define X2NES_SCALER_H

#include <stdint.h>

/* Scale NES 256x240 XRGB frame to the display framebuffer. Arbitrary
 * integer/fractional ratios via per-row/col source LUTs (1024x768 from
 * 256x240 => x*4 exact, y*16/5 pattern). */
void scaler_init(int dst_w, int dst_h);
void scaler_blit(uint32_t *dst, const uint32_t *src256x240);

#endif
