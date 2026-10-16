/* Action 5 style paired ring, adapted to decoded 48 kHz / 5 ms PCM.
 * Independent write cursors and a shared read cursor; fixed physical TX L/R.
 * Caller serializes operations and provides comparable, unwrapped BT us.
 * This is NOT a verified ADC/video clock mapping. No sleeps or allocations.
 */
#ifndef MIC_STEREO_H
#define MIC_STEREO_H
#include <stdint.h>
#include <string.h>
#include "mic_pcm_observation.h"
#include "mic_stereo_phase.h"
#define MIC_STEREO_SAMPLES 240
#define MIC_STEREO_CAP 80
#define MIC_STEREO_WAIT_MS 300
#define MIC_STEREO_SUSPEND_FRAMES 60
#define MIC_STEREO_RESUME_FRAMES 400
#define MIC_STEREO_MATCH_US 2500
#define MIC_STEREO_MAX_AHEAD 1000

struct mic_stereo_frame {
    uint64_t index, received_ms;
    int64_t stamp_us;
    int real, stamp_valid, concealed;
    mic_pcm_origin origin;
    int16_t pcm[MIC_STEREO_SAMPLES];
};
struct mic_stereo_pair {
    uint64_t index;
    unsigned present;
    struct mic_stereo_frame frame[2];
};
struct mic_stereo_channel {
    unsigned count;
    unsigned pending_head, pending_count;
    struct mic_stereo_frame pending[MIC_STEREO_CAP];
    uint64_t last_index, write_index;
    int have_index, have_stamp, suspended, sync_pending;
    int64_t last_stamp_us, resume_stamp_us;
    uint64_t resume_index;
    int resume_stamp_valid;
    unsigned resume_count, sync_misses;
    uint64_t real_samples, silent_samples, concealed_samples, late, duplicate, overflow, purged;
    uint64_t matches, relocations, sync_requests, sync_drops, sync_resets, suspends, resumes;
    mic_pcm_origin last_origin;
};
struct mic_stereo {
    struct mic_stereo_pair ring[MIC_STEREO_CAP];
    struct mic_stereo_channel channel[2];
    struct mic_stereo_phase phase;
    mic_pcm_origin output_origins[2];
    uint64_t next_index, output_frames;
    unsigned active_mask, wait_ms;
    int started, error;
};
typedef int (*mic_stereo_output)(void *, const int16_t *, unsigned, unsigned);
static void mic_stereo_init(struct mic_stereo *m, unsigned active_mask)
{
    memset(m, 0, sizeof *m); m->active_mask = active_mask & 3; m->wait_ms = MIC_STEREO_WAIT_MS;
    mic_stereo_phase_init(&m->phase);
}
static void mic_stereo_set_active(struct mic_stereo *m, unsigned active_mask)
{
    active_mask &= 3;
    for (unsigned slot = 0; slot < 2; slot++) if (!(active_mask & (1u << slot))) {
        m->channel[slot].suspended = 0; m->channel[slot].resume_count = 0;
        m->channel[slot].purged += m->channel[slot].pending_count;
        m->channel[slot].pending_head = m->channel[slot].pending_count = 0;
    }
    m->active_mask = active_mask;
}
static struct mic_stereo_frame *mic_stereo_at(struct mic_stereo *m, unsigned slot, uint64_t index)
{
    struct mic_stereo_pair *p = &m->ring[index % MIC_STEREO_CAP];
    return slot < 2 && p->index == index && (p->present & (1u << slot)) ? &p->frame[slot] : NULL;
}
/* Retire only the changed link, including old timestamp history. Never rewind
 * the common read cursor or flush healthy-peer frames. */
