/* Included by miccore.c. usage_mtx serializes leases and all recorder commands.
 * Tokens are caller-generated random capabilities; never include them in logs. */
static int usage_index(const char *token)
{
    if (!token || strlen(token) < 16 || strlen(token) > 64) return -1;
    for (const char *p = token; *p; p++) if (!isalnum((unsigned char)*p) && *p != '-') return -1;
    for (int i = 0; i < 16; i++) if (!strcmp(token, usage_tokens[i])) return i;
    return -1;
}

/* st_mtx held. A muted peer can occupy one side of a stereo take, provided
 * the other side is audible. A single muted peer retains the legacy skip. */
static int session_peer_ready(struct mic_tx *tx, uint64_t now)
{
    return tx && tx_ready(tx) && tx->last_audio_ms && now >= tx->last_audio_ms &&
        now - tx->last_audio_ms < 1000 && !tx->stream_lost && !tx->user_hold;
}

/* feed_mtx held. Check again after storage preparation: the target, hold,
 * mute or live status can change while the SSD/encoder is being opened. */
static int session_record_ready(const uint8_t bd[2][6], unsigned channels,
                                struct mic_tx **selected)
{
    pthread_mutex_lock(&E.st_mtx);
    struct mic_tx *tx = tx_find_bd(bd[0]);
    uint64_t now = now_ms();
    int ready = session_peer_ready(tx, now), order = 1;
    int audible = ready && !(tx->tx_flags & 1);
    if (channels == 2) {
        struct mic_tx *other = tx_find_bd(bd[1]);
        order = tx == &TX[0] && other == &TX[1];
        int other_ready = session_peer_ready(other, now);
        audible |= other_ready && !(other->tx_flags & 1);
        ready &= other_ready;
    }
    *selected = tx;
    pthread_mutex_unlock(&E.st_mtx);
    return !order ? -1 : ready && audible;
}

