/* Observation only. No clock fitting, BT unwrapping, or media resampling.
 * Callers never hold this mutex across decoder, transport or file I/O.
 * Ring reads are non-consuming: independent clients cannot steal evidence.
 */
#ifndef MIC_TIMING_H
#define MIC_TIMING_H
#include <inttypes.h>
#include "mic_pcm_observation.h"
#define MIC_TIMING_CAP 2048
#define MIC_TIMING_SPANS 64
struct mic_receive_span {
    size_t bytes;
    uint64_t first_ns, last_ns;
    int synthetic, merged, address_verified;
};
struct mic_timing_event {
    uint64_t id, generation, first_sample, receive_ns, receive_end_ns, observed_ns;
    uint32_t btclk;
    uint16_t intra, seq;
    unsigned samples, missing, slot;
    int clock_valid, synthetic, merged, source, result;
    const char *kind, *reason;
    uint8_t bd[6];
    unsigned raw_len;
    uint8_t raw[64];
};
static struct {
    pthread_mutex_t mutex;
    char boot_id[37];
    uint64_t epoch_ns, serial;
    struct mic_timing_event ring[MIC_TIMING_CAP];
} MT = { .mutex = PTHREAD_MUTEX_INITIALIZER };
/* Parser metadata and context are feed_mtx protected. Synthetic native calls
 * are synchronous on the originating thread; never infer authenticity from a
 * packet's sender byte. Even authentic observations are NOT calibration. */
static _Thread_local int timing_synthetic_depth;
struct mic_timing_stream {
    uint64_t generation, next_sample;
    uint64_t concealed, missing, duplicate, backward, clock_decrease, metadata_merges;
    uint32_t last_clock;
    int have_clock;
    struct mic_receive_span spans[MIC_TIMING_SPANS];
    size_t span_count;
    struct mic_timing_event context;
    /* feed_mtx protected, cumulative since engine start; index 0 is the
     * stream entry, index 1 the stock uplink datagram consumer. */
    struct { uint64_t calls, bytes, wait_ns, dispatch_ns; } ingress[2];
    uint64_t checksum_rejections, stream_resync_bytes, stream_overflows;
};
static struct mic_timing_stream TS[2];

static uint64_t timing_now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static void timing_ingress_begin(unsigned slot, unsigned source, size_t bytes,
                                 uint64_t received, uint64_t acquired)
{
    TS[slot].ingress[source].calls++;
    TS[slot].ingress[source].bytes += bytes;
    uint64_t wait = acquired >= received ? acquired - received : 0;
    if (wait > TS[slot].ingress[source].wait_ns) TS[slot].ingress[source].wait_ns = wait;
}

static void timing_ingress_end(unsigned slot, unsigned source, uint64_t acquired, uint64_t ended)
{
    uint64_t elapsed = ended >= acquired ? ended - acquired : 0;
    if (elapsed > TS[slot].ingress[source].dispatch_ns) TS[slot].ingress[source].dispatch_ns = elapsed;
}

static void timing_init(void)
{
    char boot[40] = {0};
    FILE *f = fopen("/proc/sys/kernel/random/boot_id", "r");
    if (f) { if (!fgets(boot, sizeof boot, f)) boot[0] = 0; fclose(f); }
    boot[strcspn(boot, "\r\n")] = 0;
    int valid = strlen(boot) == 36;
    for (size_t i = 0; valid && i < 36; i++)
        if (!(isxdigit((unsigned char)boot[i]) || boot[i] == '-')) valid = 0;
    pthread_mutex_lock(&MT.mutex);
    MT.boot_id[0] = 0;
    if (valid) memcpy(MT.boot_id, boot, 37);
    MT.epoch_ns = timing_now_ns();
    MT.serial = 0;
    memset(TS, 0, sizeof TS);
    pthread_mutex_unlock(&MT.mutex);
}