static void mic_stereo_reset_channel(struct mic_stereo *m, unsigned slot, int active)
{
    if (slot >= 2) return;
    mic_stereo_phase_reset(&m->phase, slot);
    struct mic_stereo_channel *c = &m->channel[slot];
    c->purged += c->pending_count;
    c->pending_head = c->pending_count = 0;
    c->purged += c->count; c->count = 0;
    c->have_index = c->have_stamp = c->suspended = 0;
    c->resume_count = c->sync_misses = 0; c->sync_pending = !!active;
    c->write_index = m->next_index;
    for (unsigned i = 0; i < MIC_STEREO_CAP; i++) {
        m->ring[i].present &= ~(1u << slot); m->ring[i].frame[slot].stamp_valid = 0;
    }
    if (active) m->active_mask |= 1u << slot; else m->active_mask &= ~(1u << slot);
}
static uint64_t mic_stereo_distance(int64_t a, int64_t b)
{
    return a >= b ? (uint64_t)a - (uint64_t)b : (uint64_t)b - (uint64_t)a;
}
/* Closest of 80 peer stamps, including recent consumed history. Matching an
 * already consumed index never permits replaying that audio. Choosing nearest
 * rather than first physical slot removes a ring-wrap dependent tie. */
static int mic_stereo_match(struct mic_stereo *m, unsigned slot, int64_t stamp, uint64_t *index)
{
    uint64_t best = MIC_STEREO_MATCH_US + 1; int found = 0;
    for (unsigned i = 0; i < MIC_STEREO_CAP; i++) {
        struct mic_stereo_frame *f = &m->ring[i].frame[slot ^ 1];
        if (!f->stamp_valid) continue;
        /* A read-side resync can only join a future unconsumed peer frame.
         * A5 clears consumed ring timestamps; keep our history for late-frame
         * diagnostics, but never match it during an established writer resync. */
        if (m->channel[slot].sync_pending && m->channel[slot].have_stamp &&
                m->started && f->index < m->next_index) continue;
        uint64_t delta = mic_stereo_distance(f->stamp_us, stamp);
        if (delta < best || (found && delta == best && f->index < *index)) {
            best = delta; *index = f->index; found = 1;
        }
    }
    return found;
}
/* A provisional returning frame must follow the healthy writer, not move it.
 * Move only that side of the pair; buffered peer PCM/provenance stays intact. */
static int mic_stereo_relocate(struct mic_stereo *m, unsigned slot, uint64_t from, uint64_t to)
{
    if (from == to) return 1;
    struct mic_stereo_frame *f = mic_stereo_at(m, slot, from);
    struct mic_stereo_pair *dest = &m->ring[to % MIC_STEREO_CAP];
    if (!f || (m->started && to < m->next_index) ||
        (dest->present && dest->index != to) || mic_stereo_at(m, slot, to)) return 0;
    struct mic_stereo_frame copy = *f;
    m->ring[from % MIC_STEREO_CAP].present &= ~(1u << slot); f->stamp_valid = 0;
    if (dest->index != to) {
        dest->frame[0].stamp_valid = dest->frame[1].stamp_valid = 0; dest->index = to;
    }
    copy.index = to; dest->frame[slot] = copy; dest->present |= 1u << slot;
    struct mic_stereo_channel *c = &m->channel[slot];
    if (c->last_index == from) { c->last_index = to; c->write_index = to + 1; }
    c->relocations++;
    return 1;
}
/* 0 accepted, 1 discarded, -1 invalid/discontinuous input. index is an initial
 * or fallback grid. After matching, advance from this writer's matched slot. */
/* Predict the same source-based cursor used by an established writer. Callers
 * still validate timestamp direction and alignment before using this cursor. */
static int mic_stereo_source_index(const struct mic_stereo_channel *c, unsigned slot,
    const mic_pcm_origin *origin, uint64_t *index)
{
    const mic_pcm_origin *prev = &c->last_origin;
    if (c->have_index && origin && origin->event_id && prev->event_id &&
            !origin->synthetic && !prev->synthetic && !origin->merged && !prev->merged &&
            origin->slot == slot && prev->slot == slot &&
            origin->generation == prev->generation &&
            !memcmp(origin->bd, prev->bd, sizeof origin->bd) &&
            origin->samples == MIC_STEREO_SAMPLES && prev->samples == MIC_STEREO_SAMPLES &&
            origin->first_sample > prev->first_sample) {
        uint64_t samples = origin->first_sample - prev->first_sample;
        if (samples <= MIC_STEREO_MAX_AHEAD * MIC_STEREO_SAMPLES &&
                samples % MIC_STEREO_SAMPLES == 0) {
            *index = c->last_index + samples / MIC_STEREO_SAMPLES;
            return 1;
        }
    }
    return 0;
}

