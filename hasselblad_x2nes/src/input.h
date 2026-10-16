#ifndef X2NES_INPUT_H
#define X2NES_INPUT_H

/* evdev input: FocalTech FTS touchscreen (type-B multitouch, coord space
 * 1024x768) + exmcu_input hardware buttons + wheels. */
#include <stdint.h>

typedef struct {
    int active;
    int x, y;
} x2_touch_t;

/* Scan /dev/input/event* and open touchscreen + button devices.
 * Returns number of devices opened (0 = none, gamepad falls back to nothing). */
int input_init(void);

/* Non-blocking read of all devices; updates touches + key/wheel state. */
void input_poll(void);

/* Current snapshot: up to 10 touch points. */
const x2_touch_t *input_touches(int *n);

/* Key state by evdev code (1 = pressed). Wheels: REL deltas since last poll. */
int input_key(int code);
int input_rel_consumed(int axis);

/* analog stick axis (ABS_X..), normalized -512..511, 0 = center */
int input_abs(int axis);

/* Human-readable dump of what was opened (for NOTES.md / debugging). */
void input_dump_devices(void);

#endif
