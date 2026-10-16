/* Native callbacks publish bounded value copies only. No engine/native calls
 * are made while holding this mailbox lock. Its condition also wakes close
 * waiters, independently of the control lock held by disconnect. */
#ifndef MIC_DG_EVENTS_H
#define MIC_DG_EVENTS_H
#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <string.h>

enum { MIC_DG_OPENED, MIC_DG_CLOSED, MIC_DG_FAILED };
struct mic_dg_event {
    uint8_t bd[6];
    unsigned kind, handle, state;
    uint64_t sequence;
};
static struct {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    struct mic_dg_event events[64];
    unsigned first, count, overflow;
    uint64_t sequence;
    int enabled;
} mic_dg_events = { .mutex = PTHREAD_MUTEX_INITIALIZER,
                    .changed = PTHREAD_COND_INITIALIZER };

static void mic_dg_events_enable(int enabled)
{
    pthread_mutex_lock(&mic_dg_events.mutex);
    mic_dg_events.first = mic_dg_events.count = mic_dg_events.overflow = 0;
    __atomic_store_n(&mic_dg_events.enabled, enabled, __ATOMIC_RELEASE);
    mic_dg_events.sequence++;
    pthread_cond_broadcast(&mic_dg_events.changed);
    pthread_mutex_unlock(&mic_dg_events.mutex);
}
static int mic_dg_events_enabled(void)
{
    return __atomic_load_n(&mic_dg_events.enabled, __ATOMIC_ACQUIRE);
}
static void mic_dg_event_push(const struct mic_dg_event *event)
{
    pthread_mutex_lock(&mic_dg_events.mutex);
    if (mic_dg_events.enabled) {
        if (mic_dg_events.count == 64) {
            mic_dg_events.first = (mic_dg_events.first + 1) % 64;
            mic_dg_events.count--;
            mic_dg_events.overflow++;
        }
        unsigned index = (mic_dg_events.first + mic_dg_events.count++) % 64;
        mic_dg_events.events[index] = *event;
        mic_dg_events.events[index].sequence = ++mic_dg_events.sequence;
        pthread_cond_broadcast(&mic_dg_events.changed);
    }
    pthread_mutex_unlock(&mic_dg_events.mutex);
}
static int mic_dg_event_pop(struct mic_dg_event *event, unsigned *overflow)
{
    pthread_mutex_lock(&mic_dg_events.mutex);
    *overflow = mic_dg_events.overflow; mic_dg_events.overflow = 0;
    int found = mic_dg_events.count != 0;
    if (found) {
        *event = mic_dg_events.events[mic_dg_events.first];
        mic_dg_events.first = (mic_dg_events.first + 1) % 64;
        mic_dg_events.count--;
    }
    pthread_mutex_unlock(&mic_dg_events.mutex);
    return found;
}
static uint64_t mic_dg_event_cursor(void)
{
    pthread_mutex_lock(&mic_dg_events.mutex);
    uint64_t sequence = mic_dg_events.sequence;
    pthread_mutex_unlock(&mic_dg_events.mutex);
    return sequence;
}
static void mic_dg_event_wait(uint64_t cursor, unsigned ms)
{
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += ms / 1000;
    until.tv_nsec += (long)(ms % 1000) * 1000000;
    if (until.tv_nsec >= 1000000000) { until.tv_sec++; until.tv_nsec -= 1000000000; }
    pthread_mutex_lock(&mic_dg_events.mutex);
    while (mic_dg_events.enabled && mic_dg_events.sequence == cursor) {
        if (pthread_cond_timedwait(&mic_dg_events.changed, &mic_dg_events.mutex, &until)) break;
    }
    pthread_mutex_unlock(&mic_dg_events.mutex);
}
#endif