static int mic_stereo_push_timestamp_now(struct mic_stereo *m, unsigned slot, uint64_t index,
    uint64_t received_ms, int stamp_valid, int64_t stamp_us,
    const int16_t *pcm, int real, const mic_pcm_origin *origin)
{
    if (m->error || slot >= 2 || index == UINT64_MAX || (real && !pcm)) return -1;
    struct mic_stereo_channel *c = &m->channel[slot], *other = &m->channel[slot ^ 1];
    if (c->suspended) {
        /* A5 compares the old counter with 400: resume on frame 401. A
         * sequence/clock gap restarts qualification; no other TX is reset. */
        uint64_t step = mic_stereo_distance(stamp_us, c->resume_stamp_us);
        int continuous = stamp_valid ?
            (stamp_us > c->resume_stamp_us && step >= 4000 && step <= 6000) :
            (index > c->resume_index && index - c->resume_index == 1);
        /* Missing BT stamps use the caller's receiver grid, like normal
         * ingress. Do not strand that side forever or treat it as calibrated.
         * A clock-source transition starts a new qualification window. */
        if (c->resume_count && (c->resume_stamp_valid != !!stamp_valid || !continuous))
            c->resume_count = 0;
        c->resume_stamp_us = stamp_us;
        c->resume_index = index; c->resume_stamp_valid = !!stamp_valid;
        if (c->resume_count++ < MIC_STEREO_RESUME_FRAMES) { c->sync_drops++; return 1; }
        c->suspended = 0; c->resumes++; c->sync_pending = 1; c->have_index = c->have_stamp = 0;
    }
    if (stamp_valid && c->have_stamp) {
        if (stamp_us <= c->last_stamp_us) { c->duplicate++; return 1; }
        uint64_t elapsed = mic_stereo_distance(stamp_us, c->last_stamp_us);
        if (elapsed <= MIC_STEREO_MAX_AHEAD * UINT64_C(5000)) index = c->last_index + (elapsed + 2500) / 5000;
        else c->sync_pending = 1;
        /* A5 advances each writer by source frames, not clock distance.
         * A continuous sequence can carry a 10 ms BT clock step without a
         * missing 5 ms audio block. Use decoded sample provenance, retaining
         * actual source gaps
         * (the parser advances first_sample through concealment). Raw stamps
         * remain unchanged for later paired matching. */
        mic_stereo_source_index(c, slot, origin, &index);
    }
    if (stamp_valid && (m->active_mask & (1u << (slot ^ 1))) && !other->suspended &&
            (c->sync_pending || !c->have_stamp || (other->sync_pending && other->count == 1))) {
        uint64_t matched = 0;
        if (mic_stereo_match(m, slot, stamp_us, &matched)) {
            c->matches++;
            if (other->sync_pending && !c->sync_pending && c->have_stamp) {
                if (mic_stereo_relocate(m, slot ^ 1, matched, index) && other->count == 1)
                    other->sync_pending = 0;
            } else {
                if (m->started && matched < m->next_index) { c->late++; return 1; }
                if (m->started && matched - m->next_index > MIC_STEREO_MAX_AHEAD) return -1;
                if (c->have_index && matched <= c->last_index &&
                        (c->sync_pending || !c->have_stamp)) {
                    /* Native A5 rewinds this writer onto its matching peer
                     * slot, replacing queued old data. Retire only this TX's
                     * superseded suffix, so it cannot replay stale frames if
                     * the producer stalls before overwriting the next slot.
                     * Preserve the peer and the shared read cursor. */
                    for (unsigned i = 0; i < MIC_STEREO_CAP; i++) {
                        struct mic_stereo_pair *p = &m->ring[i];
                        if (p->index < matched) continue;
                        if (p->present & (1u << slot)) {
                            p->present &= ~(1u << slot); c->count--;
                            c->purged++; c->sync_drops++;
                        }
                        p->frame[slot].stamp_valid = 0;
                    }
                    c->have_index = 0;
                }
                if (matched != index) c->relocations++;
                index = matched;
            }
            c->sync_pending = 0; c->sync_misses = 0;
        } else if (c->sync_pending && other->have_stamp &&
                (c->have_stamp || stamp_us < other->last_stamp_us ||
                 mic_stereo_distance(stamp_us, other->last_stamp_us) > MIC_STEREO_CAP * UINT64_C(5000))) {
            /* Established resync waits for an unconsumed matching peer. A
             * returning writer without valid stamp history may arrive first:
             * retain its bounded future candidate for the peer to relocate. */
            c->sync_drops++;
            if (++c->sync_misses >= MIC_STEREO_CAP) {
                mic_stereo_reset_channel(m, slot, 1); c->sync_resets++;
            }
            return 1;
        }
    }
    if (m->started && index < m->next_index) { c->late++; return 1; }
    if (m->started && index - m->next_index > MIC_STEREO_MAX_AHEAD) return -1;
    if (c->have_index && index <= c->last_index) { c->duplicate++; return 1; }
    struct mic_stereo_pair *pair = &m->ring[index % MIC_STEREO_CAP];
    if ((pair->present && pair->index != index) || c->count == MIC_STEREO_CAP) {
        c->overflow++; c->sync_pending = 1; return 1;
    }
    if (!m->started) for (unsigned i = 0; i < MIC_STEREO_CAP; i++) if (m->ring[i].present) {
        uint64_t n = m->ring[i].index;
        if ((index > n ? index - n : n - index) > MIC_STEREO_MAX_AHEAD) return -1;
    }
    if (pair->index != index) {
        pair->frame[0].stamp_valid = pair->frame[1].stamp_valid = 0; pair->index = index;
    }
    struct mic_stereo_frame *f = &pair->frame[slot];
    f->index = index; f->received_ms = received_ms; f->real = !!real; f->concealed = 0;
    f->stamp_us = stamp_us; f->stamp_valid = !!stamp_valid;
    f->origin = origin ? *origin : (mic_pcm_origin){0};
    if (real) memcpy(f->pcm, pcm, sizeof f->pcm); else memset(f->pcm, 0, sizeof f->pcm);
    pair->present |= 1u << slot;
    c->count++; c->last_index = index; c->write_index = index + 1; c->have_index = 1;
    c->last_origin = origin ? *origin : (mic_pcm_origin){0};
    c->have_stamp = !!stamp_valid; c->last_stamp_us = stamp_us;
    return 0;
}

