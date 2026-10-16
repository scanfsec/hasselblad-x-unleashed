/* Provenance of decoded PCM, not an ADC clock mapping. Frames are per channel. */
#ifndef MIC_PCM_OBSERVATION_H
#define MIC_PCM_OBSERVATION_H
#include <stdint.h>
#include <time.h>

typedef struct {
    uint64_t event_id, generation, first_sample;
    uint64_t receive_ns, receive_end_ns, observed_ns;
    unsigned samples, slot;
    int source, synthetic, merged, muted;
    uint8_t bd[6];
} mic_pcm_origin;

typedef struct {
    uint64_t first_sample, frames, enqueue_ns;
    unsigned real_mask;
    mic_pcm_origin origins[2]; /* physical TX slots, before channel routing */
} mic_pcm_submission;

static uint64_t mic_pcm_observed_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}
#endif
