/* Included after recording observation serializers. No token or control lock. */
#ifndef MIC_RECORDING_TIMELINE_H
#define MIC_RECORDING_TIMELINE_H
static int recording_timeline_json(char *out, size_t size, uint64_t start, uint64_t after)
{
    mic_input_event events[MIC_AAC_TIMELINE_PAGE];
    mic_aac_stats s;
    char path[1539], boot[37];
    uint64_t epoch, latest, lost = 0;
    unsigned mode;
    int owned, count;
    pthread_mutex_lock(&E.rec_mtx);
    if (!E.rec_aac || !start || start != E.rec_start_ns) {
        pthread_mutex_unlock(&E.rec_mtx); return -1;
    }
    mic_json_string(path, E.rec_path);
    owned = E.rec_owner[0] != 0; mode = rec_channel_mode_locked();
    pthread_mutex_lock(&E.rec_aac->mutex);
    s = E.rec_aac->stats;
    latest = E.rec_aac->input_journal.latest;
    count = mic_input_page(&E.rec_aac->input_journal, after, events, &lost);
    pthread_mutex_unlock(&E.rec_aac->mutex);
    s.error |= E.rec_error;
    pthread_mutex_lock(&MT.mutex);
    memcpy(boot, MT.boot_id, sizeof boot); epoch = MT.epoch_ns;
    pthread_mutex_unlock(&MT.mutex);
    pthread_mutex_unlock(&E.rec_mtx);
    if (count < 0) return -1;
    uint64_t next = after + lost + (unsigned)count;
    int n = snprintf(out, size,
        "{\"recording_timeline_api\":1,\"clock_domain\":\"camera:CLOCK_MONOTONIC\","
        "\"mapping_verified\":false,\"sample0_monotonic_ns\":null,"
        "\"boot_id\":\"%s\",\"engine_start_monotonic_ns\":%" PRIu64 ","
        "\"recorder_start_monotonic_ns\":%" PRIu64 ",\"local_record_path\":%s,\"owned\":%s,"
        "\"rate\":48000,\"channels\":%u,\"channel_mode\":%u,"
        "\"after_cursor\":%" PRIu64 ",\"next_cursor\":%" PRIu64 ",\"latest_id\":%" PRIu64 ","
        "\"lost_events\":%" PRIu64 ",\"has_more\":%s,"
        "\"submitted_frames\":%" PRIu64 ",\"encoder_input_frames\":%" PRIu64 ","
        "\"encoder_delay_samples\":%u,\"completed\":%s,\"encoder_flushed\":%s,\"error\":%s,"
        "\"consumed_observed_monotonic_ns\":%" PRIu64 ",\"input_closed_monotonic_ns\":%" PRIu64 ","
        "\"flush_observed_monotonic_ns\":%" PRIu64 ",\"completed_observed_monotonic_ns\":%" PRIu64 ",\"events\":[",
        boot, epoch, start, path, owned ? "true" : "false", s.channels, mode,
        after, next, latest, lost, next < latest ? "true" : "false", s.submitted, s.input_samples,
        s.encoder_delay_samples, s.completed ? "true" : "false", s.encoder_flushed ? "true" : "false",
        s.error ? "true" : "false", s.consumed_observed_ns, s.input_closed_ns,
        s.flush_observed_ns, s.completed_observed_ns);
    if (n < 0 || (size_t)n >= size) return -1;
    size_t used = (size_t)n;
    for (int i = 0; i < count; i++) {
        char block[1800];
        unsigned muted = 0;
        if (events[i].kind == MIC_INPUT_QUEUED) for (unsigned slot = 0; slot < 2; slot++)
            if (events[i].block.origins[slot].event_id && events[i].block.origins[slot].muted)
                muted |= 1u << slot;
        if (recording_submission_json(&events[i].block, events[i].kind == MIC_INPUT_QUEUED,
                                      block, sizeof block) < 0) return -1;
        n = snprintf(out + used, size - used, "%s{\"id\":%" PRIu64 ",\"kind\":\"%s\",\"muted_tx_mask\":%u,\"block\":%s}",
            i ? "," : "", events[i].id, events[i].kind == MIC_INPUT_QUEUED ? "queued" :
            events[i].kind == MIC_INPUT_OVERFLOW ? "overflow" : "padding", muted, block);
        if (n < 0 || (size_t)n >= size - used) return -1;
        used += (size_t)n;
    }
    n = snprintf(out + used, size - used, "]}");
    return n < 0 || (size_t)n >= size - used ? -1 : (int)(used + (size_t)n);
}
#endif