/* PLC has source sample provenance but no captured clock. Fill only an
 * established writer's unconsumed source position; never use the following
 * packet's stamp, establish/resume alignment, or advance clock history. */
static int mic_stereo_push_concealed(struct mic_stereo *m, unsigned slot,
    uint64_t received_ms, const int16_t *pcm, const mic_pcm_origin *origin)
{
    if (m->error || slot >= 2 || !pcm || !origin) return -1;
    struct mic_stereo_channel *c = &m->channel[slot];
    const mic_pcm_origin *prev = &c->last_origin;
    if (!(m->active_mask & (1u << slot)) || !c->have_index || !c->have_stamp ||
            c->sync_pending || c->suspended || c->pending_count ||
            !origin->synthetic || origin->merged || prev->synthetic || prev->merged ||
            !prev->event_id || origin->event_id <= prev->event_id ||
            origin->source != 4 || prev->source != 4 ||
            origin->slot != slot || prev->slot != slot ||
            origin->generation != prev->generation || memcmp(origin->bd, prev->bd, 6) ||
            origin->samples != MIC_STEREO_SAMPLES || prev->samples != MIC_STEREO_SAMPLES ||
            origin->first_sample <= prev->first_sample) return 1;
    uint64_t distance = origin->first_sample - prev->first_sample;
    if (distance % MIC_STEREO_SAMPLES || distance > 40u * MIC_STEREO_SAMPLES) return 1;
    uint64_t step = distance / MIC_STEREO_SAMPLES;
    if (c->last_index > UINT64_MAX - step) return 1;
    uint64_t index = c->last_index + step;
    if ((m->started && (index < m->next_index || index - m->next_index >= MIC_STEREO_CAP)) ||
            c->count == MIC_STEREO_CAP) return 1;
    struct mic_stereo_pair *pair = &m->ring[index % MIC_STEREO_CAP];
    if ((pair->present && pair->index != index) || mic_stereo_at(m, slot, index)) return 1;
    if (pair->index != index) {
        pair->frame[0].stamp_valid = pair->frame[1].stamp_valid = 0; pair->index = index;
    }
    struct mic_stereo_frame *f = &pair->frame[slot];
    *f = (struct mic_stereo_frame){.index=index, .received_ms=received_ms,
        .real=1, .concealed=1, .origin=*origin};
    memcpy(f->pcm, pcm, sizeof f->pcm);
    pair->present |= 1u << slot; c->count++;
    return 0;
}

