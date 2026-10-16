#include "netpad.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>

struct __attribute__((packed)) np_packet {
    unsigned magic;
    unsigned short seq;
    unsigned short btn;
    short ax[4];
};

static int np_sock = -1;
static unsigned short np_btn;
static short np_ax[4];
static unsigned long long np_last_ms;

static unsigned long long np_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + ts.tv_nsec / 1000000ull;
}

int netpad_init(void) {
    np_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (np_sock < 0) return -1;
    /* x2nes<->xdoom switch via fork+execl: without this the child inherits
     * the bound socket and its own netpad_init fails with EADDRINUSE,
     * silently killing pad input in the new process */
    fcntl(np_sock, F_SETFD, FD_CLOEXEC);
    fcntl(np_sock, F_SETFL, fcntl(np_sock, F_GETFL, 0) | O_NONBLOCK);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(NETPAD_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(np_sock, (struct sockaddr*)&a, sizeof a) < 0) {
        close(np_sock);
        np_sock = -1;
        return -1;
    }
    return 0;
}

void netpad_poll(void) {
    if (np_sock < 0) return;
    /* capped so a pathological error can never wedge the game loop */
    for (int drained = 0; drained < 64; drained++) {
        struct np_packet p;
        ssize_t n = recv(np_sock, &p, sizeof p, 0);
        if (n != (ssize_t)sizeof p) {
            if (n >= 0) continue;         /* runt/garbage: keep draining */
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            static int logged;
            if (!logged) {
                logged = 1;
                write(2, "netpad: recv error, source disabled\n", 36);
            }
            np_sock = -1;                 /* hard error: disable source */
            break;
        }
        if (p.magic != NETPAD_MAGIC) continue;
        np_btn = p.btn;
        memcpy(np_ax, p.ax, sizeof np_ax);
        np_last_ms = np_now_ms();
    }
}

unsigned netpad_buttons(void) { return np_btn; }

int netpad_norm(int axis) {
    if (axis < 0 || axis > 3) return 0;
    /* -32768..32767 -> -512..511, same scale as input.c evdev sticks */
    return (int)((int)np_ax[axis] * 512 / 32768);
}

int netpad_alive(void) {
    return np_last_ms != 0 && (np_now_ms() - np_last_ms) < 500;
}