static void timing_append_span(unsigned slot, size_t bytes, uint64_t ns, int synthetic)
{
    if (TS[slot].span_count == MIC_TIMING_SPANS) {
        /* Keep conservative bounds when fragmentation exceeds capacity. */
        TS[slot].spans[1].bytes += TS[slot].spans[0].bytes;
        if (TS[slot].spans[0].first_ns < TS[slot].spans[1].first_ns)
            TS[slot].spans[1].first_ns = TS[slot].spans[0].first_ns;
        if (TS[slot].spans[0].last_ns > TS[slot].spans[1].last_ns)
            TS[slot].spans[1].last_ns = TS[slot].spans[0].last_ns;
        TS[slot].spans[1].synthetic |= TS[slot].spans[0].synthetic;
        TS[slot].spans[1].address_verified &= TS[slot].spans[0].address_verified;
        TS[slot].spans[1].merged = 1;
        memmove(TS[slot].spans, TS[slot].spans + 1, (--TS[slot].span_count) * sizeof *TS[slot].spans);
        pthread_mutex_lock(&MT.mutex); TS[slot].metadata_merges++; pthread_mutex_unlock(&MT.mutex);
    }
    TS[slot].spans[TS[slot].span_count++] = (struct mic_receive_span){bytes, ns, ns, synthetic, 0, 0};
}

static void timing_consume(unsigned slot, size_t bytes)
{
    while (bytes && TS[slot].span_count) {
        if (bytes < TS[slot].spans[0].bytes) { TS[slot].spans[0].bytes -= bytes; break; }
        bytes -= TS[slot].spans[0].bytes;
        memmove(TS[slot].spans, TS[slot].spans + 1, (--TS[slot].span_count) * sizeof *TS[slot].spans);
    }
}

static void timing_receive_context(unsigned slot, size_t offset, size_t bytes, const uint8_t bd[6])
{
    memset(&TS[slot].context, 0, sizeof TS[slot].context);
    TS[slot].context.source = 1; /* miccore_feed entry, not controller/ADC time */
    memcpy(TS[slot].context.bd, bd, 6); /* configured target, attribution unverified */
    size_t end = offset + bytes, pos = 0;
    int seen = 0, verified = 1;
    for (size_t i = 0; i < TS[slot].span_count && pos < end; i++) {
        struct mic_receive_span *s = &TS[slot].spans[i];
        if (pos + s->bytes > offset) {
            seen = 1;
            verified &= s->address_verified;
            if (!TS[slot].context.receive_ns || s->first_ns < TS[slot].context.receive_ns)
                TS[slot].context.receive_ns = s->first_ns;
            if (s->last_ns > TS[slot].context.receive_end_ns) TS[slot].context.receive_end_ns = s->last_ns;
            TS[slot].context.synthetic |= s->synthetic;
            TS[slot].context.merged |= s->merged;
        }
        pos += s->bytes;
    }
    if (seen && verified) TS[slot].context.source = 4;
}

static void timing_new_stream(unsigned slot)
{
    pthread_mutex_lock(&MT.mutex);
    TS[slot].generation++;
    TS[slot].next_sample = 0;
    TS[slot].have_clock = 0;
    pthread_mutex_unlock(&MT.mutex);
}

static struct mic_timing_event timing_publish(unsigned slot, struct mic_timing_event event)
{
    if (slot >= 2) return (struct mic_timing_event){0};
    event.slot = slot;
    event.observed_ns = timing_now_ns();
    pthread_mutex_lock(&MT.mutex);
    event.id = ++MT.serial;
    event.generation = TS[slot].generation;
    event.first_sample = TS[slot].next_sample;
    TS[slot].next_sample += event.samples;
    if (event.samples && event.synthetic) TS[slot].concealed += event.samples;
    TS[slot].missing += event.missing;
    if (!strcmp(event.reason, "duplicate")) TS[slot].duplicate++;
    if (!strcmp(event.reason, "backward_sequence")) TS[slot].backward++;
    if (event.samples && event.clock_valid && !event.synthetic) {
        if (TS[slot].have_clock && event.btclk < TS[slot].last_clock) TS[slot].clock_decrease++;
        TS[slot].have_clock = 1; TS[slot].last_clock = event.btclk;
    }
    MT.ring[(event.id - 1) % MIC_TIMING_CAP] = event;
    pthread_mutex_unlock(&MT.mutex);
    return event;
}

static struct mic_timing_event timing_pcm(unsigned slot, unsigned samples, int concealed)
{
    struct mic_timing_event e = TS[slot].context;
    e.kind = "pcm"; e.reason = concealed ? "sequence_gap_concealment" : "decoded";
    e.samples = samples;
    e.synthetic |= concealed;
    if (concealed) e.clock_valid = 0; /* never borrow the following packet clock */
    return timing_publish(slot, e);
}

