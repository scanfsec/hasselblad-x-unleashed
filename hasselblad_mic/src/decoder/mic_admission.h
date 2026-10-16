/* Value-only admission snapshot. Native callbacks never take engine locks.
 * This supplements the stock address list; it is not Bluetooth authentication. */
#ifndef MIC_ADMISSION_H
#define MIC_ADMISSION_H
#include <pthread.h>
#include <stdint.h>
#include <string.h>
enum { MIC_ADMISSION_NONE, MIC_ADMISSION_ALLOW, MIC_ADMISSION_HELD };
static struct {
    pthread_mutex_t mutex;
    int enabled;
    struct { uint8_t bd[6]; int policy; } tx[2];
} mic_admission = { .mutex = PTHREAD_MUTEX_INITIALIZER };

static void mic_admission_enable(int enabled)
{
    pthread_mutex_lock(&mic_admission.mutex);
    mic_admission.enabled = enabled;
    if (!enabled) memset(mic_admission.tx, 0, sizeof mic_admission.tx);
    pthread_mutex_unlock(&mic_admission.mutex);
}

static void mic_admission_set(unsigned slot, const uint8_t *bd, int policy)
{
    if (slot >= 2) return;
    pthread_mutex_lock(&mic_admission.mutex);
    memset(&mic_admission.tx[slot], 0, sizeof mic_admission.tx[slot]);
    if (bd && (policy == MIC_ADMISSION_ALLOW || policy == MIC_ADMISSION_HELD)) {
        memcpy(mic_admission.tx[slot].bd, bd, 6);
        mic_admission.tx[slot].policy = policy;
    }
    pthread_mutex_unlock(&mic_admission.mutex);
}

static int mic_admission_policy(const uint8_t *bd)
{
    int policy = MIC_ADMISSION_NONE;
    if (!bd) return 0;
    pthread_mutex_lock(&mic_admission.mutex);
    if (mic_admission.enabled) for (unsigned i = 0; i < 2; i++)
        if (mic_admission.tx[i].policy && !memcmp(mic_admission.tx[i].bd, bd, 6)) policy = mic_admission.tx[i].policy;
    pthread_mutex_unlock(&mic_admission.mutex);
    return policy;
}

static int mic_admission_allows(const uint8_t *bd)
{
    return mic_admission_policy(bd) == MIC_ADMISSION_ALLOW;
}
#endif