/* A resync input can arrive before the peer batch which will match it. Keep
 * those future candidates outside the committed ring: its read cursor must
 * not consume provisional PCM, and the healthy writer must not move for it.
 * Flush on either producer callback before the caller drains real pairs. */
static void mic_stereo_pending_flush(struct mic_stereo *m, unsigned slot, uint64_t now_ms, int force)
{
    struct mic_stereo_channel *c = &m->channel[slot], *other = &m->channel[slot ^ 1];
    while (c->pending_count) {
        struct mic_stereo_frame f = c->pending[c->pending_head];
        int expired = force || (now_ms >= f.received_ms && now_ms - f.received_ms >= m->wait_ms);
        uint64_t matched = 0;
        int match = other->have_stamp && mic_stereo_match(m, slot, f.stamp_us, &matched);
        /* Only an unresolved writer needs a peer anchor. Once the first
         * staged frame has established its cursor, commit its suffix using
         * normal source cadence. Requiring another match for each frame can
         * deadlock two pending queues if a clock step makes the peer resync.
         * push_timestamp_now still enforces ordering and ring capacity. */
        int established = c->have_stamp && !c->sync_pending;
        if (!established && !match && !expired &&
                (!other->have_stamp || f.stamp_us > other->last_stamp_us)) break;
        c->pending_head = (c->pending_head + 1) % MIC_STEREO_CAP;
        c->pending_count--;
        if (!established && !match) { c->sync_drops++; continue; }
        mic_stereo_push_timestamp_now(m, slot, f.index, f.received_ms, f.stamp_valid,
            f.stamp_us, f.pcm, f.real, &f.origin);
    }
}

static int mic_stereo_push_timestamp(struct mic_stereo *m, unsigned slot, uint64_t index,
    uint64_t received_ms, int stamp_valid, int64_t stamp_us,
    const int16_t *pcm, int real, const mic_pcm_origin *origin)
{
    if (m->error || slot >= 2 || index == UINT64_MAX || (real && !pcm)) return -1;
    struct mic_stereo_channel *c = &m->channel[slot], *other = &m->channel[slot ^ 1];
    uint64_t matched = 0;
    int future = stamp_valid && c->sync_pending && other->have_stamp &&
        (m->active_mask & (1u << (slot ^ 1))) && !other->suspended && !other->sync_pending &&
        stamp_us > other->last_stamp_us &&
        mic_stereo_distance(stamp_us, other->last_stamp_us) > MIC_STEREO_MATCH_US &&
        !mic_stereo_match(m, slot, stamp_us, &matched);
    int rc;
    if (c->pending_count || future) {
        if (!stamp_valid || mic_stereo_distance(stamp_us, other->last_stamp_us) >
                MIC_STEREO_CAP * UINT64_C(5000)) { c->sync_drops++; return 1; }
        if (c->pending_count && stamp_us <= c->pending[(c->pending_head+c->pending_count-1)%MIC_STEREO_CAP].stamp_us) {
            c->duplicate++; return 1;
        }
        if (c->pending_count == MIC_STEREO_CAP) { c->overflow++; return 1; }
        struct mic_stereo_frame *f = &c->pending[(c->pending_head+c->pending_count)%MIC_STEREO_CAP];
        *f = (struct mic_stereo_frame){.index=index, .received_ms=received_ms,
            .stamp_us=stamp_us, .real=!!real, .stamp_valid=1,
            .origin=origin ? *origin : (mic_pcm_origin){0}};
        if (real) memcpy(f->pcm, pcm, sizeof f->pcm);
        c->pending_count++; rc = 0;
    } else rc = mic_stereo_push_timestamp_now(m, slot, index, received_ms, stamp_valid,
                                             stamp_us, pcm, real, origin);
    mic_stereo_pending_flush(m, slot, received_ms, 0);
    mic_stereo_pending_flush(m, slot ^ 1, received_ms, 0);
    return rc;
}
static int mic_stereo_push_observed(struct mic_stereo *m, unsigned slot, uint64_t index,
    uint64_t received_ms, const int16_t *pcm, int real, const mic_pcm_origin *origin)
{
    return mic_stereo_push_timestamp(m, slot, index, received_ms, 0, 0, pcm, real, origin);
}
static int mic_stereo_push(struct mic_stereo *m, unsigned slot, uint64_t index,
    uint64_t received_ms, const int16_t *pcm, int real)
{
    return mic_stereo_push_observed(m, slot, index, received_ms, pcm, real, NULL);
}
/* Shared consumption always emits a pair; missing sides are zero. force
 * bypasses waiting, but never bypasses timestamp safety or sink errors. */
