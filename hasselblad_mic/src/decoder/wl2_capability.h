/* Action5 wl_capability_process_unlocked@0x3a720.
 * Read-only capability subset; audio/control negotiation is separate. */
struct wl2_capability {
    unsigned present;
    uint8_t version[4];
    uint16_t total_mib;
    uint8_t denoise_mask, led_mask, channel_mask;
    uint8_t encode_type_mask, frame_duration_mask, encode_format_mask;
    uint16_t encode_maxbytes;
    uint32_t encode_bitrate;
    uint8_t sample_rate_mask, sample_bits, sample_channel_mask;
    char model[51];
};

/* A malformed record never publishes a partially parsed snapshot. */
static int wl2_parse_capability(const uint8_t *p, size_t n, struct wl2_capability *out)
{
    struct wl2_capability next = {0};
    size_t off = 0;
    if (!p || !n || !out || n > 251) return -1;
    while (off < n) {
        unsigned tag = 0, byte;
        do {
            if (off >= n) return -1;
            byte = p[off++];
            tag += byte; /* 0xff extends the tag; it is not padding. */
        } while (byte == 255);
        if (off >= n) return -1;
        size_t len = p[off++];
        if (len > n - off) return -1;
        const uint8_t *v = p + off;
        switch (tag) {
        case 5:
            /* 0x3aca8 / 0x3b434: nine-byte encoding descriptor. */
            if (len != 9) return -1;
            next.encode_type_mask = v[0];
            next.frame_duration_mask = v[1];
            next.encode_format_mask = v[2];
            next.encode_maxbytes = (uint16_t)(v[3] | (v[4] << 8));
            next.encode_bitrate = (uint32_t)v[5] | ((uint32_t)v[6] << 8)
                | ((uint32_t)v[7] << 16) | ((uint32_t)v[8] << 24);
            break;
        case 6:
            /* 0x3ad3c / 0x3b550: sample frequency, bits, channels. */
            if (len != 3) return -1;
            next.sample_rate_mask = v[0];
            next.sample_bits = v[1];
            next.sample_channel_mask = v[2];
            break;
        case 1:
            if (len != 4) return -1;
            memcpy(next.version, v, 4);
            break;
        case 9:
            if (len != 2) return -1;
            next.total_mib = (uint16_t)(v[0] | (v[1] << 8));
            break;
        case 10: case 11: case 12:
            if (len != 1) return -1;
            if (tag == 10) next.denoise_mask = v[0];
            if (tag == 11) next.led_mask = v[0];
            if (tag == 12) next.channel_mask = v[0];
            break;
        case 13:
            if (!len || len > 50) return -1;
            memcpy(next.model, v, len);
            /* Reject control bytes rather than letting names alter JSON/UI. */
            for (size_t i = 0; i < len && next.model[i]; i++)
                if ((unsigned char)next.model[i] < 32) return -1;
            break;
        default:
            off += len;
            continue;
        }
        if (next.present & (1u << tag)) return -1;
        next.present |= 1u << tag;
        off += len;
    }
    if (!next.present) return -1;
    *out = next;
    return 0;
}

/* Initial WL2 reply: [length, sum(payload)&63, 0x20, seq, TLVs].
 * Accept exactly one complete message, never a truncated datagram. */
static int wl2_parse_capability_packet(const uint8_t *p, size_t n, struct wl2_capability *out)
{
    if (!p || n < 4 || n > 255 || p[0] != n || p[2] != 0x20) return -1;
    unsigned sum = 0;
    for (size_t i = 2; i < n; i++) sum += p[i];
    if ((sum & 63) != p[1]) return -1;
    return wl2_parse_capability(p + 4, n - 4, out);
}

/* Later capability reply: six clock bytes followed by a Wl message.
 * Action5 0x3c5cc..0x3c61c passes body+13, body[7]-7 to the TLV parser.
 * Return consumed bytes so following audio units remain available to the caller. */
static size_t wl2_parse_capability_envelope(const uint8_t *p, size_t n,
                                          struct wl2_capability *out)
{
    if (wl2_parse_capability_packet(p, n, out) == 0) return n;
    if (!p || n < 13 || p[6] != 0x55 || p[10] != 0x20) return 0;
    size_t len = p[7];
    if (len < 7 || len > n - 6) return 0;
    unsigned sum = 0;
    for (size_t i = 10; i < 6 + len; i++) sum += p[i];
    if ((sum & 63) != (p[9] & 63)) return 0;
    if (wl2_parse_capability(p + 13, len - 7, out)) return 0;
    return 6 + len;
}