static mic_pcm_origin timing_pcm_origin(struct mic_timing_event e)
{
    mic_pcm_origin p = {.event_id = e.id, .generation = e.generation, .first_sample = e.first_sample,
        .receive_ns = e.receive_ns, .receive_end_ns = e.receive_end_ns, .observed_ns = e.observed_ns,
        .samples = e.samples, .slot = e.slot, .source = e.source, .synthetic = e.synthetic, .merged = e.merged};
    memcpy(p.bd, e.bd, sizeof p.bd);
    return p;
}

static void timing_packet_note(unsigned slot, const char *reason, unsigned missing)
{
    struct mic_timing_event e = TS[slot].context;
    e.kind = "packet"; e.reason = reason; e.samples = 0; e.missing = missing;
    timing_publish(slot, e);
}

/* Wire fields are retained as bytes. Their units/event semantics are not
 * certified. No request/reply pairing or usable calibration anchor is claimed. */
static void timing_sync(unsigned slot, const uint8_t *d, int n, int transmit,
                        uint64_t begin, uint64_t end, int result, const uint8_t *bd)
{
    if (n < 13 || d[9] != 0x12 || (d[10] != 0x15 && d[10] != 0x56 && d[10] != 0x58)) return;
    struct mic_timing_event e = transmit ? (struct mic_timing_event){0} : TS[slot].context;
    e.kind = transmit ? "sync_tx" : "sync_rx";
    e.reason = "raw_unverified";
    e.clock_valid = 0;
    e.samples = e.missing = 0;
    e.seq = (uint16_t)(d[6] | (d[7] << 8));
    e.result = result;
    if (bd) memcpy(e.bd, bd, 6);
    if (transmit) { e.receive_ns = begin; e.receive_end_ns = end; e.source = 3; }
    e.raw_len = n < (int)sizeof e.raw ? (unsigned)n : sizeof e.raw;
    memcpy(e.raw, d, e.raw_len);
    timing_publish(slot, e);
}

static int timing_summary(unsigned slot, char *out, size_t size)
{
    pthread_mutex_lock(&MT.mutex);
    int n = snprintf(out, size,
        "{\"timing_version\":1,\"boot_id\":\"%s\",\"engine_start_monotonic_ns\":%" PRIu64 ","
        "\"stream_generation\":%" PRIu64 ",\"next_sample_index\":%" PRIu64 ",\"latest_event_id\":%" PRIu64 ","
        "\"sample0_monotonic_ns\":null,\"mapping_verified\":false,\"drift_ppm\":null,\"residual_ns\":null,\"error_bound_ns\":null,"
        "\"bt_clock_bits\":null,\"intra_unit\":null,\"calibration_anchors\":0,"
        "\"missing_packets\":%" PRIu64 ",\"concealed_samples\":%" PRIu64 ",\"duplicates\":%" PRIu64 ","
        "\"backward_sequences\":%" PRIu64 ",\"clock_decreases_unclassified\":%" PRIu64 ",\"metadata_merges\":%" PRIu64 ","
        "\"ingress\":{\"feed\":{\"calls\":%" PRIu64 ",\"bytes\":%" PRIu64 ",\"max_wait_ns\":%" PRIu64 ",\"max_dispatch_ns\":%" PRIu64 "},"
        "\"uplink\":{\"calls\":%" PRIu64 ",\"bytes\":%" PRIu64 ",\"max_wait_ns\":%" PRIu64 ",\"max_dispatch_ns\":%" PRIu64 "},"
        "\"checksum_rejections\":%" PRIu64 ",\"stream_resync_bytes\":%" PRIu64 ",\"stream_overflows\":%" PRIu64 "}}",
        MT.boot_id, MT.epoch_ns, TS[slot].generation, TS[slot].next_sample, MT.serial,
        TS[slot].missing, TS[slot].concealed, TS[slot].duplicate, TS[slot].backward, TS[slot].clock_decrease, TS[slot].metadata_merges,
        TS[slot].ingress[0].calls, TS[slot].ingress[0].bytes, TS[slot].ingress[0].wait_ns, TS[slot].ingress[0].dispatch_ns,
        TS[slot].ingress[1].calls, TS[slot].ingress[1].bytes, TS[slot].ingress[1].wait_ns, TS[slot].ingress[1].dispatch_ns,
        TS[slot].checksum_rejections, TS[slot].stream_resync_bytes, TS[slot].stream_overflows);
    pthread_mutex_unlock(&MT.mutex);
    return n < 0 || (size_t)n >= size ? -1 : n;
}

