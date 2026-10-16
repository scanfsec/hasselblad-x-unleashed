/*
 * readhook.c -- stage-2 preload skeleton (COMPILE-ONLY for stage 1).
 *
 * Stage 2 plan: bt_bsa_app is started with LD_PRELOAD=readhook.so so the
 * engine is guaranteed present at boot (no frida dependency). The hook:
 *   1. constructor: dlopen("libmiccore.so") -> its constructor starts the
 *      engine (FIFO / status / control / auto-connect) exactly as in
 *      stage 1; zero engine changes needed.
 *   2. interpose connect(): remember fds that connect to
 *      /data/misc/bluetooth/bt-dg-socket* (the SPP data channel).
 *   3. interpose read()/recv(): after the real call returns N>0 on such
 *      an fd, hand the buffer to miccore_feed(buf, N) -- same bytes the
 *      frida hook saw at app_audio_play_data.
 *
 * NOT DEPLOYED in stage 1 (tools/build.sh compiles it only).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "miccore.h"

#define MAX_FD 1024
static unsigned char dg_fd[MAX_FD];       /* 1 = bt-dg-socket fd */

static void (*real_connect)(void);
static ssize_t (*real_read)(int, void *, size_t);
static ssize_t (*real_recv)(int, void *, size_t, int);

/* dlsym(RTLD_NEXT) of variadic connect() needs the real signature */
int connect(int fd, const struct sockaddr *addr, socklen_t len)
{
    static int (*next)(int, const struct sockaddr *, socklen_t);
    if (!next) {
        next = (int (*)(int, const struct sockaddr *, socklen_t))dlsym(RTLD_NEXT, "connect");
        if (!next) return -1;
    }
    int rc = next(fd, addr, len);
    if (rc == 0 && addr && addr->sa_family == AF_UNIX && fd >= 0 && fd < MAX_FD) {
        const struct sockaddr_un *u = (const struct sockaddr_un *)addr;
        if (strstr(u->sun_path, "bt-dg-socket"))
            dg_fd[fd] = 1;
    }
    return rc;
}

static void maybe_feed(int fd, void *buf, ssize_t n)
{
    if (n > 0 && fd >= 0 && fd < MAX_FD && dg_fd[fd])
        miccore_feed((const uint8_t *)buf, (int)n);
}

ssize_t read(int fd, void *buf, size_t n)
{
    if (!real_read)
        real_read = (ssize_t (*)(int, void *, size_t))dlsym(RTLD_NEXT, "read");
    ssize_t r = real_read(fd, buf, n);
    maybe_feed(fd, buf, r);
    return r;
}

ssize_t recv(int fd, void *buf, size_t n, int flags)
{
    if (!real_recv)
        real_recv = (ssize_t (*)(int, void *, size_t, int))dlsym(RTLD_NEXT, "recv");
    ssize_t r = real_recv(fd, buf, n, flags);
    maybe_feed(fd, buf, r);
    return r;
}

__attribute__((constructor)) static void readhook_ctor(void)
{
    const char *so = "/data/mic/libmiccore.so";
    /* RTLD_GLOBAL (0x100|RTLD_NOW): the engine's exported __assert_fail
     * stub must be visible to liblc3plus_b.so which the engine dlopens */
    void *h = dlopen(so, RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        fprintf(stderr, "readhook: dlopen %s: %s\n", so, dlerror());
        return;
    }
    /* miccore's own constructor already ran miccore_start() */
    fprintf(stderr, "readhook: engine loaded\n");
    (void)real_connect;
}
