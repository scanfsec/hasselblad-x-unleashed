/* Read-only per-take AAC queue observations. rec_mtx binds path, identity and
 * sink snapshot; no feed/control lock, token disclosure or clock fitting. */
#ifndef MIC_RECORDING_OBSERVATION_H
#define MIC_RECORDING_OBSERVATION_H

static int recording_origin_json(const mic_pcm_origin *p, char *out, size_t size)
{
    int n;
    if (!p->event_id) n = snprintf(out, size, "null");
    else n = snprintf(out, size,
        "{\"event_id\":%" PRIu64 ",\"tx_slot\":%u,\"stream_generation\":%" PRIu64 ","
        "\"first_sample_index\":%" PRIu64 ",\"samples\":%u,"
        "\"receive_monotonic_ns\":%" PRIu64 ",\"receive_end_monotonic_ns\":%" PRIu64 ","
        "\"decoded_observed_monotonic_ns\":%" PRIu64 ",\"synthetic\":%s,\"receive_bounds_merged\":%s,\"source\":\"%s\","
        "\"tx_address\":\"%02x:%02x:%02x:%02x:%02x:%02x\"}",
        p->event_id, p->slot, p->generation, p->first_sample, p->samples,
        p->receive_ns, p->receive_end_ns, p->observed_ns, p->synthetic ? "true" : "false", p->merged ? "true" : "false",
        p->source == 2 ? "uplink_recv_address_verified" : p->source == 4 ? "feed_entry_transport_address"
        : "feed_entry_configured_address_unverified",
        p->bd[0], p->bd[1], p->bd[2], p->bd[3], p->bd[4], p->bd[5]);
    return n < 0 || (size_t)n >= size ? -1 : n;
}

static int recording_submission_json(const mic_pcm_submission *p, int sources, char *out, size_t size)
{
    int n;
    if (!p->frames) n = snprintf(out, size, "null");
    else {
        char origins[2][720] = {"null", "null"};
        if (sources) for (unsigned i = 0; i < 2; i++)
            if (recording_origin_json(&p->origins[i], origins[i], sizeof origins[i]) < 0) return -1;
        n = snprintf(out, size,
            "{\"file_input_sample_index\":%" PRIu64 ",\"frames\":%" PRIu64 ","
            "\"enqueue_observed_monotonic_ns\":%" PRIu64 ",\"real_tx_mask\":%u,\"origins\":[%s,%s]}",
            p->first_sample, p->frames, p->enqueue_ns, p->real_mask, origins[0], origins[1]);
    }
    return n < 0 || (size_t)n >= size ? -1 : n;
}

static int recording_input_json(char *out, size_t size)
{
    if (!out || !size) return -1;
    char path[1539], boot[37];
    uint64_t start, epoch;
    unsigned channel_mode;
    int owned, available;
    mic_aac_stats s = {0};
    pthread_mutex_lock(&E.rec_mtx);
    mic_json_string(path, E.rec_path);
    start = E.rec_start_ns;
    channel_mode = rec_channel_mode_locked();
    owned = E.rec_owner[0] != 0;
    available = E.rec_aac != NULL;
    if (available) s = mic_aac_snapshot(E.rec_aac);
    s.error |= E.rec_error;
    pthread_mutex_lock(&MT.mutex);
    memcpy(boot, MT.boot_id, sizeof boot); epoch = MT.epoch_ns;
    pthread_mutex_unlock(&MT.mutex);
    pthread_mutex_unlock(&E.rec_mtx);
    char first[1800], last[1800], dropped_first[260], dropped_last[260], padding[260];
    if (recording_submission_json(&s.first_queued, 1, first, sizeof first) < 0 ||
        recording_submission_json(&s.last_queued, 1, last, sizeof last) < 0 ||
        recording_submission_json(&s.first_overflow, 0, dropped_first, sizeof dropped_first) < 0 ||
        recording_submission_json(&s.last_overflow, 0, dropped_last, sizeof dropped_last) < 0 ||
        recording_submission_json(&s.last_padding, 0, padding, sizeof padding) < 0) return -1;
    int n = snprintf(out, size,
        "{\"recording_input_api\":1,\"available\":%s,\"coverage\":\"first_last_queued_chunks\","
        "\"clock_domain\":\"camera:CLOCK_MONOTONIC\",\"mapping_verified\":false,"
        "\"sample0_monotonic_ns\":null,\"boot_id\":\"%s\",\"engine_start_monotonic_ns\":%" PRIu64 ","
        "\"recorder_start_monotonic_ns\":%" PRIu64 ",\"local_record_path\":%s,\"owned\":%s,"
        "\"rate\":48000,\"channels\":%u,\"channel_mode\":%u,\"submitted_frames\":%" PRIu64 ","
        "\"encoder_input_frames\":%" PRIu64 ",\"encoder_delay_samples\":%u,"
        "\"completed\":%s,\"error\":%s,\"queued_chunks\":%" PRIu64 ","
        "\"first_queued\":%s,\"last_queued\":%s,\"overflow_frames\":%" PRIu64 ","
        "\"first_overflow\":%s,\"last_overflow\":%s,\"padding_frames\":%" PRIu64 ",\"last_padding\":%s}",
        available ? "true" : "false", boot, epoch, start, path, owned ? "true" : "false",
        s.channels, channel_mode, s.submitted, s.input_samples, s.encoder_delay_samples,
        s.completed ? "true" : "false", s.error ? "true" : "false", s.queued_chunks,
        first, last, s.overflow_samples, dropped_first, dropped_last, s.padding_frames, padding);
    return n < 0 || (size_t)n >= size ? -1 : n;
}
#endif