static int timing_page(char *out, size_t size, uint64_t after)
{
    if (!out || size < 1600) return -1;
    /* Existing observers read at most 8192 bytes including the newline.
     * Fill that byte budget instead of limiting each round trip to four
     * events, which can fall behind the two TXs' packet + PCM observations. */
    if (size > 8191) size = 8191;
    pthread_mutex_lock(&MT.mutex);
    uint64_t first = MT.serial > MIC_TIMING_CAP ? MT.serial - MIC_TIMING_CAP + 1 : 1;
    uint64_t cursor = after < first - 1 ? first - 1 : after;
    uint64_t lost = cursor > after ? cursor - after : 0;
    if (after > MT.serial) { pthread_mutex_unlock(&MT.mutex); return -1; }
    int n = snprintf(out, size, "{\"timing_version\":1,\"boot_id\":\"%s\",\"engine_start_monotonic_ns\":%" PRIu64 ","
        "\"clock_domain\":\"camera:CLOCK_MONOTONIC\",\"lost_events\":%" PRIu64 ",\"events\":[", MT.boot_id, MT.epoch_ns, lost);
    unsigned emitted = 0;
    while (cursor < MT.serial) {
        struct mic_timing_event *e = &MT.ring[cursor % MIC_TIMING_CAP];
        char row[1400], clock[24] = "null", intra[16] = "null", raw[129];
        if (e->clock_valid) { snprintf(clock, sizeof clock, "%u", e->btclk); snprintf(intra, sizeof intra, "%u", e->intra); }
        for (unsigned i = 0; i < e->raw_len; i++) snprintf(raw + 2*i, 3, "%02x", e->raw[i]);
        raw[2*e->raw_len] = 0;
        int k = snprintf(row, sizeof row,
            "%s{\"id\":%" PRIu64 ",\"stream_generation\":%" PRIu64 ",\"kind\":\"%s\",\"reason\":\"%s\","
            "\"tx_address\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"source\":\"%s\","
            "\"receive_monotonic_ns\":%" PRIu64 ",\"receive_end_monotonic_ns\":%" PRIu64 ",\"observed_monotonic_ns\":%" PRIu64 ","
            "\"tx_slot\":%u,\"receive_bounds_merged\":%s,\"first_sample_index\":%" PRIu64 ",\"samples\":%u,\"seq\":%u,"
            "\"btclk_raw\":%s,\"intra_raw\":%s,\"synthetic\":%s,\"calibration_eligible\":false,\"missing_packets\":%u,"
            "\"transport_result\":%d,\"raw_duss_hex\":\"%s\"}", emitted ? "," : "", e->id, e->generation, e->kind, e->reason,
            e->bd[0], e->bd[1], e->bd[2], e->bd[3], e->bd[4], e->bd[5],
            e->source == 2 ? "uplink_recv_address_verified" : e->source == 3 ? "send_call_bounds" :
            e->source == 4 ? "feed_entry_transport_address" : "feed_entry_configured_address_unverified",
            e->receive_ns, e->receive_end_ns, e->observed_ns, e->slot, e->merged ? "true" : "false", e->first_sample, e->samples, e->seq,
            clock, intra, e->synthetic ? "true" : "false", e->missing, e->result, raw);
        if (k < 0 || (size_t)k >= sizeof row || n < 0 || (size_t)n + (size_t)k + 110 >= size) break;
        memcpy(out + n, row, (size_t)k); n += k; cursor++; emitted++;
    }
    int k = snprintf(out + n, size - (size_t)n, "],\"next_cursor\":%" PRIu64 ",\"has_more\":%s}", cursor, cursor < MT.serial ? "true" : "false");
    pthread_mutex_unlock(&MT.mutex);
    return n + k;
}
#endif
