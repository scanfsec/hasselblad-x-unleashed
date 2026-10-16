/* Live PCM is opt-in through a separate, fixed-format FIFO. feed_mtx owns
 * this state and excludes decoder callbacks, status and shutdown. Recording
 * has its own queues: a slow live reader must never drop recording samples. */
#ifndef MIC_PCM_STEREO_H
#define MIC_PCM_STEREO_H
static struct {
    int fd;
    unsigned mode;
    uint64_t session, start_ms, next_open_ms, written_samples, dropped_samples;
    struct mic_stereo queue;
    struct mic_stereo_clock clock;
} PCM2 = {.fd = -1};

static void pcm_stereo_close(void)
{
    if (PCM2.fd >= 0) close(PCM2.fd);
    PCM2.fd = -1;
}

static int pcm_stereo_output(void *unused, const int16_t *pcm, unsigned frames, unsigned real_mask)
{
    (void)unused; (void)real_mask;
    if (PCM2.fd < 0 || frames != MIC_STEREO_SAMPLES) return -1;
    int16_t routed[MIC_STEREO_SAMPLES * 2];
    if (PCM2.mode != 2) {
        route_stereo(routed, pcm, frames, PCM2.mode);
        pcm = routed;
    }
    ssize_t n;
    do { n = write(PCM2.fd, pcm, frames * 4); } while (n < 0 && errno == EINTR);
    if (n == (ssize_t)frames * 4) { PCM2.written_samples += frames; return 0; }
    PCM2.dropped_samples += frames;
    /* Each 960-byte write is <= this FIFO's PIPE_BUF, so nonblocking EAGAIN
     * drops one whole block, never half a sample/channel. No unbounded backlog. */
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    pcm_stereo_close();
    return -1;
}

static void pcm_stereo_tick(uint64_t now)
{
    if (PCM2.fd < 0) {
        if (now < PCM2.next_open_ms) return;
        PCM2.next_open_ms = now + 250;
        int fd = open(STEREO_FIFO_PATH, O_WRONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) return; /* No reader: retain no audio and do no assembly. */
        struct stat st;
        if (fstat(fd, &st) || !S_ISFIFO(st.st_mode) || fpathconf(fd, _PC_PIPE_BUF) < MIC_STEREO_SAMPLES * 4) {
            close(fd); return;
        }
        PCM2.fd = fd; PCM2.session++; PCM2.start_ms = now;
        PCM2.written_samples = PCM2.dropped_samples = 0;
        pthread_mutex_lock(&E.rec_mtx);
        PCM2.mode = channel_mode_locked();
        pthread_mutex_unlock(&E.rec_mtx);
        mic_stereo_init(&PCM2.queue, 3);
        mic_stereo_advance(&PCM2.queue, 0);
        mic_stereo_clock_init(&PCM2.clock, now);
    }
    if (now < PCM2.start_ms) return;
    unsigned active = 0;
    pthread_mutex_lock(&E.st_mtx);
    for (unsigned slot = 0; slot < 2; slot++)
        if (TX[slot].connected && !TX[slot].stream_lost && !TX[slot].user_hold)
            active |= 1u << slot;
    pthread_mutex_unlock(&E.st_mtx);
    mic_stereo_set_active(&PCM2.queue, active);
    if (mic_stereo_drain_before(&PCM2.queue, now, 0, (now - PCM2.start_ms) / 5,
            pcm_stereo_output, NULL) < 0) return;
    /* A bounded drain may leave real frames queued beyond a large gap. Let
     * subsequent ticks drain them; zero padding must never purge that audio. */
    if (PCM2.queue.channel[0].count || PCM2.queue.channel[1].count) return;
    /* Same wait budget as paired consumption; otherwise padding could retire
     * a valid late partner before the ring's own deadline. */
    uint64_t elapsed = now - PCM2.start_ms;
    uint64_t limit = elapsed > PCM2.queue.wait_ms ? (elapsed - PCM2.queue.wait_ms) / 5 : 0;
    static const int16_t zero[MIC_STEREO_SAMPLES * 2] = {0};
    for (unsigned i = 0; PCM2.fd >= 0 && i < MIC_STEREO_CAP && PCM2.queue.next_index < limit; i++) {
        if (pcm_stereo_output(NULL, zero, MIC_STEREO_SAMPLES, 0)) break;
        mic_stereo_advance(&PCM2.queue, PCM2.queue.next_index + 1);
    }
}

