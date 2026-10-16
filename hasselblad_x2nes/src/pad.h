#ifndef X2NES_PAD_H
#define X2NES_PAD_H

#include <stdint.h>

/* NES button bits (standard order, core-facing) */
enum {
    PAD_A = 1 << 0,
    PAD_B = 1 << 1,
    PAD_SELECT = 1 << 2,
    PAD_START = 1 << 3,
    PAD_UP = 1 << 4,
    PAD_DOWN = 1 << 5,
    PAD_LEFT = 1 << 6,
    PAD_RIGHT = 1 << 7,
};

/* Compute NES pad bits from current touch points. */
uint8_t pad_scan(const int *xs, const int *ys, int n);

/* Draw the overlay (pressed buttons highlighted). */
void pad_draw(uint32_t *px, int w, int h, uint8_t pressed);

/* 1 if the on-screen EXIT pad button is held (leave). */
int pad_exit_pressed(void);

#endif
