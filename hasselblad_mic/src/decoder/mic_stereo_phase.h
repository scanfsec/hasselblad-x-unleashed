/* Frame-internal PCM phase alignment: A5 process_threadloop 0x4436c..0x44664
 * averages 79 valid paired timestamp differences and converts us to samples.
 * Here the later-stamped input is delayed using ITS OWN history, not a prefix
 * copied from the opposite TX. No resampling, gain or channel substitution.
 * Timestamp comparability is a caller precondition; this is not AV calibration.
 */
#ifndef MIC_STEREO_PHASE_H
#define MIC_STEREO_PHASE_H
#include <stdint.h>
#include <string.h>
#include "mic_pcm_observation.h"
#define MIC_PHASE_SAMPLES 240
#define MIC_PHASE_WINDOW 79
#define MIC_PHASE_MAX_US 2500
struct mic_stereo_phase {
    unsigned observations, later_slot, calibrated, enabled;
    uint64_t sum_us, corrections, padded_samples[2];
    unsigned delay_samples[2], history_mask, used_history_mask;
    int16_t history[2][MIC_PHASE_SAMPLES];
    mic_pcm_origin origins[2];
};
static void mic_stereo_phase_init(struct mic_stereo_phase *p)
{
    memset(p, 0, sizeof *p); p->enabled = 1;
}
static void mic_stereo_phase_reset(struct mic_stereo_phase *p, unsigned slot)
{
    if (slot >= 2) return;
    p->observations = p->calibrated = 0; p->sum_us = 0;
    p->delay_samples[slot] = 0; p->history_mask &= ~(1u << slot);
    memset(&p->origins[slot], 0, sizeof p->origins[slot]);
}
static void mic_stereo_phase_observe(struct mic_stereo_phase *p, int valid, int64_t left, int64_t right)
{
    if (!p->enabled || p->calibrated) return;
    unsigned later = right >= left;
    uint64_t delta = later ? (uint64_t)right - (uint64_t)left : (uint64_t)left - (uint64_t)right;
    if (!valid || delta > MIC_PHASE_MAX_US || (p->observations && later != p->later_slot)) {
        p->observations = 0; p->sum_us = 0;
        if (!valid || delta > MIC_PHASE_MAX_US) return;
    }
    p->later_slot = later; p->sum_us += delta;
    if (++p->observations == MIC_PHASE_WINDOW) {
        unsigned delay = (unsigned)((p->sum_us / p->observations) * 48 / 1000);
        p->delay_samples[later] = delay; p->delay_samples[later ^ 1] = 0;
        p->calibrated = 1; p->corrections++;
    }
}
static void mic_stereo_phase_apply(struct mic_stereo_phase *p, int16_t *pcm, unsigned real_mask,
                                  mic_pcm_origin origins[2])
{
    p->used_history_mask = 0;
    for (unsigned slot = 0; slot < 2; slot++) {
        if (!(real_mask & (1u << slot))) {
            /* Muting/disconnection must not leak the previous frame's tail. */
            p->history_mask &= ~(1u << slot); memset(&p->origins[slot], 0, sizeof p->origins[slot]);
            continue;
        }
        int16_t current[MIC_PHASE_SAMPLES];
        for (unsigned s = 0; s < MIC_PHASE_SAMPLES; s++) current[s] = pcm[2*s+slot];
        unsigned delay = p->enabled ? p->delay_samples[slot] : 0;
        mic_pcm_origin original = origins[slot];
        if (delay) {
            int history = !!(p->history_mask & (1u << slot));
            if (history) p->used_history_mask |= 1u << slot;
            else p->padded_samples[slot] += delay;
            for (unsigned s = 0; s < MIC_PHASE_SAMPLES; s++)
                pcm[2*s+slot] = s >= delay ? current[s-delay] :
                    history ? p->history[slot][MIC_PHASE_SAMPLES-delay+s] : 0;
            origins[slot].merged = 1;
            if (history && p->origins[slot].generation == original.generation &&
                    p->origins[slot].first_sample + MIC_PHASE_SAMPLES == original.first_sample) {
                origins[slot].synthetic |= p->origins[slot].synthetic;
                origins[slot].first_sample = original.first_sample - delay;
                origins[slot].receive_ns = p->origins[slot].receive_ns;
            } else origins[slot].synthetic = 1;
        }
        memcpy(p->history[slot], current, sizeof current); p->origins[slot] = original;
        p->history_mask |= 1u << slot;
    }
}
#endif
