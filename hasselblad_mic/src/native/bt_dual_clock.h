/* Serialize the factory clock handler while lending it only its peer's latch.
 * The native parser, replies and retry completion still execute unchanged.
 * No fabricated incoming packets or writes to native completion flags.
 * Both incoming-request and outgoing-request paths establish real local time.
 */
#ifndef MIC_BT_DUAL_CLOCK_H
#define MIC_BT_DUAL_CLOCK_H
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <dlfcn.h>
#include "bt_resolver.h"
#include "arm64_hook.h"

struct mic_clock_peer {
    uint8_t bd[6];
    uint32_t transport; /* DG table index plus native handle; zero = absent */
    uint64_t latch, updated_ms;
};
static struct {
    pthread_mutex_t mutex;
    int initialized, failed;
    uint8_t *dg;
    uint64_t *latch;
    void (*receive)(const uint8_t *, const uint8_t *, int);
    int (*send)(const uint8_t *, int);
    int (*system_time)(uint64_t *);
    struct mic_clock_peer peer[2];
} mic_clock;

static uint64_t mic_clock_now(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static uint32_t mic_clock_transport(const uint8_t *bd)
{
    uint32_t found = 0;
    if (!bd || !mic_clock.dg) return 0;
    for (unsigned i = 0; i < 20; i++) {
        const uint8_t *p = mic_clock.dg + i * 32;
        uint8_t a[12], b[12];
        /* Do not keep a snapshot spanning a native open/close callback. */
        for (unsigned j = 0; j < sizeof a; j++) a[j] = __atomic_load_n(p + j, __ATOMIC_ACQUIRE);
        for (unsigned j = 0; j < sizeof b; j++) b[j] = __atomic_load_n(p + j, __ATOMIC_ACQUIRE);
        if (memcmp(a, b, sizeof a) || !a[0] || !a[1] || memcmp(a + 3, bd, 6)) continue;
        if (found) return 0; /* ambiguous duplicate connection, never guess */
        found = ((i + 1) << 16) | a[10] | ((uint32_t)a[11] << 8);
    }
    return found;
}

/* mutex held, including across the native handler's nested send(2). */
static struct mic_clock_peer *mic_clock_peer_for(const uint8_t *bd)
{
    if (!bd) return NULL;
    uint32_t transport = mic_clock_transport(bd);
    struct mic_clock_peer *free_peer = NULL;
    uint64_t now = mic_clock_now();
    for (unsigned i = 0; i < 2; i++) {
        struct mic_clock_peer *p = &mic_clock.peer[i];
        if (p->transport && p->transport != mic_clock_transport(p->bd)) memset(p, 0, sizeof *p);
        if (!p->transport) { free_peer = p; continue; }
        if (!memcmp(p->bd, bd, 6)) {
            /* Native requests retry every 2s and close after three tries.
             * A latch older than 10s cannot belong to that pending exchange. */
            if (now < p->updated_ms || now - p->updated_ms > 10000) p->latch = 0;
            return p;
        }
    }
    if (!transport || !free_peer) return NULL;
    memcpy(free_peer->bd, bd, 6);
    free_peer->transport = transport;
    return free_peer;
}

static int mic_clock_send(const uint8_t *bd, int subtype)
{
    int saved = errno;
    pthread_mutex_lock(&mic_clock.mutex);
    if (subtype == 1) {
        struct mic_clock_peer *p = mic_clock_peer_for(bd);
        if (p) {
            uint64_t stamp = 0;
            mic_clock.system_time(&stamp); /* exact factory clock source/units */
            p->latch = stamp;
            p->updated_ms = mic_clock_now();
        }
    }
    errno = saved;
    int result = __atomic_load_n(&mic_clock.send, __ATOMIC_ACQUIRE)(bd, subtype);
    saved = errno;
    pthread_mutex_unlock(&mic_clock.mutex);
    errno = saved;
    return result;
}

static void mic_clock_receive(const uint8_t *bd, const uint8_t *data, int length)
{
    int saved = errno, exchange = 0;
    if (data && length >= 13 && data[0] == 0x55 && data[4] == 0x3a &&
        data[5] == 7 && data[9] == 0x12 && data[10] == 0x58) {
        unsigned declared = (data[1] | ((unsigned)data[2] << 8)) & 1023;
        if (declared < 13 || declared > (unsigned)length) return;
        unsigned payload = declared - 13;
        if (payload >= 2 && data[11] == 0) {
            if (data[12] == 1) exchange = 1;
            if (data[12] == 2 && payload >= 8 &&
                (data[13] || data[14] || data[15] || data[16])) exchange = 2;
        }
    }
    pthread_mutex_lock(&mic_clock.mutex);
    struct mic_clock_peer *p = exchange ? mic_clock_peer_for(bd) : NULL;
    uint32_t transport = p ? p->transport : 0;
    *mic_clock.latch = p ? p->latch : 0;
    errno = saved;
    __atomic_load_n(&mic_clock.receive, __ATOMIC_ACQUIRE)(bd, data, length);
    saved = errno;
    if (p && transport == mic_clock_transport(bd)) {
        p->latch = *mic_clock.latch;
        if (exchange == 1) p->updated_ms = mic_clock_now();
    }
    *mic_clock.latch = 0; /* no other peer can inherit this call's time */
    pthread_mutex_unlock(&mic_clock.mutex);
    errno = saved;
}

/* Startup is serialized by miccore's lifecycle mutex. Hooks are permanent;
 * failure after one installation is reported, never undone while reachable. */
static int mic_clock_install(void *base, const struct mic_bt_abi *abi)
{
    if (mic_clock.initialized) return mic_clock.failed ? -1 : 0;
    if (!base || !abi || !abi->clock_latch || !abi->dg_table) return -1;
    mic_clock.system_time = (int (*)(uint64_t *))dlsym(RTLD_DEFAULT, "duss_osal_get_system_time");
    if (!mic_clock.system_time) return -1;
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr)) return -1;
    int rc = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    if (!rc) rc = pthread_mutex_init(&mic_clock.mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    if (rc) return -1;
    mic_clock.dg = (uint8_t *)base + abi->dg_table;
    mic_clock.latch = (uint64_t *)((uint8_t *)base + abi->clock_latch);
    mic_clock.initialized = 1;
    mic_clock.failed = 1;
    rc = mic_hook_install((uint8_t *)base + abi->offset[MIC_ABI_CLOCK_SEND], sig_CLOCK_SEND,
                         (void *)mic_clock_send, (void **)&mic_clock.send);
    if (rc < 0) return rc;
    int warning = rc;
    rc = mic_hook_install((uint8_t *)base + abi->offset[MIC_ABI_CLOCK_RECV], sig_CLOCK_RECV,
                         (void *)mic_clock_receive, (void **)&mic_clock.receive);
    if (rc < 0) return rc;
    mic_clock.failed = 0;
    return warning || rc;
}
#endif
