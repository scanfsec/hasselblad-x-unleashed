/* Mic 2 WL1 settings. Included by miccore.c; no separate engine/thread.
 * Wire contract: Action 5 libwirelessmicservice 0x38640 / 0x3bb40.
 * Gain uses value+12 (set_tx_in_gain @0x2796c); range bytes f4/0c
 * in init_dji_mic_2_capability_info_unlocked @0x39744.
 * Only expose fields confirmed by a fresh CRC-validated WL1 report.
 */
#ifndef TX_DSOCK
#define TX_DSOCK "/dsock/57011"
#endif

struct tx_setting {
    const char *name;
    int id, min, max, shift, extra;
};
static const struct tx_setting tx_settings[] = {
    {"gain", 3, -12, 12, 0, 2},
    {"mute", 5, 0, 1, 0, 0},
    {"record", 6, 0, 1, 5, 0},
    {"vibrate", 8, 0, 1, 3, 0},
    {"lowcut", 10, 0, 1, 1, 0},
    {"highcut", 16, 0, 1, 14, 0},
    {"led", 15, 0, 2, 12, 0},
    {"auto_record", 13, 0, 1, 10, 0},
    {"record_button", 14, 0, 1, 11, 0},
    {"denoise", 17, 0, 1, 15, 0},
    {"auto_shutdown", 18, 0, 1, 2, 1},
    {"denoise_button", 19, 0, 1, 3, 1}
};
#define TX_SETTING_COUNT (sizeof tx_settings / sizeof tx_settings[0])

static int tx_ready(struct mic_tx *tx)
{
    return tx->wl_protocol != 2 && tx->paired && tx->connected && tx->cfg_bd_valid && tx->tx_status_ms
        && now_ms() - tx->tx_status_ms < 10000;
}

static void tx_format_expire(struct mic_tx *tx)
{
    if (tx->tx_format_result == 1 && (!tx_ready(tx) || now_ms() - tx->tx_format_ms > 60000))
        tx->tx_format_result = 4;
}

/* Caller holds st_mtx. Stale telemetry cannot indefinitely block local WAVs. */
static int tx_format_active(struct mic_tx *tx)
{
    tx_format_expire(tx);
    return tx->tx_format_result == 1 || (tx_ready(tx) && ((tx->tx_flags >> 6) & 3) == 1);
}

static int tx_json_append(char *out, size_t cap, size_t *used, const char *fmt, ...)
{
    if (*used >= cap) return -1;
    va_list ap;
    va_start(ap, fmt);
    int written = vsnprintf(out + *used, cap - *used, fmt, ap);
    va_end(ap);
    if (written < 0 || (size_t)written >= cap - *used) return -1;
    *used += (size_t)written;
    return 0;
}