static void pcm_stereo_feed(struct mic_tx *tx, const int16_t *pcm, int samples,
    const mic_pcm_origin *origin)
{
    if (PCM2.fd < 0 || samples != MIC_STEREO_SAMPLES) return;
    unsigned slot = (unsigned)(tx - TX);
    const struct mic_timing_event *event = &TS[slot].context;
    uint64_t now = now_ms(), received = event->receive_ns ? event->receive_ns / 1000000 : now, index;
    pthread_mutex_lock(&E.st_mtx);
    int real = tx->connected && !tx->stream_lost && !tx->user_hold && tx->cfg_bd_valid &&
        tx->tx_status_ms && now >= tx->tx_status_ms && now - tx->tx_status_ms < 10000 && !(tx->tx_flags & 1);
    pthread_mutex_unlock(&E.st_mtx);
    if (pcm_concealed && (pcm_concealed != 1 || !real)) return;
    if (PCM2.clock.peer[slot].seen && PCM2.clock.peer[slot].generation != TS[slot].generation)
        mic_stereo_reset_channel(&PCM2.queue, slot, 1);
    if (pcm_concealed) {
        mic_stereo_push_concealed(&PCM2.queue, slot, received, pcm, origin);
        return;
    }
    int mapped = mic_stereo_clock_index(&PCM2.clock, slot, TS[slot].generation, received,
            event->clock_valid && !event->synthetic, event->btclk, event->intra, &index);
    if (mapped >= 0)
        mic_stereo_push_timestamp(&PCM2.queue, slot, index, received,
            mapped == 0, PCM2.clock.peer[slot].stamp_us, pcm, real, origin);
}

static int pcm_stereo_status(char *out, size_t size)
{
    int n = snprintf(out, size,
        "{\"path\":\"" STEREO_FIFO_PATH "\",\"format\":\"s16le\",\"sample_rate\":48000,\"channels\":2,"
        "\"active\":%s,\"channel_mode\":%u,\"session\":%llu,\"start_monotonic_ms\":%llu,"
        "\"written_samples\":%llu,\"dropped_samples\":%llu,\"mapping_verified\":false,"
        "\"receiver_fallback_frames\":[%llu,%llu],\"clock_rebases\":[%llu,%llu],"
        "\"clock_catchup\":[%s,%s],\"clock_catchup_dropped_frames\":[%llu,%llu],"
        "\"algorithm\":\"paired_ring_v1\",\"capacity_frames\":%u,\"wait_ms\":%u,\"suspended\":[%s,%s],"
        "\"concealed_samples\":[%llu,%llu]}",
        PCM2.fd >= 0 ? "true" : "false", PCM2.mode ? PCM2.mode : 2,
        (unsigned long long)PCM2.session, (unsigned long long)PCM2.start_ms,
        (unsigned long long)PCM2.written_samples, (unsigned long long)PCM2.dropped_samples,
        (unsigned long long)PCM2.clock.peer[0].fallback_frames,
        (unsigned long long)PCM2.clock.peer[1].fallback_frames,
        (unsigned long long)PCM2.clock.peer[0].rebases,
        (unsigned long long)PCM2.clock.peer[1].rebases,
        PCM2.clock.peer[0].shared_catchup ? "true" : "false",
        PCM2.clock.peer[1].shared_catchup ? "true" : "false",
        (unsigned long long)PCM2.clock.peer[0].catchup_drops,
        (unsigned long long)PCM2.clock.peer[1].catchup_drops,
        MIC_STEREO_CAP, PCM2.fd >= 0 ? PCM2.queue.wait_ms : MIC_STEREO_WAIT_MS,
        PCM2.queue.channel[0].suspended ? "true" : "false",
        PCM2.queue.channel[1].suspended ? "true" : "false",
        (unsigned long long)PCM2.queue.channel[0].concealed_samples,
        (unsigned long long)PCM2.queue.channel[1].concealed_samples);
    return n < 0 || (size_t)n >= size ? -1 : n;
}

static void *pcm_stereo_thread(void *unused)
{
    (void)unused;
    while (!atomic_load(&engine_stopping)) {
        pthread_mutex_lock(&E.feed_mtx);
        pcm_stereo_tick(now_ms());
        unsigned wait_ms = PCM2.fd >= 0 ? 5 : 250;
        pthread_mutex_unlock(&E.feed_mtx);
        engine_wait_ms(wait_ms);
    }
    return NULL;
}
#endif