static int mic_stereo_drain_limit(struct mic_stereo *m, uint64_t now_ms, int force,
    uint64_t limit, unsigned tail_samples, mic_stereo_output output, void *opaque)
{
    if (m->error || !output) return -1;
    mic_stereo_pending_flush(m, 0, now_ms, force);
    mic_stereo_pending_flush(m, 1, now_ms, force);
    unsigned emitted = 0;
    while (emitted < MIC_STEREO_CAP) {
        struct mic_stereo_pair *first = NULL;
        for (unsigned i = 0; i < MIC_STEREO_CAP; i++)
            if (m->ring[i].present && (!first || m->ring[i].index < first->index)) first = &m->ring[i];
        if (!first) break;
        uint64_t index = m->started ? m->next_index : first->index;
        if (index >= limit) break;
        struct mic_stereo_frame *f[2] = {mic_stereo_at(m, 0, index), mic_stereo_at(m, 1, index)};
        unsigned samples = tail_samples && index == limit-1 ? tail_samples:MIC_STEREO_SAMPLES;
        /* Leave a completely missing partial tail to the file's padding path.
         * A future queued pair must not be substituted for this position. */
        if (samples < MIC_STEREO_SAMPLES && !f[0] && !f[1]) break;
        if (f[0] && f[1] && f[0]->stamp_valid && f[1]->stamp_valid &&
                mic_stereo_distance(f[0]->stamp_us, f[1]->stamp_us) > MIC_STEREO_MATCH_US + 500) {
            unsigned lag = f[0]->stamp_us < f[1]->stamp_us ? 0 : 1;
            struct mic_stereo_channel *c = &m->channel[lag];
            mic_stereo_phase_reset(&m->phase, lag);
            c->sync_pending = 1; c->sync_requests++;
            /* Native A5 (0x44154..0x441c8) requests future alignment but
             * admits this real pair to the decoder. Do not manufacture a
             * silent block merely because its clock jumped. `real` controls
             * audio policy (mute or unconfirmed status), not stamp validity:
             * a muted returning TX must not silence its healthy peer. Keep
             * rejection for gross discontinuities outside the 200 ms gate. */
            if (mic_stereo_distance(f[0]->stamp_us, f[1]->stamp_us) >=
                    MIC_STEREO_MATCH_US * MIC_STEREO_CAP) {
                c->sync_drops++; c->count--;
                m->ring[index % MIC_STEREO_CAP].present &= ~(1u << lag); f[lag] = NULL;
            }
        }
        for (unsigned missing = 0; missing < 2; missing++) {
            unsigned live = missing ^ 1;
            struct mic_stereo_channel *c = &m->channel[missing];
            if (f[missing] || !(m->active_mask & (1u << missing)) || c->suspended) continue;
            if (c->have_index && c->write_index > index) continue; /* proven hole */
            if (!force && m->channel[live].count >= MIC_STEREO_SUSPEND_FRAMES && !c->count) {
                mic_stereo_phase_reset(&m->phase, missing);
                c->suspended = 1; c->suspends++; c->resume_count = 0; continue;
            }
            struct mic_stereo_frame *waiting = f[live];
            if (!waiting) waiting = &first->frame[(first->present & 1) ? 0 : 1];
            if (!force && (now_ms < waiting->received_ms || now_ms - waiting->received_ms < m->wait_ms))
                return (int)emitted;
        }
        m->started = 1; m->next_index = index;
        unsigned real_mask = 0; int16_t pcm[MIC_STEREO_SAMPLES * 2] = {0};
        memset(m->output_origins, 0, sizeof m->output_origins);
        for (unsigned i = 0; i < 2; i++) if (f[i]) m->output_origins[i] = f[i]->origin;
        for (unsigned i = 0; i < 2; i++) if (f[i] && f[i]->real) {
            real_mask |= 1u << i;
            for (unsigned s = 0; s < MIC_STEREO_SAMPLES; s++) pcm[2*s + i] = f[i]->pcm[s];
        }
        int valid_pair = real_mask == 3 && f[0]->stamp_valid && f[1]->stamp_valid &&
            !m->channel[0].sync_pending && !m->channel[1].sync_pending;
        mic_stereo_phase_observe(&m->phase, valid_pair,
            f[0] ? f[0]->stamp_us : 0, f[1] ? f[1]->stamp_us : 0);
        uint64_t phase_padding[2] = {m->phase.padded_samples[0], m->phase.padded_samples[1]};
        mic_stereo_phase_apply(&m->phase, pcm, real_mask, m->output_origins);
        for (unsigned i = 0; i < 2; i++) {
            if (samples < MIC_STEREO_SAMPLES && m->output_origins[i].event_id)
                m->output_origins[i].samples = samples;
            if (m->phase.padded_samples[i] - phase_padding[i] > samples)
                m->phase.padded_samples[i] = phase_padding[i] + samples;
        }
        if (output(opaque, pcm, samples, real_mask)) { m->error = 1; return -1; }
        for (unsigned i = 0; i < 2; i++) {
            struct mic_stereo_channel *c = &m->channel[i];
            if (f[i]) { m->ring[index % MIC_STEREO_CAP].present &= ~(1u << i); c->count--; }
            if (real_mask & (1u << i)) {
                c->real_samples += samples;
                if (f[i]->concealed) c->concealed_samples += samples;
            }
            else c->silent_samples += samples;
        }
        m->next_index++; m->output_frames++; emitted++;
    }
    return (int)emitted;
}
static int mic_stereo_drain_before(struct mic_stereo *m, uint64_t now_ms, int force,
    uint64_t limit, mic_stereo_output output, void *opaque)
{
    return mic_stereo_drain_limit(m, now_ms, force, limit, 0, output, opaque);
}
/* Terminal file drain: consume the last pair but emit only its prefix before
 * the sample endpoint. The assembler must not resume after this partial pair.
 * Live FIFO consumers keep the whole-block API above. */
