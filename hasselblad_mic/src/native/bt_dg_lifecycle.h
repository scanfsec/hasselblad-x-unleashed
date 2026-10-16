/* Native callers (HFP and network callbacks included) share one DG allocator.
 * Reuse each peer's open/pending client; serialize its native writers. Failed
 * OPEN events have handle 0xff, also the allocator's placeholder for EVERY
 * pending client. Make the original failure handler use its BD fallback,
 * never free whichever unrelated pending client happens to appear first.
 */
#ifndef MIC_BT_DG_LIFECYCLE_H
#define MIC_BT_DG_LIFECYCLE_H
#include <errno.h>
#include <pthread.h>
#include "bt_resolver.h"
#include "arm64_hook.h"
#include "../decoder/mic_dg_events.h"

static struct {
    pthread_mutex_t mutex;
    int initialized, failed;
    uint8_t *table;
    int (*open)(int, const uint8_t *);
    int (*listen)(void);
    void (*callback)(unsigned char, const void *);
} mic_dg;

static unsigned mic_dg_handle(const uint8_t *entry)
{
    return entry[10] | ((unsigned)entry[11] << 8);
}

static unsigned mic_dg_state(const uint8_t *bd)
{
    unsigned state = 0;
    for (unsigned i = 0; i < 20; i++) {
        const uint8_t *e = mic_dg.table + i * 32;
        if (e[0] && (!e[2] || e[1]) && !memcmp(e + 3, bd, 6)) state |= e[1] ? 2 : 1;
    }
    return state;
}

static int mic_dg_opened_handle(const uint8_t *bd, unsigned handle)
{
    for (unsigned i = 0; i < 20; i++) {
        const uint8_t *e = mic_dg.table + i * 32;
        if (e[0] && e[1] && mic_dg_handle(e) == handle && !memcmp(e + 3, bd, 6))
            return 1;
    }
    return 0;
}

/* All ordinary native DG writers are behind this recursive mutex: open,
 * listen and the callback dispatcher (both OPEN and CLOSE). Callbacks can
 * themselves request a listener, so a non-recursive mutex would deadlock. */
static int mic_dg_open(int exclusive, const uint8_t *bd)
{
    int saved = errno, result = 0, reuse = 0;
    pthread_mutex_lock(&mic_dg.mutex);
    if (!exclusive && bd) {
        for (unsigned i = 0; i < 20; i++) {
            const uint8_t *e = mic_dg.table + i * 32;
            if (e[0] && (!e[2] || e[1]) && !memcmp(e + 3, bd, 6)) {
                reuse = 1;
                break;
            }
        }
    }
    errno = saved;
    if (!reuse) result = __atomic_load_n(&mic_dg.open, __ATOMIC_ACQUIRE)(exclusive, bd);
    saved = errno;
    pthread_mutex_unlock(&mic_dg.mutex);
    errno = saved;
    return result;
}

static int mic_dg_listen(void)
{
    int saved = errno;
    pthread_mutex_lock(&mic_dg.mutex);
    errno = saved;
    int result = __atomic_load_n(&mic_dg.listen, __ATOMIC_ACQUIRE)();
    saved = errno;
    pthread_mutex_unlock(&mic_dg.mutex);
    errno = saved;
    return result;
}

static void mic_dg_callback(unsigned char event, const void *message)
{
    int saved = errno;
    uint8_t failed_open[14];
    pthread_mutex_lock(&mic_dg.mutex);
    const uint8_t *m = message;
    struct mic_dg_event notice = {0};
    int notify = 0;
    if (event == 0 && m) {
        memcpy(notice.bd, m + 2, 6);
        notice.kind = (m[0] || m[1]) ? MIC_DG_FAILED : MIC_DG_OPENED;
        notice.handle = mic_dg_handle(m);
        notify = 1;
    } else if (event == 1 && m) {
        notice.handle = m[0] | ((unsigned)m[1] << 8);
        /* CLOSE carries a handle only. Save identity BEFORE native teardown
         * erases the client, or leaves an idle server with its old address. */
        for (unsigned i = 0; i < 20; i++) {
            const uint8_t *e = mic_dg.table + i * 32;
            if (e[0] && mic_dg_handle(e) == notice.handle) {
                if (e[1]) {
                    memcpy(notice.bd, e + 3, 6);
                    notice.kind = MIC_DG_CLOSED; notify = 1;
                }
                break;
            }
        }
    }
    /* Audited OPEN layout: status:u16, BD[6], service:u8, pad,
     * handle:u16, UIPC:u8, pad. The failed branch reads no later fields. */
    if (event == 0 && m && (m[0] || m[1]) && mic_dg_handle(m) == 255) {
        unsigned missing = 0xffff;
        for (;;) {
            int found = 0;
            for (unsigned i = 0; i < 20; i++) {
                const uint8_t *e = mic_dg.table + i * 32;
                if (e[0] && mic_dg_handle(e) == missing) { found = 1; break; }
            }
            if (!found) break;
            missing--; /* at most 20 occupied handles; cannot exhaust u16 */
        }
        memcpy(failed_open, m, sizeof failed_open);
        failed_open[10] = (uint8_t)missing;
        failed_open[11] = (uint8_t)(missing >> 8);
        message = failed_open;
    }
    errno = saved;
    __atomic_load_n(&mic_dg.callback, __ATOMIC_ACQUIRE)(event, message);
    saved = errno;
    if (notify) {
        notice.state = mic_dg_state(notice.bd);
        /* The original handler can ignore an unknown success handle while
         * this BD already has a different open connection. Require the exact
         * post-callback handle/BD, not the aggregate peer state, before
         * publishing OPEN. No native table writes or extra closes here. */
        if (notice.kind != MIC_DG_OPENED || mic_dg_opened_handle(notice.bd, notice.handle))
            mic_dg_event_push(&notice);
    }
    pthread_mutex_unlock(&mic_dg.mutex);
    errno = saved;
}

static int mic_dg_install(void *base, const struct mic_bt_abi *abi)
{
    if (mic_dg.initialized) return mic_dg.failed ? -1 : 0;
    if (!base || !abi || !abi->dg_table) return -1;
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr)) return -1;
    int rc = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    if (!rc) rc = pthread_mutex_init(&mic_dg.mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    if (rc) return -1;
    mic_dg.table = (uint8_t *)base + abi->dg_table;
    mic_dg.initialized = 1; mic_dg.failed = 1;
    rc = mic_hook_install((uint8_t *)base + abi->offset[MIC_ABI_DG_CALLBACK], sig_DG_CALLBACK,
                         (void *)mic_dg_callback, (void **)&mic_dg.callback);
    if (rc < 0) return rc;
    int warning = rc;
    rc = mic_hook_install((uint8_t *)base + abi->offset[MIC_ABI_DG_LISTEN], sig_DG_LISTEN,
                         (void *)mic_dg_listen, (void **)&mic_dg.listen);
    if (rc < 0) return rc;
    warning |= rc;
    rc = mic_hook_install((uint8_t *)base + abi->offset[MIC_ABI_DG_OPEN], sig_DG_OPEN,
                         (void *)mic_dg_open, (void **)&mic_dg.open);
    if (rc < 0) return rc;
    mic_dg.failed = 0;
    return warning || rc;
}
#endif