static void session_command(int an, char **av, char *resp, int rsz)
{
    snprintf(resp, rsz, "FAIL invalid-session-command");
    if (an < 2) return;
    const char *token = av[1];
    if (strlen(token) < 16 || strlen(token) > 64) return;
    for (const char *p = token; *p; p++) if (!isalnum((unsigned char)*p) && *p != '-') return;
    pthread_mutex_lock(&usage_mtx);
    if (atomic_load(&engine_stopping)) {
        snprintf(resp, rsz, "FAIL engine-stopping");
        pthread_mutex_unlock(&usage_mtx);
        return;
    }
    int idx = usage_index(token);
    if (!strcmp(av[0], "use") && an == 2) {
        if (idx < 0) for (int i = 0; i < 16; i++) if (!usage_tokens[i][0]) {
            idx = i; strcpy(usage_tokens[i], token); break;
        }
        snprintf(resp, rsz, idx < 0 ? "FAIL lease-capacity" : "OK acquired");
    } else if (!strcmp(av[0], "release") && an == 2) {
        pthread_mutex_lock(&E.rec_mtx);
        int active = rec_active_locked() && !strcmp(E.rec_owner, token);
        pthread_mutex_unlock(&E.rec_mtx);
        if (active) snprintf(resp, rsz, "FAIL owned-recording-active");
        else {
            if (idx >= 0) usage_tokens[idx][0] = 0;
            snprintf(resp, rsz, "OK released");
        }
    } else if (idx < 0) snprintf(resp, rsz, "FAIL lease-required");
    else if (!strcmp(av[0], "owner_status") && an == 2) {
        pthread_mutex_lock(&E.feed_mtx);
        pthread_mutex_lock(&E.rec_mtx);
        int mine = !strcmp(E.rec_owner, token);
        unsigned slot = E.rec_slot;
        pthread_mutex_unlock(&E.rec_mtx);
        if (!mine) snprintf(resp, rsz, "FAIL not-owner");
        else if (status_json_for_slot(slot, resp, rsz) < 0)
            snprintf(resp, rsz, "FAIL status-too-large");
        pthread_mutex_unlock(&E.feed_mtx);
    } else if (!strcmp(av[0], "rec_owned") || !strcmp(av[0], "rec_aac_owned") ||
               !strcmp(av[0], "rec_stereo_owned") || !strcmp(av[0], "rec_aac_stereo_owned")) {
        unsigned channels = strstr(av[0], "stereo") ? 2 : 1;
        int aac = !strncmp(av[0], "rec_aac_", 8);
        uint8_t bd[2][6];
        unsigned mode = 2;
        if (channels == 2 && an == 6) {
            if (strlen(av[5]) != 1 || av[5][0] < '1' || av[5][0] > '3') {
                snprintf(resp, rsz, "FAIL bad-channel-mode"); goto done;
            }
            mode = (unsigned)(av[5][0] - '0');
        } else if (an != (int)channels + 3) goto done;
        const char *path = av[channels + 2];
        if (path[0] != '/' || strlen(path) >= sizeof E.rec_path) goto done;
        for (unsigned slot = 0; slot < channels; slot++)
            if (strlen(av[slot + 2]) != 17 || parse_bd_arg(av[slot + 2], bd[slot])) goto done;
        if (channels == 2 && !memcmp(bd[0], bd[1], 6)) {
            snprintf(resp, rsz, "FAIL duplicate-target"); goto done;
        }
        pthread_mutex_lock(&E.rec_mtx);
        int same_path = !strcmp(E.rec_owner, token) && !strcmp(E.rec_path, path);
        int same_channels = rec_channels_locked() == channels;
        int same_target = channels == 2 ? !memcmp(E.rec_targets, bd, sizeof bd) : !memcmp(E.rec_target, bd[0], 6);
        int same_mode = rec_channel_mode_locked() == mode;
        int mine = same_path && same_target && same_channels && same_mode && (!!E.rec_aac == aac);
        int busy = rec_active_locked();
        pthread_mutex_unlock(&E.rec_mtx);
        if (mine) snprintf(resp, rsz, "OK %s", path); /* lost start reply, even after a disconnect */
        else if (same_path && !same_channels) snprintf(resp, rsz, "FAIL recording-channel-mismatch");
        else if (same_path && !same_target) snprintf(resp, rsz, "FAIL recording-target-mismatch");
        else if (same_path && !same_mode) snprintf(resp, rsz, "FAIL recording-mode-mismatch");
        else if (same_path) snprintf(resp, rsz, "FAIL recording-format-mismatch");
        else if (busy) snprintf(resp, rsz, "FAIL recording-busy");
        else {
            struct mic_tx *tx;
            struct rec_prepared prepared = {0};
            int started = 0;
            pthread_mutex_lock(&E.feed_mtx);
            int ready = session_record_ready(bd, channels, &tx);
            pthread_mutex_unlock(&E.feed_mtx);
            if (ready > 0 && rec_prepare(&prepared, path, aac, channels)) {
                snprintf(resp, rsz, "FAIL recording-busy-or-open");
                goto done;
            }
            pthread_mutex_lock(&E.feed_mtx);
            if (ready > 0) ready = session_record_ready(bd, channels, &tx);
            if (ready < 0) snprintf(resp, rsz, "FAIL stereo-target-order");
            else if (!ready) snprintf(resp, rsz, "OK skipped-unavailable-or-muted");
            else {
                if (rec_start_prepared(tx, 0, channels, mode, &prepared)) snprintf(resp, rsz, "FAIL recording-busy-or-open");
                else {
                    pthread_mutex_lock(&E.rec_mtx);
                    strcpy(E.rec_owner, token);
                    pthread_mutex_unlock(&E.rec_mtx);
                    snprintf(resp, rsz, "OK %s", path);
                    started = 1;
                }
            }
            pthread_mutex_unlock(&E.feed_mtx);
            if (started) mlog("rec_start: %s sec=0", path);
            rec_discard_prepared(&prepared);
        }
    } else if (!strcmp(av[0], "stop_owned") && an == 3) {
        pthread_mutex_lock(&E.rec_mtx);
        if (strcmp(E.rec_owner, token) || strcmp(E.rec_path, av[2])) snprintf(resp, rsz, "FAIL not-owner-or-path");
        else {
            uint64_t bytes;
            int rc = rec_stop_locked(NULL, 0, &bytes);
            if (!rc) snprintf(resp, rsz, "OK %s %llu", E.rec_path, (unsigned long long)bytes);
            else if (rc == 1) snprintf(resp, rsz, "OK stopping");
            else snprintf(resp, rsz, rc == -2 ? "FAIL recording-io-error" : "FAIL not-recording");
        }
        pthread_mutex_unlock(&E.rec_mtx);
    }
done:
    pthread_mutex_unlock(&usage_mtx);
}

/* Administrative micd termination is refused while any client or recorder
 * owns resources. Normal clients only release their lease, never stop micd. */
static int usage_busy_locked(void)
{
    int used = 0;
    for (int i = 0; i < 16; i++) if (usage_tokens[i][0]) used = 1;
    pthread_mutex_lock(&E.rec_mtx);
    if (rec_active_locked()) used = 1;
    pthread_mutex_unlock(&E.rec_mtx);
    return used;
}

int miccore_in_use(void)
{
    pthread_mutex_lock(&usage_mtx);
    int used = usage_busy_locked();
    pthread_mutex_unlock(&usage_mtx);
    return used;
}

int miccore_stop_if_idle(void)
{
    pthread_mutex_lock(&usage_mtx);
    int used = usage_busy_locked();
    if (!used) atomic_store(&engine_stopping, 1);
    pthread_mutex_unlock(&usage_mtx);
    if (used) return 1;
    return miccore_shutdown();
}