/* Caller holds st_mtx; return -1 with no partial JSON on insufficient space. */
static int tx_status_json(struct mic_tx *tx, char *out, size_t cap)
{
    size_t i, n = 0;
    if (!out || !cap) return -1;
    tx_format_expire(tx);
    if (tx->wl_protocol == 2 && tx->connected && tx->paired && tx->wl2_cap.present) {
        const struct wl2_capability *c = &tx->wl2_cap;
        char model[303] = "null", version[24] = "null", total[16] = "null";
        char denoise[16] = "null", led[16] = "null", channel[16] = "null";
        char encoding[240] = "null", sampling[128] = "null";
        if (c->present & (1u << 13)) mic_json_string(model, c->model);
        if (c->present & (1u << 1)) snprintf(version, sizeof version, "\"%u.%u.%u.%u\"",
            c->version[3], c->version[2], c->version[1], c->version[0]);
        if (c->present & (1u << 9)) snprintf(total, sizeof total, "%u", c->total_mib);
        if (c->present & (1u << 10)) snprintf(denoise, sizeof denoise, "%u", c->denoise_mask);
        if (c->present & (1u << 11)) snprintf(led, sizeof led, "%u", c->led_mask);
        if (c->present & (1u << 12)) snprintf(channel, sizeof channel, "%u", c->channel_mask);
        if (c->present & (1u << 5)) snprintf(encoding, sizeof encoding,
            "{\"type_mask\":%u,\"frame_duration_mask\":%u,\"format_mask\":%u,\"max_bytes\":%u,\"bitrate\":%u}",
            c->encode_type_mask, c->frame_duration_mask, c->encode_format_mask,
            c->encode_maxbytes, c->encode_bitrate);
        if (c->present & (1u << 6)) snprintf(sampling, sizeof sampling,
            "{\"rate_mask\":%u,\"bits\":%u,\"channel_mask\":%u}",
            c->sample_rate_mask, c->sample_bits, c->sample_channel_mask);
        if (tx_json_append(out, cap, &n,
            "{\"supported\":false,\"settings\":{},\"protocol\":\"wl2\",\"capabilities\":{"
            "\"model\":%s,\"firmware_version\":%s,\"total_mib\":%s,\"denoise_mask\":%s,"
            "\"led_mask\":%s,\"channel_mask\":%s,\"encoding\":%s,\"sampling\":%s},\"format_result\":%d,\"format_request\":\"%llu\"}",
            model, version, total, denoise, led, channel, encoding, sampling, tx->tx_format_result,
            (unsigned long long)tx->tx_format_request) < 0) goto truncated;
        return (int)n;
    }
    if (tx_json_append(out, cap, &n, "{\"supported\":%s,\"settings\":{", tx_ready(tx) ? "true" : "false") < 0)
        goto truncated;
    if (tx_ready(tx)) for (i = 0; i < TX_SETTING_COUNT; i++) {
        const struct tx_setting *s = &tx_settings[i];
        int value = s->extra == 2 ? tx->mic_gain
            : (int)(((s->extra ? tx->tx_extra : tx->tx_flags) >> s->shift) & (s->id == 15 ? 3 : 1));
        if (tx_json_append(out, cap, &n,
            "%s\"%s\":{\"value\":%d,\"min\":%d,\"max\":%d%s}",
            i ? "," : "", s->name, value, s->min, s->max,
            s->id == 15 ? ",\"options\":[0,2]" : "") < 0) goto truncated;
    }
    if (tx_ready(tx)) {
        char free_space[24] = "null", recordable[24] = "null";
        int mounted = (tx->tx_flags >> 8) & 1;
        if (mounted && tx->capacity >= 0 && tx->capacity <= 65535) {
            snprintf(free_space, sizeof free_space, "%d", tx->capacity);
            /* Action 5 Mic 2 capability: rate bit1, 140 KiB/s.
             * caculate_tx_recordable_duration @0x2a918 overrides it with
             * 187 KiB/s when status bit14 is set. This is an estimate. */
            if (((tx->tx_flags >> 6) & 3) != 1)
                snprintf(recordable, sizeof recordable, "%d",
                    tx->capacity * 1024 / ((tx->tx_flags & (1u << 14)) ? 187 : 140));
        }
        if (tx_json_append(out, cap, &n, "},\"firmware_version\":\"%u.%u.%u.%u\","
            "\"storage\":{\"mounted\":%s,\"free_mib\":%s,\"format_state\":%u,\"estimated_recordable_seconds\":%s},\"format_result\":%d,\"format_request\":\"%llu\"}",
            tx->tx_version[3], tx->tx_version[2], tx->tx_version[1], tx->tx_version[0],
            mounted ? "true" : "false", free_space, (tx->tx_flags >> 6) & 3, recordable, tx->tx_format_result,
            (unsigned long long)tx->tx_format_request) < 0) goto truncated;
    } else if (tx_json_append(out, cap, &n, "},\"firmware_version\":null,\"storage\":null,\"format_result\":%d,\"format_request\":\"%llu\"}",
        tx->tx_format_result, (unsigned long long)tx->tx_format_request) < 0) goto truncated;
    return (int)n;
truncated:
    out[0] = 0;
    return -1;
}

static int tx_build_packet(uint8_t out[135], const uint8_t bd[6], int id, int value)
{
    memset(out, 0, 135);
    out[0] = 1;
    out[3] = 135;
    memcpy(out + 4, bd, 6);
    out[10] = 0x55;
    out[11] = 9;       /* 4-byte WL header + 19 00 00 id value */
    out[12] = 0x53;    /* camera=5, TX0=3 (single-TX engine) */
    out[14] = 0x19;
    out[17] = (uint8_t)id;
    /* Action 5 set_tx_rec_btn_en @0x28db4, auto_shutdown @0x29b04,
     * and denoise_btn @0x29d74..0x29d94
     * invert enable before transmission; telemetry is normalized likewise. */
    out[18] = (uint8_t)(id == 3 ? value + 12 : (id == 14 || id == 18 || id == 19) ? !value : value);
    out[13] = (uint8_t)((out[14] + out[17] + out[18]) & 0x3f);
    return 135;
}

