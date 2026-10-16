/* Relative audio placement, NOT a calibrated ADC-to-camera clock mapping.
 * Action5's integer expression is floor(btclk * 625 / 2) + intra (us).
 * The 28-bit wrap model and cross-peer clock relationship remain unverified
 * on X2D II. Discontinuities fall back to receiver time and are counted;
 * callers must continue reporting mapping_verified=false.
 */
#ifndef MIC_STEREO_CLOCK_H
#define MIC_STEREO_CLOCK_H
#include <stdint.h>
#include <string.h>
#define MIC_BT_PERIOD_US (UINT64_C(268435456) * 625 / 2)
/* Fit within the paired ring's 300 ms waiting budget (80 x 5 ms slots).
 * A provisional reconnect clock must not reserve frames seconds ahead of
 * the healthy peer. Otherwise the non-advancing-index correction below can
 * retain that future offset even after the raw clock becomes normal again. */
#define MIC_STEREO_CLOCK_SKEW_US INT64_C(300000)
struct mic_stereo_clock_peer {
    uint64_t generation, last_index, fallback_frames, rebases, catchup_drops;
    int64_t offset_us, stamp_us, raw_delta_us;
    unsigned recovery_frames;
    int seen, rebased, raw_valid, shared_catchup;
};
struct mic_stereo_clock {
    uint64_t start_ms, origin_ms, origin_index, origin_us;
    int anchored;
    struct mic_stereo_clock_peer peer[2];
};
static void mic_stereo_clock_init(struct mic_stereo_clock *c, uint64_t start_ms)
{
    memset(c, 0, sizeof *c); c->start_ms = start_ms;
}
/* 0 placed using the relative BT model; 1 receiver fallback; -1 too old/invalid.
 * received_ms is the transport callback boundary, never the current poll time. */
static int mic_stereo_clock_index(struct mic_stereo_clock *c, unsigned slot,
    uint64_t generation, uint64_t received_ms, int valid, uint32_t raw,
    uint16_t intra, uint64_t *index)
{
    if (slot >= 2 || !index || received_ms < c->start_ms) return -1;
    struct mic_stereo_clock_peer *p = &c->peer[slot];
    uint64_t arrival = (received_ms - c->start_ms) / 5;
    int changed = p->seen && p->generation != generation;
    if (changed) { p->offset_us = 0; p->rebased = 0; p->recovery_frames = 0; p->shared_catchup = 0; }
    int fallback = !valid || raw >= (1u << 28) || intra >= 1250;
    if (!fallback) {
        uint64_t stamp = ((uint64_t)raw * 625 / 2 + intra) % MIC_BT_PERIOD_US;
        if (!c->anchored) {
            c->anchored = 1; c->origin_us = stamp;
            c->origin_ms = received_ms; c->origin_index = arrival;
        }
        int64_t delta = (int64_t)stamp - (int64_t)c->origin_us;
        int64_t expected = ((int64_t)received_ms - (int64_t)c->origin_ms) * 1000;
        int64_t period = (int64_t)MIC_BT_PERIOD_US;
        int64_t wraps = expected - delta;
        wraps = wraps >= 0 ? (wraps + period / 2) / period : -((-wraps + period / 2) / period);
        delta += wraps * period;
        /* An established 5 ms sampling clock can arrive in a late burst.
         * Moving those old frames to receiver time would permanently advance
         * this writer and poison subsequent pairing. Preserve its shared
         * timestamps; the recorder may discard frames already consumed.
         * This exception is one-sided: future stamps, generation changes,
         * invalid metadata and discontinuous clocks still use the bound. */
        int64_t raw_step = delta - p->raw_delta_us;
        int continuous = p->seen && !changed && !p->rebased && p->raw_valid &&
            raw_step >= 4000 && raw_step <= 6000;
        /* A provisional reconnect stamp may need a receiver-time offset.
         * Recover only after three advancing stamps fit the ORIGINAL shared
         * clock. A provisional cursor may be ahead of that clock: discard
         * already reserved indices until the shared clock catches up instead
         * of advancing the provisional cursor forever. A peer
         * on a genuinely different clock must remain explicitly unverified. */
        int64_t common_grid = delta >= 0 ? (delta + 2500) / 5000 : -((-delta + 2500) / 5000);
        common_grid += (int64_t)c->origin_index;
        int shared_candidate = p->seen && !changed && raw_step > 0 && common_grid >= 0 &&
            delta - expected >= -MIC_STEREO_CLOCK_SKEW_US &&
            delta - expected <= MIC_STEREO_CLOCK_SKEW_US;
        if (p->rebased && shared_candidate) {
            if (++p->recovery_frames >= 3) {
                p->offset_us = 0; p->rebased = 0; p->recovery_frames = 0;
                p->shared_catchup = 1;
            }
        } else p->recovery_frames = 0;
        p->raw_delta_us = delta;
        p->raw_valid = 1;
        if (p->shared_catchup) {
            if (shared_candidate && (uint64_t)common_grid <= p->last_index) {
                p->catchup_drops++;
                return -1; /* Never replay or retimestamp already reserved PCM. */
            }
            p->shared_catchup = 0;
        }
        int64_t error = delta + p->offset_us - expected;
        if ((error < -MIC_STEREO_CLOCK_SKEW_US && !continuous) ||
                error > MIC_STEREO_CLOCK_SKEW_US) {
            p->offset_us = expected - delta; p->rebased = 1; p->rebases++;
            if (!shared_candidate) p->recovery_frames = 0;
        }
        delta += p->offset_us;
        p->stamp_us = delta + (int64_t)c->origin_index * 5000;
        int64_t grid = delta >= 0 ? (delta + 2500) / 5000 : -((-delta + 2500) / 5000);
        grid += (int64_t)c->origin_index;
        if (grid < 0) return -1;
        *index = (uint64_t)grid;
        /* Independent nearest rounding can alternate between a skipped grid
         * cell and a repeated one at the half-frame boundary. That is not a
         * clock reversal: keep a one-frame cadence for an established 4--6 ms
         * clock while its rounded stamp is within one cell of the next index.
         * Preserve stamp_us (including its sub-frame phase) for paired-ring
         * matching; never turn this quantization jitter into a clock offset.
         * Missing frames, generation changes and rebased clocks do not qualify. */
        if (continuous && !p->rebased) {
            uint64_t next = p->last_index + 1;
            uint64_t distance = *index >= next ? *index - next : next - *index;
            if (distance <= 1) *index = next;
        }
        /* The parser already rejected duplicate/backward sequence numbers.
         * A non-advancing timestamp here must not mute every later PCM frame. */
        if (p->seen && !changed && *index <= p->last_index) {
            uint64_t next = arrival > p->last_index ? arrival : p->last_index + 1;
            p->offset_us += (int64_t)(next - *index) * 5000;
            *index = next; p->rebased = 1; p->rebases++;
            if (!shared_candidate) p->recovery_frames = 0;
        }
        fallback = p->rebased;
    } else {
        p->raw_valid = 0;
        p->recovery_frames = 0;
        p->shared_catchup = 0;
        *index = arrival;
        /* A callback can carry several frames with one receive timestamp. */
        if (p->seen && !changed && *index <= p->last_index) *index = p->last_index + 1;
    }
    p->seen = 1; p->generation = generation; p->last_index = *index;
    p->fallback_frames += !!fallback;
    return fallback;
}
#endif
