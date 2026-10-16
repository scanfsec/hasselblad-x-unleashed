/* Bounded, non-consuming per-sink input journal. Caller holds AAC mutex.
 * No allocation, I/O, clock fitting, or codec calls on the producer path. */
#ifndef MIC_AAC_TIMELINE_H
#define MIC_AAC_TIMELINE_H
#include "mic_pcm_observation.h"
#define MIC_AAC_TIMELINE_CAP 2048
#define MIC_AAC_TIMELINE_PAGE 3
enum { MIC_INPUT_QUEUED = 1, MIC_INPUT_OVERFLOW, MIC_INPUT_PADDING };
typedef struct {
    uint64_t id;
    unsigned kind;
    mic_pcm_submission block;
} mic_input_event;
typedef struct {
    uint64_t latest;
    mic_input_event events[MIC_AAC_TIMELINE_CAP];
} mic_input_journal;

static void mic_input_append(mic_input_journal *j, unsigned kind, const mic_pcm_submission *block)
{
    uint64_t id = ++j->latest;
    j->events[(id - 1) % MIC_AAC_TIMELINE_CAP] = (mic_input_event){id, kind, *block};
}

/* A slow reader gets an explicit gap, never a silently complete journal. */
static int mic_input_page(const mic_input_journal *j, uint64_t after,
                          mic_input_event out[MIC_AAC_TIMELINE_PAGE], uint64_t *lost)
{
    if (after > j->latest) return -1;
    uint64_t oldest = j->latest > MIC_AAC_TIMELINE_CAP ? j->latest - MIC_AAC_TIMELINE_CAP : 0;
    *lost = after < oldest ? oldest - after : 0;
    after += *lost;
    unsigned count = 0;
    while (after < j->latest && count < MIC_AAC_TIMELINE_PAGE) {
        out[count++] = j->events[after % MIC_AAC_TIMELINE_CAP];
        after++;
    }
    return (int)count;
}
#endif