static int mic_stereo_drain_samples(struct mic_stereo *m, uint64_t now_ms,
    uint64_t samples, mic_stereo_output output, void *opaque)
{
    unsigned tail = (unsigned)(samples % MIC_STEREO_SAMPLES);
    return mic_stereo_drain_limit(m, now_ms, 1, samples/MIC_STEREO_SAMPLES + !!tail,
        tail, output, opaque);
}
static int mic_stereo_drain(struct mic_stereo *m, uint64_t now_ms, int force,
    mic_stereo_output output, void *opaque)
{
    return mic_stereo_drain_before(m, now_ms, force, UINT64_MAX, output, opaque);
}
static void mic_stereo_advance(struct mic_stereo *m, uint64_t index)
{
    if (m->started && index <= m->next_index) return;
    /* Padding bypasses the PCM callback. It is still an audio discontinuity;
     * never splice an old frame tail across a wall-clock silence interval. */
    m->phase.history_mask = 0;
    m->started = 1; m->next_index = index;
    for (unsigned i = 0; i < MIC_STEREO_CAP; i++) {
        struct mic_stereo_pair *p = &m->ring[i];
        if (p->index >= index) continue;
        for (unsigned slot = 0; slot < 2; slot++) if (p->present & (1u << slot)) {
            m->channel[slot].count--; m->channel[slot].late++;
        }
        p->present = 0;
    }
}
#endif
