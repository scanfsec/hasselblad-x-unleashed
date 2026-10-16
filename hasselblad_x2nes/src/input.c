#include "input.h"
#include "util.h"

#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MAX_DEV 8
#define MAX_SLOTS 8
#define MAX_TOUCH 10

struct dev {
    int fd;
    char name[64];
    int is_touch;
};

static struct dev devs[MAX_DEV];
static int n_dev;

/* type-B slot state */
struct slot {
    int tracking_id;   /* -1 = up */
    int x, y;
    int updated;
};
static struct slot slots[MAX_SLOTS];
static int cur_slot;

static x2_touch_t touches[MAX_TOUCH];
static int n_touch;

static uint8_t keys[KEY_MAX / 8 + 1];
static int rel_state[REL_MAX + 1];

/* analog axes (EV_ABS), normalized to -512..511 (0 = center)
 * DualSense: ABS_X/Y = left stick, ABS_Z/RZ = right stick,
 * ABS_HAT0X/Y = dpad hat */
static int abs_val[ABS_MAX + 1];
static int abs_center[ABS_MAX + 1];
static int abs_range[ABS_MAX + 1];

static void open_one(const char *path) {
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return;
    char name[64] = {0};
    ioctl(fd, EVIOCGNAME(sizeof name - 1), name);
    int is_touch = 0;
    uint8_t abs_bits[ABS_MAX / 8 + 1] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs_bits), abs_bits) >= 0) {
        if (abs_bits[ABS_MT_POSITION_X / 8] & (1 << (ABS_MT_POSITION_X % 8)))
            is_touch = 1;
    }
    /* name-based fallback: FocalTech fts_ts etc. */
    if (!is_touch && (strstr(name, "ts") || strstr(name, "touch") || strstr(name, "fts")))
        is_touch = 1;
    /* skip nothing else for now: keep buttons/key devices too */
    if (n_dev < MAX_DEV) {
        devs[n_dev].fd = fd;
        devs[n_dev].is_touch = is_touch;
        snprintf(devs[n_dev].name, sizeof devs[n_dev].name, "%s", name);
        n_dev++;
        /* gamepad? calibrate axes so reads start centered */
        unsigned char key_bits[(KEY_MAX / 8) + 1] = {0};
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof key_bits), key_bits) >= 0) {
            if (key_bits[BTN_SOUTH / 8] & (1 << (BTN_SOUTH % 8))) {
                for (int ax = 0; ax <= ABS_MAX; ax++) {
                    struct input_absinfo ai;
                    if (ioctl(fd, EVIOCGABS(ax), &ai) >= 0 && ai.maximum > ai.minimum) {
                        abs_center[ax] = (ai.maximum + ai.minimum) / 2;
                        abs_range[ax] = (ai.maximum - ai.minimum) / 2;
                        abs_val[ax] = 0;
                    }
                }
            }
        }
    } else {
        close(fd);
    }
}

int input_init(void) {
    n_dev = 0;
    for (int i = 0; i < 32; i++) {
        char path[32];
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        open_one(path);
    }
    for (int i = 0; i < MAX_SLOTS; i++) slots[i].tracking_id = -1;
    cur_slot = 0;
    n_touch = 0;
    memset(keys, 0, sizeof keys);
    memset(rel_state, 0, sizeof rel_state);
    return n_dev;
}

void input_dump_devices(void) {
    for (int i = 0; i < n_dev; i++)
        fprintf(stderr, "[input] dev%d fd=%d touch=%d name='%s'\n",
                i, devs[i].fd, devs[i].is_touch, devs[i].name);
}

static void apply_slots(void) {
    n_touch = 0;
    for (int i = 0; i < MAX_SLOTS && n_touch < MAX_TOUCH; i++) {
        if (slots[i].tracking_id != -1) {
            touches[n_touch].active = 1;
            touches[n_touch].x = slots[i].x;
            touches[n_touch].y = slots[i].y;
            n_touch++;
        }
    }
}

static void handle_event(struct dev *d, const struct input_event *ev) {
    if (ev->type == EV_SYN && ev->code == SYN_REPORT) {
        if (d->is_touch) apply_slots();
        return;
    }
    if (ev->type == EV_ABS) {
        switch (ev->code) {
        case ABS_MT_SLOT:
            if (ev->value >= 0 && ev->value < MAX_SLOTS) cur_slot = ev->value;
            break;
        case ABS_MT_TRACKING_ID:
            slots[cur_slot].tracking_id = ev->value;
            break;
        case ABS_MT_POSITION_X:
            slots[cur_slot].x = ev->value;
            break;
        case ABS_MT_POSITION_Y:
            slots[cur_slot].y = ev->value;
            break;
        case ABS_X:
            /* single-touch fallback drivers */
            if (cur_slot == 0) slots[0].x = ev->value;
            break;
        case ABS_Y:
            if (cur_slot == 0) slots[0].y = ev->value;
            break;
        default:
            break;
        }
    } else if (ev->type == EV_KEY) {
        if (ev->code < KEY_MAX) {
            if (ev->value) keys[ev->code / 8] |= (uint8_t)(1 << (ev->code % 8));
            else keys[ev->code / 8] &= (uint8_t)~(1 << (ev->code % 8));
        }
    } else if (ev->type == EV_REL) {
        if (ev->code <= REL_MAX) rel_state[ev->code] += ev->value;
    } else if (ev->type == EV_ABS) {
        if (ev->code <= ABS_MAX && abs_range[ev->code] > 0)
            abs_val[ev->code] = ((ev->value - abs_center[ev->code]) * 512) / abs_range[ev->code];
        else if (ev->code <= ABS_MAX)
            abs_val[ev->code] = ev->value ? (ev->value > 0 ? 512 : -512) : 0;
    }
}

void input_poll(void) {
    struct input_event evs[64];
    for (int i = 0; i < n_dev; i++) {
        for (;;) {
            ssize_t n = read(devs[i].fd, evs, sizeof evs);
            if (n <= 0) break;
            int cnt = (int)(n / sizeof(struct input_event));
            for (int k = 0; k < cnt; k++) handle_event(&devs[i], &evs[k]);
        }
    }
}

const x2_touch_t *input_touches(int *n) {
    *n = n_touch;
    return touches;
}

int input_key(int code) {
    if (code >= KEY_MAX) return 0;
    return (keys[code / 8] >> (code % 8)) & 1;
}

int input_rel_consumed(int axis) {
    if (axis > REL_MAX) return 0;
    int v = rel_state[axis];
    rel_state[axis] = 0;
    return v;
}

/* analog stick: -512..511, 0 = center */
int input_abs(int axis) {
    if (axis < 0 || axis > ABS_MAX) return 0;
    return abs_val[axis];
}