/* Caller holds st_mtx. No status is changed merely because sendto succeeds. */
static int tx_send_locked(struct mic_tx *tx, int id, int value)
{
    uint8_t packet[135];
    tx_build_packet(packet, tx->cfg_bd, id, value);
    packet[12] = (uint8_t)(0x50 | tx_wire_destination(tx));
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    memcpy(addr.sun_path + 1, TX_DSOCK, sizeof TX_DSOCK - 1);
    ssize_t sent = fd < 0 ? -1 : sendto(fd, packet, sizeof packet, MSG_DONTWAIT,
        (struct sockaddr *)&addr, offsetof(struct sockaddr_un, sun_path) + sizeof TX_DSOCK);
    if (fd >= 0) close(fd);
    return sent == sizeof packet;
}

static void tx_format(const uint8_t bd[6], char *resp, int rsz)
{
    const char *result;
    uint64_t request = 0;
    pthread_mutex_lock(&E.rec_mtx);
    pthread_mutex_lock(&E.st_mtx);
    struct mic_tx *tx = tx_find_bd(bd);
    if (tx) tx_format_expire(tx);
    if (!tx) result = "FAIL target-changed";
    else if (!tx_ready(tx)) result = "FAIL tx-status-unavailable";
    else if (rec_active_locked() || tx->recording != 0) result = "FAIL recording-active";
    else if (tx->tx_format_result == 1 || ((tx->tx_flags >> 6) & 3)) result = "FAIL format-not-idle";
    else if (tx->tx_format_request == UINT64_MAX) result = "FAIL request-id-exhausted";
    else if (!tx_send_locked(tx, 12, 1)) result = "FAIL tx-send-failed";
    else {
        tx->tx_format_ms = now_ms();
        /* Identity must not repeat when more than one request fits in a tick. */
        request = tx->tx_format_ms > tx->tx_format_request ? tx->tx_format_ms : tx->tx_format_request + 1;
        tx->tx_format_request = request;
        tx->tx_format_result = 1;
        result = "OK queued";
    }
    pthread_mutex_unlock(&E.st_mtx);
    pthread_mutex_unlock(&E.rec_mtx);
    if (request) snprintf(resp, (size_t)rsz, "OK queued %llu", (unsigned long long)request);
    else snprintf(resp, (size_t)rsz, "%s", result);
}

static void tx_control_target(const uint8_t *target, const char *name, const char *arg, char *resp, int rsz)
{
    size_t i;
    char *end;
    long value;
    const struct tx_setting *setting = NULL;
    for (i = 0; i < TX_SETTING_COUNT; i++)
        if (!strcmp(name, tx_settings[i].name)) setting = &tx_settings[i];
    errno = 0;
    value = strtol(arg, &end, 10);
    if (!setting || !*arg || *end || errno || value < setting->min || value > setting->max
            || (setting->id == 15 && value != 0 && value != 2)) {
        snprintf(resp, (size_t)rsz, "FAIL invalid-setting");
        return;
    }
    pthread_mutex_lock(&E.st_mtx);
    struct mic_tx *tx = tx_find_bd(target);
    if (!tx) {
        pthread_mutex_unlock(&E.st_mtx);
        snprintf(resp, (size_t)rsz, "FAIL target-changed");
        return;
    }
    if (!tx_ready(tx)) {
        pthread_mutex_unlock(&E.st_mtx);
        snprintf(resp, (size_t)rsz, "FAIL tx-status-unavailable");
        return;
    }
    if (tx_format_active(tx)) {
        pthread_mutex_unlock(&E.st_mtx);
        snprintf(resp, (size_t)rsz, "FAIL formatting-active");
        return;
    }
    int sent = tx_send_locked(tx, setting->id, (int)value);
    pthread_mutex_unlock(&E.st_mtx);
    snprintf(resp, (size_t)rsz, sent ? "OK queued" : "FAIL tx-send-failed");
}

static void tx_control(const char *name, const char *arg, char *resp, int rsz)
{
    tx_control_target(NULL, name, arg, resp, rsz);
}
