/* Bounded decoded history for stereo startup. Caller holds rec_mtx.
 * Placement uses the existing relative clock; this is not ADC calibration. */
#ifndef MIC_PREROLL_H
#define MIC_PREROLL_H
#include "mic_pcm_observation.h"
#include <stdint.h>
#include <string.h>
#define MIC_PREROLL_BLOCKS 20u
#define MIC_PREROLL_AGE_MS 100u
struct mic_preroll_frame {
    mic_pcm_origin origin;
    uint64_t stamp;
    int16_t pcm[240];
};
struct mic_preroll {
    struct mic_preroll_frame frames[MIC_PREROLL_BLOCKS];
    unsigned head, count;
};
static const struct mic_preroll_frame *mic_preroll_at(const struct mic_preroll *h, unsigned i)
{ return &h->frames[(h->head+i)%MIC_PREROLL_BLOCKS]; }
static int mic_preroll_follows(const struct mic_preroll_frame *a, const mic_pcm_origin *b, uint64_t stamp)
{
    uint64_t period=UINT64_C(268435456)*625/2;
    uint64_t step=(stamp+period-a->stamp)%period;
    return a->origin.generation==b->generation && a->origin.slot==b->slot &&
        !memcmp(a->origin.bd,b->bd,6) && a->origin.event_id<b->event_id &&
        a->origin.first_sample<=UINT64_MAX-240 && a->origin.first_sample+240==b->first_sample &&
        a->origin.receive_ns<=b->receive_ns && step>=4000 && step<=6000;
}
static void mic_preroll_store(struct mic_preroll *h, const int16_t *pcm,
                              const mic_pcm_origin *origin, uint64_t stamp, int eligible)
{
    if (!eligible || !origin || !origin->event_id || !origin->generation ||
            !origin->receive_ns || origin->samples!=240 || origin->synthetic || origin->merged) {
        h->head=h->count=0; return;
    }
    if (h->count && !mic_preroll_follows(mic_preroll_at(h,h->count-1),origin,stamp))
        h->head=h->count=0;
    if (h->count==MIC_PREROLL_BLOCKS) { h->head=(h->head+1)%MIC_PREROLL_BLOCKS; h->count--; }
    struct mic_preroll_frame *f=&h->frames[(h->head+h->count++)%MIC_PREROLL_BLOCKS];
    f->origin=*origin;f->stamp=stamp;memcpy(f->pcm,pcm,sizeof f->pcm);
}
#endif
