/*
 * miccore.c -- DJI Mic 2 feed-style engine.  See miccore.h for the contract.
 *
 * Reverse-engineering facts baked in (all verified against the LIVE device
 * binary /system/bin/bt_bsa_app, firmware 316, md5 de64e90af24d1b06697e87b0e12b8907):
 *   app_dg_open_ex2       base+0x25910   (int idx, bd_addr *bd)
 *   app_dg_close_ex2      base+0x257d8   (bd_addr *bd)  -- signature VERIFIED
 *                          2026-10-03 by full disassembly (BACKEND_NOTES §10):
 *                          scans 20-slot table @0xea750 (stride 0x20), only
 *                          touches entries with used(+0)==1 AND opened(+1)==1
 *                          AND bdcmp(entry+3, bd)==0, then BSA_DgClose with
 *                          handle@+0xa.  Entries allocated but never opened
 *                          (open evt pending / TX asleep) are IGNORED by it;
 *                          only native callbacks may retire these slots.
 *   app_audio_play_data   base+0x23fd0   (conn, buf, len)  <- hook point, buf = Wl stream
 *   app_audio_ccode_sync  base+0x237d0   (bd_addr *bd, int ccode)
 *   app_dg_send_ex2       base+0x25068   (bd_addr *bd, void *frame, int len)
 *   DG connection table   base+0xea750   20 x 0x20:
 *                          +0 used, +1 opened, +2 is_server, +3 bd[6],
 *                          +0xa handle(u16), +0xc port, +0x10 GKI buf ptr.
 *                          Stock free path = GKI_freebuf(+0x10) + zero 32B.
 *   GKI_freebuf           base+0x31a5c   (void *buf)
 *
 * Wl block (SPP stream, from micsock.c lineage):
 *   55 77 05 | cksum@3 | cmd@4 | r@5 | seq@6 | payload@7..len-2    len = b[1] (=119)
 *   cksum: (sum b[4..len-2]) & 0x3f == b[3] & 0x3f
 *   cmd 0x10: 111B LC3plus frame @7  (48k mono 5ms -> 240 s16)
 *   cmd 0x19: status TLV stream @7: skip 0xff filler; [type][len][value..];
 *             battery = type 0x10, charging = 0x05, mute = 0x01,
 *             recording = 0x06 (libwirelessmicservice.so @0x39a20)
 *
 * v3 unified SPP framing (ground truth: artifacts/feeddump_live.bin,
 * 2026-10-03; spec REPORT_audio_start 1.2 + REPORT_ctrl_frames):
 *   AA <l16be: ver(2b)|count(4b)|len(10b)> <payload> A5
 *     ver=0: payload = count x 125B audio units (6B hdr {btclk u32le,
 *            intra u16le} + 119B Wl) + optional len-byte tail = a
 *            TX->camera DUSS control frame
 *   tail/DUSS v1: 55 | len16le (len|0x400) | crc8(0x77 over first 3) |
 *            sender(0x3a=TX) | receiver | seq16le | flags8 | set | cmd |
 *            payload | crc16le(0x3692).  Camera replies (product style):
 *            sender=0x07, receiver=0x18, flags 0x40, set 0x12.
 *   Passive responders: 0x15->0x56 echo, 0x17->0x55 {clamp(dBm+100),bd},
 *            0x57->16+6n answer, 0x58/02->0x56; plus the RSSI-event leg
 *            (each 1 Hz 0x582 poll -> one 0x55, product chain per
 *            REPORT_ctrl_frames 3; disable: keepalive_055=0 in mic.cfg).
 *   Shutdown: 0x54 {00,0x20,bd} sleep notice -> 200 ms grace -> close
 *            (feature_bt_audio_stop@0x489c40). Ordinary disconnect only closes.
 *
 * DUSS v1 frames for the kickstart ladder (REPORT_dji_spp_protocol.md section 3):
 *   55 | len_lo | 0x04|len_hi | crc8(0x77) | sender 0x07 | receiver 0x3A | seq16le
 *   | 0x40 | 0x12 | cmd_id | payload | crc16le(0x3692);  wrapped in AA <be16> .. A5.
 *
 * WL2 (wl 2.0) capability exchange, camera side (Action5
 * libwirelessmicservice, wl_capability_sync / wl_msg_set_core):
 *   downlink WL-compact frame (AA ver=0 wrapped like DUSS):
 *     55 | len(=payload+4) | (type<<4)|dst | (ver<<6)|(sum(payload)&0x3f)
 *     | payload;  type 5 capability/settings, dst 3 = TX0.
 *   capability sync  ~2s after the first audio, retry every 20
 *     non-capability uplink frames (max 3), payload = 20 00 00 02 02 +
 *     rx tags + host_name "X2D II" + rx_cap identity block.
 *   TX 0x20 capability reply (forms: [n][csum6][20][seq][TLVs] or
 *     [6B clock][55][len][x][csum][20]...) -> full settings download:
 *     11 x payload 19 00 00 <tag> <value>, one AA frame each, >=1 ms
 *     apart (gain +12dB offset, rec-btn/auto-shutdown enable inverted).
 *   after that the TX reports real status as 0x19 records
 *     [n][csum6][19][x][TLVs]: battery=T0x10, charging=T0x05, mute=T0x01,
 *     recording=T0x06, gain=T0x11 (value-12).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <ctype.h>
#include <math.h>
#include "mic_aac.h"
#include "mic_stereo.h"
#include "mic_stereo_clock.h"
#include "mic_preroll.h"
#include <time.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <dlfcn.h>
#include <stddef.h>
#include <stdatomic.h>
#include <poll.h>

#include "miccore.h"
#include "wl2_capability.h"
#include "mic_timing.h"
#include "mic_dg_events.h"

/* ------------------------------------------------------------------ */
/* paths & limits                                                      */
/* ------------------------------------------------------------------ */
#ifndef MIC_DIR
#define MIC_DIR    "/data/mic"
#endif
#define FIFO_PATH  MIC_DIR "/mic.pcm"
#define STEREO_FIFO_PATH MIC_DIR "/mic.stereo.pcm"
#define STATUS_TMP MIC_DIR "/.mic_status.json.tmp"
#define STATUS_PATH MIC_DIR "/mic_status.json"
#define CTL_PATH   MIC_DIR "/mic_ctl"
#ifndef CFG_PATH
#define CFG_PATH   MIC_DIR "/mic.cfg"
#endif
#ifndef LOG_PATH
#define LOG_PATH   "/blackbox/hbl-logs/mic/mic.log"
#endif
#ifndef LOG_LIMIT
#define LOG_LIMIT (4 * 1024 * 1024)
#endif
#define LC3_SO     MIC_DIR "/liblc3plus_b.so"
#ifndef MIC_LC3_ETSI_PATH
#define MIC_LC3_ETSI_PATH "/data/mic/libLC3plus_bionic.so"
#endif
#define BT_SOCK    "/data/misc/bluetooth/bt-daemon-socket"
#ifndef DEV_XML
#define DEV_XML    "/data/misc/bluetooth/bt_devices.xml"
#endif

#define MAXPL      2100          /* bsa frame payload cap */
#define SRATE      48000
#define FRAME_MS   5
#define NSAMP      240           /* 48k * 5ms */

/* Production and Frida ARM64 builds discover the same audited ABI family.
 * Host fixtures retain constant offsets for their fake DG memory tables. */
#include "mic_admission.h"
#if defined(MICCORE_NATIVE_HOOK) || defined(__aarch64__)
#include "../native/bt_dual_timeout.h"
#include "../native/bt_dual_clock.h"
#include "../native/bt_dg_lifecycle.h"
#include "../native/bt_admission.h"
#define MICCORE_DISCOVERED_ABI 1
#ifdef MICCORE_NATIVE_HOOK
extern const struct mic_bt_abi *mic_native_abi(void);
extern void *mic_native_base(void);
#else
static struct mic_bt_abi development_abi;
static const struct mic_bt_abi *mic_native_abi(void) { return &development_abi; }
#endif
#define OFF_APP_INIT (mic_native_abi()->offset[MIC_ABI_APP_INIT])
#define OFF_HFP_SET_EN (mic_native_abi()->offset[MIC_ABI_HFP_SET_EN])
#define OFF_DM_VIS (mic_native_abi()->offset[MIC_ABI_DM_VIS])
#define OFF_AG_OPEN (mic_native_abi()->offset[MIC_ABI_AG_OPEN])
#define OFF_PLAY_DATA (mic_native_abi()->offset[MIC_ABI_PLAY_DATA])
#define OFF_DG_OPEN (mic_native_abi()->offset[MIC_ABI_DG_OPEN])
#define OFF_DG_LISTEN (mic_native_abi()->offset[MIC_ABI_DG_LISTEN])
#define OFF_DG_CLOSE (mic_native_abi()->offset[MIC_ABI_DG_CLOSE])
#define OFF_CCODE (mic_native_abi()->offset[MIC_ABI_CCODE])
#define OFF_DG_SEND (mic_native_abi()->offset[MIC_ABI_DG_SEND])
#define OFF_GKI_FREE (mic_native_abi()->offset[MIC_ABI_GKI_FREE])
#define OFF_INITED (mic_native_abi()->inited)
#define OFF_DG_TABLE (mic_native_abi()->dg_table)
#else
/* Legacy bt_bsa_app offsets from module base (PIE), firmware 316. */
#define OFF_APP_INIT  0x2b168      /* app_init() == WMS dsock cmd 0x00 action */
#define OFF_HFP_SET_EN 0x2b100     /* bt_hfp_set_enable(u8): keep AG armed for TX dance */
#define OFF_DM_VIS    0xda30       /* app_dm_set_visibility(disc, conn) */
#define OFF_AG_OPEN   0x17008      /* app_ag_open(bd): engage TX's HF -> native DJI branch */
#define OFF_PLAY_DATA 0x23fd0      /* app_audio_play_data(bd, buf, len) */
#define OFF_DG_OPEN   0x25910
#define OFF_DG_LISTEN 0x252c8      /* int app_dg_listen(void), firmware 316 */
#define OFF_DG_CLOSE  0x257d8
#define OFF_CCODE     0x237d0
#define OFF_DG_SEND   0x25068
#define OFF_CLI_CB    0x7f9f0      /* -> BSA client ctrl block (diagnostics) */
#define OFF_INITED    0x825a6      /* u8: stack-inited flag set by WMS cmd 0x00 handler */
#define OFF_DG_TABLE  0xea750      /* 20 x 0x20 app_dg connection table */
#define OFF_GKI_FREE  0x31a5c      /* GKI_freebuf(void *buf) */
#endif
#define DG_CONS       20
#define DG_ESZ        0x20

/* ------------------------------------------------------------------ */
/* engine state                                                        */
/* ------------------------------------------------------------------ */
enum { ST_IDLE = 0, ST_CONNECTING, ST_STREAMING, ST_RECONNECTING };
static const char *ST_NAME[] = { "idle", "connecting", "streaming", "reconnecting" };
/* status_write() uses "offline" instead of ST_NAME when device_offline */

/* Transport, parser, telemetry and retry state belong to a fixed TX slot.
 * E owns process-wide threads, sinks and locks; no mutex lives in a movable
 * device record. Mapping changes are serialized with control/feed operations. */
struct mic_tx {
    uint8_t cfg_bd[6];
    int cfg_bd_valid;
    int state;
    int connected;               /* stream data seen recently */
    int stream_lost;             /* availability, independent of background retry phase */
    int paired;                  /* v2: XML has a key entry for cfg_bd */
    char name[64];
    int battery;                 /* percent, -1 unknown (Mic 2 never reports one) */
    int battery_level;           /* Mic 2 WL1 status byte 6: 1..7 level, -1 unknown.
                                  * Action 5 GUI CameraCtrlTXPairComponent_GetBattery
                                  * (dji_gui_on_disp1 0x618680) indexes a 7-entry
                                  * table with level-1; onWlMic1BatteryChg toasts a
                                  * low-battery warning at 6/7 -> smaller = fuller.
                                  * Live 10-04: fully charged TX reports 2. */
    int charging;                /* v3 TLV T0x05: -1 unknown, 0/1 */
    int mute;                    /* v3 TLV T0x01: -1 unknown, 0/1 */
    int recording;               /* v3 TLV T0x06: -1 unknown, 0/1 */
    int rssi;                    /* dBm, 0 = never polled */
    uint64_t last_poll_ms;       /* last successful get_status send */
    uint64_t last_audio_ms;      /* CLOCK_MONOTONIC-ish ms */
    uint64_t retry_gate_ms;     /* earliest next auto-reconnect */
    uint64_t last_pad_ms;        /* last stall-pad emitted at */
    int retries;
    int prev_state;
    uint64_t idle_since_ms, idle_try_ms;
    int kick_stage;              /* 0 none, 1 ccode, 2 frames, 3 HFP retry, 4 post-bond SPP */
    int kick_tries;              /* v3: kickstart send retries (DG open race) */
    uint64_t kick_ms;
    uint64_t kick_audio_first_ns, kick_audio_last_ns;
    uint32_t ts_btclk;           /* u32, 0 until first time-sync */
    uint16_t ts_intra;           /* u16 */
    int ts_valid;
    uint64_t ts_58_01_ms;        /* epoch ms when TX's 0x58/01 arrived */
    int ts_58_01_valid;
    int bond_fails;              /* consecutive auto-pair AUTH failures */
    int connect_fails;           /* consecutive connect-incomplete timeouts */
    volatile int device_offline; /* >=3 fails or user disconnect: stop retry */
    volatile int user_hold;      /* ctl disconnect/unpair: suppress every
                                  * automatic (re)connect / auto-pair and
                                  * ignore TX-initiated stream until the
                                  * user issues `connect` or `pair` */
    int dg_close_pending;        /* control_mtx: old transport must retire before reconnect */
    int dg_seen_valid, dg_seen_state; /* control_mtx: table edge fallback */
    int dial_pending;            /* st_mtx: released by native OPEN/CLOSE/failure */
    uint64_t dg_event_after;      /* control_mtx: ignore events queued before a new explicit attempt */
    volatile uint64_t frames;
    volatile uint64_t drops;
    volatile double level;
    uint8_t sbuf[1 << 20];
    int slen;
    int have_seq;
    uint8_t last_seq;
    int seq_clock_valid;
    uint32_t seq_btclk;
    uint16_t seq_intra;
    uint64_t seq_receive_ns;
    int autocmd_pending;           /* 1: boot auto-connect; 2: accepted dial deferred to hk */
    uint64_t autocmd_at;           /* earliest time for deferred connect */
    uint64_t connect_ms;           /* when CONNECTING was entered */
    uint64_t dial_until_ms;        /* control_mtx/st_mtx: this peer's native dial window */
    uint8_t sc[128];
    int sc_len;
    int wl_protocol;             /* 0 unknown, 1 WL1, 2 WL2; guarded by st_mtx */
    struct wl2_capability wl2_cap;
    int cap_sync_sent;           /* initial sync queued */
    int cap_sync_done;           /* 0x20 seen + settings downloaded */
    int cap_sync_resends;        /* 20-frame resends used, cap 3 */
    int nocap_frames;            /* non-capability uplink frames while waiting */
    uint64_t first_audio_ms;     /* cap-sync anchor: first WL2 audio or ladder-out */
    int mic_gain;                /* dB, -128 unknown */
    unsigned tx_flags, tx_extra;
    uint8_t tx_version[4]; /* WL1 payload[11..14], least significant first */
    uint64_t tx_format_ms;
    uint64_t tx_format_request;
    int tx_format_result; /* 0 none, 1 pending, 2 success, 3 failure, 4 unknown/timeout */
    uint64_t tx_status_ms;
    int capacity;                /* TX storage field (raw u16), -1 unknown */
};
static struct mic_tx TX[2];

static int tx_wire_destination(const struct mic_tx *tx)
{
    return tx == &TX[1] ? 9 : 3;
}

/* st_mtx held (or startup before feeds). A legacy unaddressed producer is
 * usable with either single slot, but ambiguous input must never be guessed. */
static struct mic_tx *tx_find_bd(const uint8_t *bd)
{
    if (!bd) {
        if (TX[0].cfg_bd_valid && TX[1].cfg_bd_valid) return NULL;
        return TX[1].cfg_bd_valid ? &TX[1] : &TX[0];
    }
    struct mic_tx *found = NULL;
    for (unsigned i = 0; i < 2; i++) {
        if (!TX[i].cfg_bd_valid || memcmp(bd, TX[i].cfg_bd, 6)) continue;
        if (found) return NULL; /* invalid duplicate configuration */
        found = &TX[i];
    }
    return found;
}

static struct {
    int started;
    int offline;                 /* MICCORE_OFFLINE=1: no ctl socket, no in-proc calls */
    int auto_connect;
    pthread_mutex_t st_mtx;
    int ka_055;                  /* cfg keepalive_055: RSSI-event->0x55 chain */
    int passive;                 /* v3.1: PASSIVE connect mode -- no engine
                                  * DgOpen (it races the factory branch's
                                  * own open during the TX HFP dance); the
                                  * engine observes + answers instead */
    volatile int visibility_set; /* one-shot: restore disc+conn after boot */
    int pcm_fd;                  /* fifo writer, -1 when closed */
    pthread_mutex_t rec_mtx;
    FILE *rec_f;
    mic_aac *rec_aac;
    char rec_owner[65];          /* rec_mtx: empty = legacy GUI recorder */
    uint8_t rec_target[6];
    uint8_t rec_targets[2][6];
    unsigned rec_slot;
    unsigned rec_channels;
    unsigned channel_mode;       /* rec_mtx: preference for the next GUI take */
    unsigned rec_channel_mode;   /* immutable: 1=mixed dual mono, 2=L/R, 3=R/L */
    uint64_t rec_start_ms, rec_real_samples, rec_silent_samples;
    uint64_t rec_start_ns, rec_timing_generation, rec_timing_first, rec_timing_last, rec_timing_end_generation;
    uint64_t rec_timing_generations[2], rec_timing_end_generations[2];
    uint64_t rec_history_after;
    int rec_timing_closed;
    char rec_path[256];
    uint64_t rec_bytes;
    uint64_t rec_deadline_ms;    /* 0 = no deadline */
    int rec_completed;
    int rec_finalizing;          /* rec_mtx: WAV detached, close result pending */
    int rec_error;
    pthread_mutex_t feed_mtx;
    pthread_t hk_thread, ctl_thread, bsa_thread, dsock_thread, relay_thread;
    pthread_t pcm_thread;
    int pcm_running;
    int hk_running, dsock_running, relay_running;
    int bsa_thread_running;
    int need_app_init;             /* bootstrap app_init() from hk thread */
    int app_init_done;
    int standalone;
    int native_clock_requests;   /* startup-only: audited native 0x58/01 responder */
    volatile uint64_t dsock_pkts;
    uint8_t relay_seq;              /* local dgram seq8, wraps */
    volatile uint64_t relay_pkts;
    int manage_bt_audio_cfg;     /* standalone: keep bt_audio.cfg = paired bd */
} E;
/* rec_mtx owns both the stereo queues and their relative clock placement. */
static struct mic_stereo record_stereo;
static pthread_cond_t rec_finalized = PTHREAD_COND_INITIALIZER;
/* Mono positions are source sample counts, not calibrated ADC timestamps.
 * Written silence commits those positions; late PCM must not append them again. */
static struct {
    uint64_t generation, source_base, output_base, fallback_next;
    uint64_t late_samples, reanchors;
    int fallback_online, have_origin, draining, tail_timed_out;
    uint64_t stop_target, stop_until_ns, tail_received;
} record_mono;
/* Frozen stereo endpoint and source generations while late transport callbacks
 * complete a stopped take. All fields are owned by rec_mtx. */
static struct {
    uint64_t target, until_ns, generations[2], received_frames[2];
    unsigned wait_mask;
    int draining, timed_out;
} record_tail;
static pthread_once_t rec_tail_once = PTHREAD_ONCE_INIT;
static pthread_cond_t rec_tail_changed;
static int rec_tail_cond_rc;
static void rec_tail_init(void)
{
    pthread_condattr_t attr;
    rec_tail_cond_rc = pthread_condattr_init(&attr);
    if (rec_tail_cond_rc) return;
    rec_tail_cond_rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (!rec_tail_cond_rc) rec_tail_cond_rc = pthread_cond_init(&rec_tail_changed, &attr);
    pthread_condattr_destroy(&attr);
}
static struct mic_stereo_clock record_clock;
static struct mic_preroll recent_pcm[2], record_preroll[2];
static uint64_t record_preroll_frames[2], record_preroll_first[2];
/* File-only deadline for committing silence when no queued PCM can advance
 * the recording. Two pairing windows absorb a transport slowdown without
 * retimestamping late PCM. Pair matching and the live FIFO retain 300 ms. */
#define MIC_REC_SILENCE_GRACE_MS 600u
#define MIC_REC_MONO_GRACE_MS MIC_STEREO_WAIT_MS
static atomic_int discovery_cancel;
static atomic_int discovery_operation;
static pthread_mutex_t discovery_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t control_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t usage_mtx = PTHREAD_MUTEX_INITIALIZER;
static char usage_tokens[16][65]; /* process-scoped leases, no timer may steal a recording */
static int engine_lock_fd = -1;
static int pcm_concealed; /* feed_mtx: 0 decoded, 1 decoder PLC, 2 zero fallback */
static atomic_int engine_stopping;
static atomic_int feed_enabled;
static pthread_mutex_t lifecycle_mtx = PTHREAD_MUTEX_INITIALIZER;

static void engine_wait_ms(unsigned ms)
{
    while (ms && !atomic_load(&engine_stopping)) {
        unsigned step = ms < 100 ? ms : 100;
        usleep(step * 1000);
        ms -= step;
    }
}

/* ------------------------------------------------------------------ */
/* logging                                                             */
/* ------------------------------------------------------------------ */
static pthread_mutex_t log_mtx = PTHREAD_MUTEX_INITIALIZER;
static int log_fd = -1;

static void mlog(const char *fmt, ...)
{
    va_list ap;
    char b[512];
    struct timespec now;
    struct tm tmv;
    int n;
    time_t t;

    clock_gettime(CLOCK_REALTIME, &now);
    t = now.tv_sec;
    localtime_r(&t, &tmv);
    n = snprintf(b, sizeof b, "[%02d:%02d:%02d.%03ld] ",
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec, now.tv_nsec / 1000000);
    va_start(ap, fmt);
    n += vsnprintf(b + n, sizeof b - n - 1, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof b - 2) n = sizeof b - 2;
    b[n++] = '\n';
    pthread_mutex_lock(&log_mtx);
    struct stat st;
    if (log_fd >= 0 && !fstat(log_fd, &st) && S_ISREG(st.st_mode) && !st.st_nlink) {
        close(log_fd);
        log_fd = -1;
    }
    if (log_fd < 0) {
        /* The stock blackbox space manager preserves the hbl-logs root. */
        mkdir("/blackbox/hbl-logs", 0755);
        mkdir("/blackbox/hbl-logs/mic", 0755);
        log_fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    }
    if (log_fd >= 0 && !fstat(log_fd, &st) && S_ISREG(st.st_mode) && st.st_size >= LOG_LIMIT) {
        /* Keep at most three generations; a failed rotation must not truncate. */
        rename(LOG_PATH ".1", LOG_PATH ".2");
        if (!rename(LOG_PATH, LOG_PATH ".1")) {
            close(log_fd);
            log_fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        }
    }
    if (log_fd >= 0 && write(log_fd, b, n) < 0) { /* logging never breaks audio */ }
    pthread_mutex_unlock(&log_mtx);
}

/* ------------------------------------------------------------------ */
/* time helper                                                         */
/* ------------------------------------------------------------------ */
static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint64_t now_epoch_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ------------------------------------------------------------------ */
/* LC3plus decoder, dual backend                                       */
/*   primary: open-source ETSI build /data/mic/libLC3plus_bionic.so    */
/*     lc3plus_dec_init(dec, 48000, 1, PLC_ADVANCED=1, hrmode=0)       */
/*     lc3plus_dec_set_frame_dms(dec, 4)  -- dms enum: 5MS == 4        */
/*     lc3plus_dec16(dec, frame, plen, int16_t **out, NULL, 0)         */
/*     PLC concealment: lc3plus_dec16(dec, NULL, 0, &out, NULL, 0)     */
/*   fallback: surgical /data/mic/liblc3plus_b.so (lc3_* API, no PLC)  */
/* ------------------------------------------------------------------ */
/* stubs the surgical .so needs at dlopen time (micsock.c lineage).
 * Bionic has no __assert_fail (glibc name); the patched liblc3plus_b.so
 * references it. We must NOT abort inside bt_bsa_app on a decoder assert:
 * log loudly and return (decode params are fixed/validated by us). */
void __assert_fail(const char *expr, const char *file, unsigned line, const char *fn)
{
    mlog("liblc3plus ASSERT: %s:%u %s: %s", file, line, fn ? fn : "", expr);
    if (getenv("MICCORE_ABORT_ON_ASSERT")) abort();
}
void __gmon_start__(void) {}
void _ITM_deregisterTMCloneTable(void) {}
void _ITM_registerTMCloneTable(void) {}

#include "mic_decoder.h"
static struct mic_decoder decoders[2] = { {.samples = NSAMP}, {.samples = NSAMP} };

static void engine_decoders_close(void)
{
    for (unsigned i = 0; i < 2; i++) decoder_close(&decoders[i]);
}

static int engine_decoder_init(unsigned slot)
{
    if (slot >= 2) return -1;
    char library[512] = MIC_LC3_ETSI_PATH;
#ifdef MICCORE_NATIVE_HOOK
    /* Keep the decoder from the same immutable release as the loaded engine,
     * even when an installer stages a newer release during a recording. */
    Dl_info component;
    if (dladdr((void *)miccore_start, &component) && component.dli_fname &&
        strlen(component.dli_fname) < sizeof library - 32) {
        snprintf(library, sizeof library, "%s", component.dli_fname);
        char *slash = strrchr(library, '/');
        if (slash) strcpy(slash + 1, "libLC3plus_bionic.so");
    }
#endif
    return decoder_init(&decoders[slot], library, LC3_SO);
}

/* ------------------------------------------------------------------ */
/* bt_bsa_app in-process calls                                         */
/* ------------------------------------------------------------------ */
static void *bb_base;            /* bt_bsa_app load base */
static int  (*p_app_init)(void);
static int  (*p_dg_open)(int, void *);
static void (*p_hfp_set_enable)(unsigned char);
static void (*p_dm_set_vis)(int, int);
static int  (*p_ag_open)(void *);
static int  (*p_dg_listen)(void);
static int  (*p_dg_close)(void *);
static int  (*p_ccode)(void *, int);
static int  (*p_dg_send)(void *, void *, int);
static void (*p_gki_free)(void *);

/* stack inited? (flag set by WMS cmd 0x00 handler after app_init) */
static int bb_stack_inited(void)
{
    if (!bb_base) return 0;
    return *(uint8_t *)((char *)bb_base + OFF_INITED) == 1;
}

static void *bb_resolve(void)
{
#ifdef MICCORE_NATIVE_HOOK
    return mic_native_base();
#else
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    unsigned long best = 0;

    if (!f) return NULL;
    /* the load base of a PIE executable is the LOWEST mapping of its
     * path (first line: r--p offset 00000000).  Do not key on perms --
     * relro segments are also r--p and live +0x7b000 in. */
    while (fgets(line, sizeof line, f)) {
        unsigned long lo;
        char perms[12], off[24];
        if (strstr(line, "bt_bsa_app") &&
            sscanf(line, "%lx %11s %23s", &lo, perms, off) == 3) {
            if (!strcmp(off, "00000000") && strstr(perms, "r"))
                { best = lo; break; }        /* exact: header mapping */
            if (best == 0 || lo < best) best = lo;
        }
    }
    fclose(f);
    if (!best) return NULL;
    return (void *)best;
#endif
}

static int bb_init(void)
{
    E.native_clock_requests = 0;
    if (E.offline) return 0;
    bb_base = bb_resolve();
    if (!bb_base) {
        mlog("bb_init: bt_bsa_app base not found (not inside it?)");
        return 0;
    }
#ifdef MICCORE_DISCOVERED_ABI
#ifndef MICCORE_NATIVE_HOOK
    if (mic_abi_load("/proc/self/exe", &development_abi)) {
        mlog("bb_init: ABI discovery failed; refuse in-process startup"); return -1;
    }
#endif
    int admission = mic_admission_install(bb_base, mic_native_abi());
    if (admission < 0) {
        mlog("bb_init: native audio admission hook failed (%d)", admission); return -1;
    }
    mic_admission_enable(1);
    mlog("bb_init: native audio admission supplemented%s", admission ? " (text protection warning)" : "");
    int dg = mic_dg_install(bb_base, mic_native_abi());
    if (dg < 0) {
        mlog("bb_init: native DG lifecycle isolation failed (%d)", dg); return -1;
    }
    mlog("bb_init: native DG lifecycle isolated%s", dg ? " (text protection warning)" : "");
    int isolated = mic_timeout_patch(bb_base, mic_native_abi());
    if (isolated < 0) {
        mlog("bb_init: per-TX native timeout isolation failed (%d)", isolated); return -1;
    }
    mlog("bb_init: per-TX native timeout isolated%s", isolated ? " (text protection warning)" : "");
    int clocks = mic_clock_install(bb_base, mic_native_abi());
    if (clocks < 0) {
        mlog("bb_init: per-TX native clock isolation failed (%d)", clocks); return -1;
    }
    mlog("bb_init: per-TX native clock isolated%s", clocks ? " (text protection warning)" : "");
    E.native_clock_requests = 1;
#endif
    p_app_init = (int (*)(void))((char *)bb_base + OFF_APP_INIT);
    p_hfp_set_enable = (void (*)(unsigned char))((char *)bb_base + OFF_HFP_SET_EN);
    p_dm_set_vis = (void (*)(int, int))((char *)bb_base + OFF_DM_VIS);
    p_ag_open = (int (*)(void *))((char *)bb_base + OFF_AG_OPEN);
    p_dg_open = (int (*)(int, void *))((char *)bb_base + OFF_DG_OPEN);
    p_dg_listen = (int (*)(void))((char *)bb_base + OFF_DG_LISTEN);
    p_dg_close = (int (*)(void *))((char *)bb_base + OFF_DG_CLOSE);
    p_ccode = (int (*)(void *, int))((char *)bb_base + OFF_CCODE);
    p_dg_send = (int (*)(void *, void *, int))((char *)bb_base + OFF_DG_SEND);
    p_gki_free = (void (*)(void *))((char *)bb_base + OFF_GKI_FREE);
    mlog("bb_init: bt_bsa_app base=%p app_init=%p open=%p close=%p ccode=%p send=%p gki_free=%p client_open=%d",
         bb_base, (void *)p_app_init, (void *)p_dg_open, (void *)p_dg_close,
         (void *)p_ccode, (void *)p_dg_send, (void *)p_gki_free, bb_stack_inited());
    return 0;
}

void *miccore_native_play_data(void)
{
    return bb_base && E.started ? (char *)bb_base + OFF_PLAY_DATA : NULL;
}

/* ------------------------------------------------------------------ */
/* bt_bsa_app DG connection table (base+0xea750, 20 x 0x20)            */
/*   +0 used  +1 opened  +2 is_server  +3 bd[6]  +0xa handle(u16)      */
/*   +0xc port  +0x10 GKI buf ptr                                       */
/* ------------------------------------------------------------------ */
static uint8_t *dg_entry(int i)
{
    return (uint8_t *)((char *)bb_base + OFF_DG_TABLE) + (size_t)i * DG_ESZ;
}

/* log every allocated table slot (diagnostics for close/leak forensics) */
static void dg_table_log(const char *why)
{
    int i;
    if (!bb_base) return;
    for (i = 0; i < DG_CONS; i++) {
        uint8_t *e = dg_entry(i);
        if (!e[0]) continue;
        mlog("dg_table[%s] slot%d used=%d opened=%d srv=%d bd=%02x:%02x:%02x:%02x:%02x:%02x handle=0x%02x%02x",
             why, i, e[0], e[1], e[2],
             e[3], e[4], e[5], e[6], e[7], e[8], e[9], e[11], e[10]);
    }
}

/* used + !opened is also the native OPEN_EVT matching state (0x24660).
 * Never erase such slots: late callbacks still own them. Idle server
 * listeners are not a connection; opened servers are. */
static int dg_target_state(const uint8_t *bd)
{
    int state = 0;
    if (!bb_base || !bd) return 0;
    for (int i = 0; i < DG_CONS; i++) {
        volatile uint8_t *e = dg_entry(i);
        if (!e[0] || (e[2] && !e[1]) || memcmp((const void *)(e + 3), bd, 6)) continue;
        state |= e[1] ? 2 : 1;
    }
    return state;
}

static int dg_open_once(const uint8_t *bd)
{
    int state = dg_target_state(bd);
    if (state) {
        mlog("connect: SPP %s; reuse native slot, no duplicate open",
             state & 2 ? "already open" : "open pending");
        return 0;
    }
    int rc = p_dg_open ? p_dg_open(0, (void *)bd) : -1;
    if (rc != 0) dg_table_log("open-rejected");
    return rc;
}

/* DgClose only submits a request and closes the FIRST matching slot.
 * Await its callback before submitting another close or allowing reopen. */
static int dg_close_and_wait(const uint8_t *bd)
{
    uint64_t deadline = now_ms() + 2500;
    int submitted_slot = -1;
    unsigned submitted_handle = 0;
    while (!atomic_load(&engine_stopping)) {
        uint64_t cursor = mic_dg_event_cursor();
        if (!dg_target_state(bd)) return 0;
        for (int i = 0; i < DG_CONS; i++) {
            volatile uint8_t *e = dg_entry(i);
            if (!e[0] || !e[1] || memcmp((const void *)(e + 3), bd, 6)) continue;
            unsigned handle = e[10] | ((unsigned)e[11] << 8);
            if (i != submitted_slot || handle != submitted_handle) {
                int rc = p_dg_close ? p_dg_close((void *)bd) : -1;
                mlog("disconnect: close request slot%d handle=%u rc=%d", i, handle, rc);
                if (rc != 0) return -1;
                submitted_slot = i; submitted_handle = handle;
            }
            break;
        }
        uint64_t now = now_ms();
        if (now >= deadline) break;
        if (mic_dg_events_enabled()) mic_dg_event_wait(cursor, (unsigned)(deadline - now));
        else engine_wait_ms(25);
    }
    dg_table_log("close-pending");
    return -1;
}

/* ------------------------------------------------------------------ */
/* DUSS frame builder (tables from dji_crc_tables.json, verified vs    */
/* live sample 55 0E 04 66 07 3A 01 00 40 12 58 02 03 EE A5)           */
/* ------------------------------------------------------------------ */
static const uint8_t duss_t8[256] = {
    0x00,0x5e,0xbc,0xe2,0x61,0x3f,0xdd,0x83,
    0xc2,0x9c,0x7e,0x20,0xa3,0xfd,0x1f,0x41,
    0x9d,0xc3,0x21,0x7f,0xfc,0xa2,0x40,0x1e,
    0x5f,0x01,0xe3,0xbd,0x3e,0x60,0x82,0xdc,
    0x23,0x7d,0x9f,0xc1,0x42,0x1c,0xfe,0xa0,
    0xe1,0xbf,0x5d,0x03,0x80,0xde,0x3c,0x62,
    0xbe,0xe0,0x02,0x5c,0xdf,0x81,0x63,0x3d,
    0x7c,0x22,0xc0,0x9e,0x1d,0x43,0xa1,0xff,
    0x46,0x18,0xfa,0xa4,0x27,0x79,0x9b,0xc5,
    0x84,0xda,0x38,0x66,0xe5,0xbb,0x59,0x07,
    0xdb,0x85,0x67,0x39,0xba,0xe4,0x06,0x58,
    0x19,0x47,0xa5,0xfb,0x78,0x26,0xc4,0x9a,
    0x65,0x3b,0xd9,0x87,0x04,0x5a,0xb8,0xe6,
    0xa7,0xf9,0x1b,0x45,0xc6,0x98,0x7a,0x24,
    0xf8,0xa6,0x44,0x1a,0x99,0xc7,0x25,0x7b,
    0x3a,0x64,0x86,0xd8,0x5b,0x05,0xe7,0xb9,
    0x8c,0xd2,0x30,0x6e,0xed,0xb3,0x51,0x0f,
    0x4e,0x10,0xf2,0xac,0x2f,0x71,0x93,0xcd,
    0x11,0x4f,0xad,0xf3,0x70,0x2e,0xcc,0x92,
    0xd3,0x8d,0x6f,0x31,0xb2,0xec,0x0e,0x50,
    0xaf,0xf1,0x13,0x4d,0xce,0x90,0x72,0x2c,
    0x6d,0x33,0xd1,0x8f,0x0c,0x52,0xb0,0xee,
    0x32,0x6c,0x8e,0xd0,0x53,0x0d,0xef,0xb1,
    0xf0,0xae,0x4c,0x12,0x91,0xcf,0x2d,0x73,
    0xca,0x94,0x76,0x28,0xab,0xf5,0x17,0x49,
    0x08,0x56,0xb4,0xea,0x69,0x37,0xd5,0x8b,
    0x57,0x09,0xeb,0xb5,0x36,0x68,0x8a,0xd4,
    0x95,0xcb,0x29,0x77,0xf4,0xaa,0x48,0x16,
    0xe9,0xb7,0x55,0x0b,0x88,0xd6,0x34,0x6a,
    0x2b,0x75,0x97,0xc9,0x4a,0x14,0xf6,0xa8,
    0x74,0x2a,0xc8,0x96,0x15,0x4b,0xa9,0xf7,
    0xb6,0xe8,0x0a,0x54,0xd7,0x89,0x6b,0x35
};
static const uint8_t duss_t16lo[256] = {
    0x00,0x89,0x12,0x9b,0x24,0xad,0x36,0xbf,
    0x48,0xc1,0x5a,0xd3,0x6c,0xe5,0x7e,0xf7,
    0x81,0x08,0x93,0x1a,0xa5,0x2c,0xb7,0x3e,
    0xc9,0x40,0xdb,0x52,0xed,0x64,0xff,0x76,
    0x02,0x8b,0x10,0x99,0x26,0xaf,0x34,0xbd,
    0x4a,0xc3,0x58,0xd1,0x6e,0xe7,0x7c,0xf5,
    0x83,0x0a,0x91,0x18,0xa7,0x2e,0xb5,0x3c,
    0xcb,0x42,0xd9,0x50,0xef,0x66,0xfd,0x74,
    0x04,0x8d,0x16,0x9f,0x20,0xa9,0x32,0xbb,
    0x4c,0xc5,0x5e,0xd7,0x68,0xe1,0x7a,0xf3,
    0x85,0x0c,0x97,0x1e,0xa1,0x28,0xb3,0x3a,
    0xcd,0x44,0xdf,0x56,0xe9,0x60,0xfb,0x72,
    0x06,0x8f,0x14,0x9d,0x22,0xab,0x30,0xb9,
    0x4e,0xc7,0x5c,0xd5,0x6a,0xe3,0x78,0xf1,
    0x87,0x0e,0x95,0x1c,0xa3,0x2a,0xb1,0x38,
    0xcf,0x46,0xdd,0x54,0xeb,0x62,0xf9,0x70,
    0x08,0x81,0x1a,0x93,0x2c,0xa5,0x3e,0xb7,
    0x40,0xc9,0x52,0xdb,0x64,0xed,0x76,0xff,
    0x89,0x00,0x9b,0x12,0xad,0x24,0xbf,0x36,
    0xc1,0x48,0xd3,0x5a,0xe5,0x6c,0xf7,0x7e,
    0x0a,0x83,0x18,0x91,0x2e,0xa7,0x3c,0xb5,
    0x42,0xcb,0x50,0xd9,0x66,0xef,0x74,0xfd,
    0x8b,0x02,0x99,0x10,0xaf,0x26,0xbd,0x34,
    0xc3,0x4a,0xd1,0x58,0xe7,0x6e,0xf5,0x7c,
    0x0c,0x85,0x1e,0x97,0x28,0xa1,0x3a,0xb3,
    0x44,0xcd,0x56,0xdf,0x60,0xe9,0x72,0xfb,
    0x8d,0x04,0x9f,0x16,0xa9,0x20,0xbb,0x32,
    0xc5,0x4c,0xd7,0x5e,0xe1,0x68,0xf3,0x7a,
    0x0e,0x87,0x1c,0x95,0x2a,0xa3,0x38,0xb1,
    0x46,0xcf,0x54,0xdd,0x62,0xeb,0x70,0xf9,
    0x8f,0x06,0x9d,0x14,0xab,0x22,0xb9,0x30,
    0xc7,0x4e,0xd5,0x5c,0xe3,0x6a,0xf1,0x78
};
static const uint8_t duss_t16hi[256] = {
    0x00,0x11,0x23,0x32,0x46,0x57,0x65,0x74,
    0x8c,0x9d,0xaf,0xbe,0xca,0xdb,0xe9,0xf8,
    0x10,0x01,0x33,0x22,0x56,0x47,0x75,0x64,
    0x9c,0x8d,0xbf,0xae,0xda,0xcb,0xf9,0xe8,
    0x21,0x30,0x02,0x13,0x67,0x76,0x44,0x55,
    0xad,0xbc,0x8e,0x9f,0xeb,0xfa,0xc8,0xd9,
    0x31,0x20,0x12,0x03,0x77,0x66,0x54,0x45,
    0xbd,0xac,0x9e,0x8f,0xfb,0xea,0xd8,0xc9,
    0x42,0x53,0x61,0x70,0x04,0x15,0x27,0x36,
    0xce,0xdf,0xed,0xfc,0x88,0x99,0xab,0xba,
    0x52,0x43,0x71,0x60,0x14,0x05,0x37,0x26,
    0xde,0xcf,0xfd,0xec,0x98,0x89,0xbb,0xaa,
    0x63,0x72,0x40,0x51,0x25,0x34,0x06,0x17,
    0xef,0xfe,0xcc,0xdd,0xa9,0xb8,0x8a,0x9b,
    0x73,0x62,0x50,0x41,0x35,0x24,0x16,0x07,
    0xff,0xee,0xdc,0xcd,0xb9,0xa8,0x9a,0x8b,
    0x84,0x95,0xa7,0xb6,0xc2,0xd3,0xe1,0xf0,
    0x08,0x19,0x2b,0x3a,0x4e,0x5f,0x6d,0x7c,
    0x94,0x85,0xb7,0xa6,0xd2,0xc3,0xf1,0xe0,
    0x18,0x09,0x3b,0x2a,0x5e,0x4f,0x7d,0x6c,
    0xa5,0xb4,0x86,0x97,0xe3,0xf2,0xc0,0xd1,
    0x29,0x38,0x0a,0x1b,0x6f,0x7e,0x4c,0x5d,
    0xb5,0xa4,0x96,0x87,0xf3,0xe2,0xd0,0xc1,
    0x39,0x28,0x1a,0x0b,0x7f,0x6e,0x5c,0x4d,
    0xc6,0xd7,0xe5,0xf4,0x80,0x91,0xa3,0xb2,
    0x4a,0x5b,0x69,0x78,0x0c,0x1d,0x2f,0x3e,
    0xd6,0xc7,0xf5,0xe4,0x90,0x81,0xb3,0xa2,
    0x5a,0x4b,0x79,0x68,0x1c,0x0d,0x3f,0x2e,
    0xe7,0xf6,0xc4,0xd5,0xa1,0xb0,0x82,0x93,
    0x6b,0x7a,0x48,0x59,0x2d,0x3c,0x0e,0x1f,
    0xf7,0xe6,0xd4,0xc5,0xb1,0xa0,0x92,0x83,
    0x7b,0x6a,0x58,0x49,0x3d,0x2c,0x1e,0x0f
};

static uint8_t duss_crc8(const uint8_t *d, int n, uint8_t init)
{
    uint8_t c = init;
    int i;
    for (i = 0; i < n; i++) c = duss_t8[c ^ d[i]];
    return c;
}

static uint16_t duss_crc16(const uint8_t *d, int n, uint16_t init)
{
    uint16_t c = init;
    int i;
    for (i = 0; i < n; i++) {
        uint8_t idx = (uint8_t)((c ^ d[i]) & 0xff);
        c = (uint16_t)((c >> 8) | (duss_t16hi[idx] << 8));
        c = (uint16_t)(c ^ duss_t16lo[idx]);
    }
    return c;
}

static uint16_t duss_seq_ctr = 0x0100;

/* build DUSS frame into out (max 64); returns total len */
static int duss_build(uint8_t *out, uint8_t cmd_id, const uint8_t *payload, int plen)
{
    int len = 11 + plen + 2;
    uint16_t c16;
    out[0] = 0x55;
    out[1] = (uint8_t)(len & 0xff);
    out[2] = (uint8_t)(0x04 | ((len >> 8) & 0x03));
    out[3] = duss_crc8(out, 3, 0x77);
    out[4] = 0x07;                 /* sender: camera (0x700) */
    out[5] = 0x3a;                 /* receiver: TX (0x1A01) */
    out[6] = (uint8_t)(duss_seq_ctr & 0xff);
    out[7] = (uint8_t)(duss_seq_ctr >> 8);
    duss_seq_ctr++;
    out[8] = 0x40;
    out[9] = 0x12;                 /* cmd_set: Mic */
    out[10] = cmd_id;
    if (plen > 0) memcpy(out + 11, payload, plen);
    c16 = duss_crc16(out, len - 2, 0x3692);
    out[len - 2] = (uint8_t)(c16 & 0xff);
    out[len - 1] = (uint8_t)(c16 >> 8);
    return len;
}

/* like duss_build but with explicit receiver byte (0x18 = relay addr) */
static int duss_build_r(uint8_t *out, uint8_t cmd_id, const uint8_t *payload, int plen, uint8_t receiver)
{
    int len = 11 + plen + 2;
    uint16_t c16;
    out[0] = 0x55;
    out[1] = (uint8_t)(len & 0xff);
    out[2] = (uint8_t)(0x04 | ((len >> 8) & 0x03));
    out[3] = duss_crc8(out, 3, 0x77);
    out[4] = 0x07;
    out[5] = receiver;
    out[6] = (uint8_t)(duss_seq_ctr & 0xff);
    out[7] = (uint8_t)(duss_seq_ctr >> 8);
    duss_seq_ctr++;
    out[8] = 0x40;
    out[9] = 0x12;
    out[10] = cmd_id;
    if (plen > 0) memcpy(out + 11, payload, plen);
    c16 = duss_crc16(out, len - 2, 0x3692);
    out[len - 2] = (uint8_t)(c16 & 0xff);
    out[len - 1] = (uint8_t)(c16 >> 8);
    return len;
}

/* wrap into AA ver=0 frame: AA <be16 count=0|len> duss A5 */
static int aa_wrap(uint8_t *out, int outsz, const uint8_t *duss, int dlen)
{
    int v = dlen & 0x3ff;
    if (3 + dlen + 1 > outsz) return -1;
    out[0] = 0xaa;
    out[1] = (uint8_t)((v >> 8) & 0xff);
    out[2] = (uint8_t)(v & 0xff);
    memcpy(out + 3, duss, dlen);
    out[3 + dlen] = 0xa5;
    return dlen + 4;
}

/* ------------------------------------------------------------------ */
/* BSA daemon client (scan / pair / unpair / fallback disconnect)      */
/* ------------------------------------------------------------------ */
enum { S_LEN0, S_LEN1, S_ID0, S_ID1, S_PL };
typedef struct {
    int st;
    unsigned len, id, need, got;
    int err;
    uint8_t buf[MAXPL];
} framer_t;

static struct {
    int fd;
    framer_t fr;
    pthread_mutex_t mtx;
    int wait_id, got_resp;
    unsigned resp_id;
    unsigned char resp[MAXPL];
    int resp_len;
    /* event side effects */
    int auto_grant;               /* answer SP_CFM_REQ with accept */
    /* bond capture */
    uint8_t bond_bd[6];
    uint8_t bond_key[16];
    uint8_t bond_key_type;
    uint8_t bond_ok;              /* 1 success / 2 fail / 0 pending */
    int bond_active;
    uint8_t bond_target[6];
    char bond_name[249];
    /* scan collection */
    struct { uint8_t bd[6]; char name[48]; unsigned cod; int rssi; } seen[64];
    int nseen, scan_mode;
    int disc_done;
} B;                                /* NOTE: B.fd MUST start at -1 (0 = stdin!) */

static void fr_init(framer_t *f) { memset(f, 0, sizeof *f); f->st = S_LEN0; }
static void fr_reset(framer_t *f)
{
    f->st = S_LEN0; f->len = f->id = f->need = f->got = 0; f->err = 0;
}

static void bsa_deliver(unsigned id, const unsigned char *pl, int n);

static void fr_feed(framer_t *f, const unsigned char *d, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        unsigned char b = d[i];
        switch (f->st) {
        case S_LEN0: f->len = b; f->st = S_LEN1; break;
        case S_LEN1:
            f->len |= (unsigned)b << 8;
            if (f->len <= 1) fr_reset(f);
            else if (f->len > 0x800) { f->err = 1; f->need = f->len - 2; f->st = S_PL; f->got = 0; }
            else { f->need = f->len - 2; f->st = S_ID0; }
            break;
        case S_ID0: f->id = b; f->st = S_ID1; break;
        case S_ID1:
            f->id |= (unsigned)b << 8; f->st = S_PL; f->got = 0;
            if (f->need == 0) { if (!f->err) bsa_deliver(f->id, NULL, 0); fr_reset(f); }
            break;
        case S_PL:
            if (!f->err && f->got < MAXPL) f->buf[f->got] = b;
            f->got++;
            if (f->got >= f->need) {
                if (!f->err) bsa_deliver(f->id, f->buf, (int)(f->got < MAXPL ? f->got : MAXPL));
                fr_reset(f);
            }
            break;
        default: f->st = S_LEN0; break;
        }
    }
}

static int bsa_conn(void)
{
    struct sockaddr_un a;
    int fd;
    if (B.fd >= 0) return B.fd;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { mlog("bsa_conn: socket: %s", strerror(errno)); return -1; }
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, BT_SOCK, sizeof a.sun_path - 1);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        mlog("bsa_conn: connect %s: %s", BT_SOCK, strerror(errno));
        close(fd);
        return -1;
    }
    B.fd = fd;
    fr_init(&B.fr);
    mlog("bsa_conn: fd=%d connected", fd);
    return fd;
}

static int bsa_send(unsigned id, const void *payload, int plen)
{
    unsigned char hdr[4];
    int len = 2 + plen;
    hdr[0] = (unsigned char)(len & 0xff);
    hdr[1] = (unsigned char)(len >> 8);
    hdr[2] = (unsigned char)(id & 0xff);
    hdr[3] = (unsigned char)(id >> 8);
    if (send(B.fd, hdr, 4, MSG_NOSIGNAL) != 4 ||
        (plen > 0 && send(B.fd, payload, plen, MSG_NOSIGNAL) != plen)) {
        /* v4: a dead client socket (bsa_server restarted by the
         * dji_network watchdog) must be dropped so the next bsa_conn()
         * reconnects -- otherwise every scan/pair/RSSI fails forever */
        mlog("bsa_send(0x%03x): %s -> drop client socket, reconnect next call",
             id, strerror(errno));
        close(B.fd);
        B.fd = -1;
        return -1;
    }
    return 0;
}

static unsigned rd16b(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static void wr16b(unsigned char *p, unsigned v) { p[0] = (unsigned char)(v & 0xff); p[1] = (unsigned char)(v >> 8); }
static uint32_t rd32b(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int seen_bd(const uint8_t *bd)
{
    int i;
    for (i = 0; i < B.nseen; i++)
        if (!memcmp(B.seen[i].bd, bd, 6)) return 1;
    return 0;
}

static void bsa_deliver(unsigned id, const unsigned char *pl, int n)
{
    unsigned base = id & 0xFFF;

    if (B.wait_id && (id & 0x8000) && base == (unsigned)B.wait_id) {
        B.resp_id = id;
        B.resp_len = n;
        if (n > 0) memcpy(B.resp, pl, n > MAXPL ? MAXPL : n);
        B.got_resp = 1;
        return;
    }
    if (!(id & 0x4000)) return;   /* only async events below */
    switch (base) {
    case 0xCB: {                  /* discovery record */
        char nm[64];
        int nm_len = 0, i;
        unsigned cod = 0;
        int have_cod = 0, rssi = 0;
        if (n >= 9) {
            cod = (unsigned)pl[6] | ((unsigned)pl[7] << 8) | ((unsigned)pl[8] << 16);
            have_cod = 1;
        }
        if (n > 0x108) rssi = (int)(int32_t)rd32b(pl + 0x104);
        if (n > 9) {
            int k;
            for (k = 0; k < 40 && 9 + k < n && pl[9 + k]; k++) nm[k] = pl[9 + k];
            nm[k] = 0;
            if (k > 0) nm_len = k;
        }
        /* EIR blob 1: len@0x118, TLVs */
        if (n > 0x119 && pl[0x118]) {
            int lim = 0x119 + (pl[0x118] > 0xf0 ? 0xf0 : pl[0x118]);
            if (lim > n) lim = n;
            for (i = 0x119; i + 1 < lim; ) {
                int tl = pl[i], tt = pl[i + 1];
                if (tl == 0 || i + 1 + tl > lim) break;
                if ((tt == 0x09 || tt == 0x08) && tl > 1) {
                    int L = tl - 1; if (L > 63) L = 63;
                    memcpy(nm, pl + i + 2, L); nm[L] = 0; nm_len = L;
                }
                i += 1 + tl;
            }
        }
        if (B.scan_mode && n >= 6 && B.nseen < 64 && !seen_bd(pl)) {
            int k;
            memcpy(B.seen[B.nseen].bd, pl, 6);
            for (k = 0; k < 47 && k < nm_len; k++) B.seen[B.nseen].name[k] = nm[k];
            B.seen[B.nseen].name[k] = 0;
            B.seen[B.nseen].cod = have_cod ? cod : 0;
            B.seen[B.nseen].rssi = rssi;
            B.nseen++;
        }
        break;
    }
    case 0xCC:                    /* inquiry complete */
        if (!B.scan_mode)         /* stray 0xCC-base event outside a scan:
                                     used to latch disc_done and poison every
                                     later bsa_sync (Bug1) -- now only logged */
            mlog("bsa: stray inquiry-complete (base 0x%x) outside scan", base);
        B.disc_done = 1;
        break;
    case 0x13E:                   /* SP_CFM_REQ: auto-accept */
        if (n >= 8 && B.bond_active && !memcmp(pl, B.bond_target, 6)) {
            unsigned char r[8];
            memcpy(r, pl, 6);
            r[6] = 1;
            r[7] = 0;
            bsa_send(0x131, r, 8);
            mlog("bsa: auto SpCfmReply accept for %02X:%02X:%02X:%02X:%02X:%02X",
                 pl[0], pl[1], pl[2], pl[3], pl[4], pl[5]);
        }
        break;
    case 0x13B:                   /* AUTH_CMPL */
        if (n > 0x112 && B.bond_active && !memcmp(pl, B.bond_target, 6)) {
            int k, nz = 0;
            memcpy(B.bond_bd, pl, 6);
            B.bond_ok = pl[0x111] ? 1 : 2;
            B.bond_key_type = pl[0x110];
            /* Action5 callback@423a74 and X2D server@16aed8:
             * key_present=255, key[16]=256..271, type=272, success=273. */
            memset(B.bond_key, 0, sizeof B.bond_key);
            if (B.bond_ok == 1 && pl[0xff]) memcpy(B.bond_key, pl + 0x100, 16);
            for (k = 0; k < 248 && 6 + k < n && pl[6 + k]; k++) B.bond_name[k] = pl[6 + k];
            B.bond_name[k] = 0;
            for (k = 0; k < 16; k++) if (B.bond_key[k]) nz = 1;
            mlog("bsa: AUTH_CMPL %s name='%s' reason=%u key_present=%d",
                 B.bond_ok == 1 ? "SUCCESS" : "FAIL", B.bond_name, pl[0x112], nz);
            (void)nz;
        }
        break;
    default:
        /* first sighting of each unknown async event id -- diagnostics for
         * protocol surprises without flooding the log */
        {
            static unsigned short seen_ids[16];
            static int nseen_ids;
            int j, known = 0;
            for (j = 0; j < nseen_ids; j++)
                if (seen_ids[j] == (unsigned short)(id & 0x3FFF)) { known = 1; break; }
            if (!known && nseen_ids < 16) {
                seen_ids[nseen_ids++] = (unsigned short)(id & 0x3FFF);
                mlog("bsa: async event id=0x%04x len=%d (unhandled)", id, n);
            }
        }
        break;
    }
}

/* pump events / wait for sync resp.
 * for_disc=1: early-return once B.disc_done (inquiry complete) -- scan use.
 * for_disc=0: ONLY a matched sync response ends the wait.  (Bug1 fix: the
 *   old code OR'ed disc_done into every bsa_sync wait, so any stray event
 *   with base id 0xCC arriving on our client socket latched disc_done=1
 *   forever and every later bsa_sync -- including the TmDisconnect fallback
 *   in miccore_disconnect -- returned instantly with no response read:
 *   "TmDisconnect resp -1 status=65535" within the same millisecond.) */
static int bsa_pump2(int timeout_ms, int for_disc)
{
    uint64_t deadline = now_ms() + (uint64_t)timeout_ms;
    for (;;) {
        if (B.fd < 0 || B.fd >= FD_SETSIZE) return -1;
        uint64_t left;
        fd_set rf;
        struct timeval tv;
        unsigned char chunk[4096];
        int r;
        if ((B.wait_id && B.got_resp) ||
            (for_disc && (B.disc_done || atomic_load(&discovery_cancel)))) return 0;
        left = deadline - now_ms();
        if (left == 0 || (int64_t)left < 0) {
            mlog("bsa_pump: timeout (fd=%d wait_id=0x%03x)", B.fd, B.wait_id);
            return -1;
        }
        FD_ZERO(&rf); FD_SET(B.fd, &rf);
        if (for_disc && left > 250) left = 250;
        tv.tv_sec = (time_t)(left / 1000);
        tv.tv_usec = (suseconds_t)((left % 1000) * 1000);
        r = select(B.fd + 1, &rf, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;
            mlog("bsa_pump: select: %s", strerror(errno));
            return -1;
        }
        if (r == 0) { if (for_disc) continue; return B.got_resp ? 0 : -1; }
        r = (int)read(B.fd, chunk, sizeof chunk);
        if (r <= 0) {
            mlog("bsa_pump: read=%d errno=%s (server closed?)", r, strerror(errno));
            close(B.fd);
            B.fd = -1;
            return -1;
        }
        fr_feed(&B.fr, chunk, r);
    }
}

static int bsa_sync(unsigned id, const void *pl, int plen, unsigned char *out, int outsz, int timeout_ms)
{
    int rc;
    pthread_mutex_lock(&B.mtx);
    if (bsa_conn() < 0) { pthread_mutex_unlock(&B.mtx); return -1; }
    B.wait_id = (int)(id & 0xFFF);
    B.got_resp = 0;
    B.resp_len = -1;
    if (bsa_send(id, pl, plen) < 0 &&
        /* v4: one transparent retry on a fresh socket (server restart) */
        (bsa_conn() < 0 || bsa_send(id, pl, plen) < 0)) {
        B.wait_id = 0;
        pthread_mutex_unlock(&B.mtx);
        return -1;
    }
    rc = bsa_pump2(timeout_ms, 0);
    B.wait_id = 0;
    B.got_resp = 0;
    if (rc == 0 && out && B.resp_len > 0)
        memcpy(out, B.resp, B.resp_len < outsz ? B.resp_len : outsz);
    pthread_mutex_unlock(&B.mtx);
    return rc == 0 ? B.resp_len : -1;
}

/* v5: is the baseband ACL to bd up right now?  TmReadRawRssi (0x582)
 * answers status=0 + rssi only while an ACL exists (status=0xC800 when
 * there is none).  Used by the reconnect path to decide between opening
 * SPP directly on a live link vs. re-engaging HFP for an absent TX. */
static int acl_is_up(const uint8_t *bd)
{
    unsigned char r[MAXPL];
    int n = bsa_sync(0x582, bd, 6, r, sizeof r, 1500);
    return n >= 12 && rd16b(r) == 0;
}

/* ------------------------------------------------------------------ */
/* bt_devices.xml helpers (name lookup, pair persist, unpair removal)  */
/* ------------------------------------------------------------------ */
static char *xmemstr(char *hay, int haylen, const char *needle)
{
    int nl = (int)strlen(needle), i;
    if (nl <= 0 || haylen < nl) return NULL;
    for (i = 0; i <= haylen - nl; i++)
        if (!strncasecmp(hay + i, needle, nl)) return hay + i;
    return NULL;
}

static int xml_field(char *s, int len, const char *tag, char *out, int outsz)
{
    char pat[64], *p, *q;
    int vl;
    snprintf(pat, sizeof pat, "<%s>", tag);
    p = xmemstr(s, len, pat);
    if (!p) return -1;
    p += strlen(pat);
    q = memchr(p, '<', (size_t)(s + len - p));
    if (!q) return -1;
    vl = (int)(q - p);
    if (vl > outsz - 1) vl = outsz - 1;
    memcpy(out, p, vl);
    out[vl] = 0;
    return vl;
}

static int bd_normalize(const char *in, char out[13])
{
    int i, j = 0;
    for (i = 0; in[i] && j < 12; i++) {
        char c = (char)tolower((unsigned char)in[i]);
        if (c == ':' || c == '-') continue;
        if (!isxdigit((unsigned char)c)) return -1;
        out[j++] = c;
    }
    out[j] = 0;
    return j == 12 ? 0 : -1;
}

static char *xml_each_device(char *buf, int len, char **cursor, char **be)
{
    char *start, *end, *stop = buf + len, *p;
    start = *cursor ? *cursor : buf;
    for (;;) {
        p = xmemstr(start, (int)(stop - start), "<device");
        if (!p) return NULL;
        if (p[7] == ' ' || p[7] == '>' || p[7] == '\t' || p[7] == '\r' || p[7] == '\n') break;
        start = p + 7;
    }
    end = xmemstr(p, (int)(stop - p), "</device>");
    if (!end) return NULL;
    end += 9;
    while (end < stop && (*end == '\n' || *end == '\r')) end++;
    *cursor = end;
    if (be) *be = end;
    return p;
}

static char *xml_find_device(char *buf, int len, const char *norm, char **be)
{
    char *cur = NULL, *blk;
    char bd[64], n2[13];
    while ((blk = xml_each_device(buf, len, &cur, be)) != NULL) {
        if (xml_field(blk, (int)(*be - blk), "bd_addr", bd, sizeof bd) >= 0 &&
            bd_normalize(bd, n2) == 0 && !strcmp(n2, norm))
            return blk;
    }
    return NULL;
}

static char *xml_load(const char *path, int *lenout)
{
    FILE *f = fopen(path, "rb");
    struct stat before, after;
    long sz;
    char *buf;
    if (!f) return NULL;
    if (fstat(fileno(f), &before)) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 512 * 1024) { fclose(f); return NULL; }
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return NULL; }
    /* BSA also writes this file in place. A read overlapping its truncate /
     * rewrite is not an authoritative database snapshot. */
    if (fstat(fileno(f), &after) || before.st_size != after.st_size ||
            before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
            before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
            before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
            before.st_ctim.tv_nsec != after.st_ctim.tv_nsec) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    buf[sz] = 0;
    *lenout = (int)sz;
    return buf;
}

static int xml_save(const char *path, const char *buf, int len)
{
    char tmp[300];
    struct stat st;
    int fd;
    if (len < 0 || snprintf(tmp, sizeof tmp, "%s.XXXXXX", path) >= (int)sizeof tmp) return -1;
    fd = mkstemp(tmp);
    if (fd < 0) return -1;
    if (stat(path, &st) == 0) {
        if (fchown(fd, st.st_uid, st.st_gid) != 0 || fchmod(fd, st.st_mode & 0777) != 0)
            goto fail;
    }
    for (int sent = 0; sent < len;) {
        ssize_t n = write(fd, buf + sent, (size_t)(len - sent));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto fail;
        sent += (int)n;
    }
    if (fsync(fd) != 0) goto fail;
    if (close(fd) != 0) { unlink(tmp); return -1; }
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
fail:
    close(fd);
    unlink(tmp);
    return -1;
}

static int xml_count_devices(char *buf, int len)
{
    char *cur = NULL, *be;
    int n = 0;
    while (xml_each_device(buf, len, &cur, &be)) n++;
    return n;
}

static int xml_replace_tag(char **buffer, int *len, const char *norm, const char *tag, const char *newval)
{
    char *buf = *buffer, *end;
    char *blk = xml_find_device(buf, *len, norm, &end);
    if (!blk) return -1;
    int blklen = (int)(end - blk);
    char op[64], cl[64], *o, *c;
    int olen, nlen, tail;
    snprintf(op, sizeof op, "<%s>", tag);
    snprintf(cl, sizeof cl, "</%s>", tag);
    o = xmemstr(blk, blklen, op);
    if (!o) return -1;
    o += strlen(op);
    c = xmemstr(o, (int)(buf + *len - o), cl);
    if (!c || c > blk + blklen) return -1;
    olen = (int)(c - o);
    nlen = (int)strlen(newval);
    tail = *len - (int)(c - buf);
    size_t offset = (size_t)(o - buf), coffset = (size_t)(c - buf);
    char *grown = realloc(buf, (size_t)*len + (size_t)nlen + 1);
    if (!grown) return -1;
    *buffer = buf = grown;
    o = buf + offset;
    c = buf + coffset;
    memmove(o + nlen, c, (size_t)tail);
    memcpy(o, newval, (size_t)nlen);
    *len += nlen - olen;
    buf[*len] = 0;
    return 0;
}

/* Device names are text, never XML markup. */
static void xml_escape_name(const char *name, char out[1500])
{
    size_t used = 0;
    for (int i = 0; name && name[i] && i < 248; i++) {
        const char *replacement = name[i] == '&' ? "&amp;" : name[i] == '<' ? "&lt;"
            : name[i] == '>' ? "&gt;" : NULL;
        if (replacement) { size_t n = strlen(replacement); memcpy(out + used, replacement, n); used += n; }
        else out[used++] = (unsigned char)name[i] < 32 ? ' ' : name[i];
    }
    out[used] = 0;
}

static void lookup_name(const uint8_t *bd, char *out, int outsz)
{
    int len;
    char *buf, *blk = NULL, *be = NULL;
    char norm[13];
    snprintf(norm, sizeof norm, "%02x%02x%02x%02x%02x%02x",
             bd[0], bd[1], bd[2], bd[3], bd[4], bd[5]);
    snprintf(out, outsz, "");
    buf = xml_load(DEV_XML, &len);
    if (!buf) return;
    blk = xml_find_device(buf, len, norm, &be);
    if (blk) {
        char nm[128];
        if (xml_field(blk, (int)(be - blk), "device_name", nm, sizeof nm) >= 0 && nm[0])
            snprintf(out, outsz, "%s", nm);
    }
    free(buf);
}

/* persist bond result into bt_devices.xml (savekey lineage) */
static int persist_key(const uint8_t *bd, const uint8_t *key, int ktype, const char *name)
{
    int len, blen, i, k;
    char *buf, *blk, *be, *bak = NULL;
    char norm[13], keyhex[64], bds[24];
    unsigned kwords[16];
    char escaped_name[1500];

    if (!key) return -1; /* Never invent a pairing record without a key. */
    int nonzero = 0;
    for (i = 0; i < 16; i++) nonzero |= key[i];
    if (!nonzero) return -1;
    xml_escape_name(name, escaped_name);

    snprintf(norm, sizeof norm, "%02x%02x%02x%02x%02x%02x",
             bd[0], bd[1], bd[2], bd[3], bd[4], bd[5]);
    snprintf(bds, sizeof bds, "%02X:%02X:%02X:%02X:%02X:%02X",
             bd[0], bd[1], bd[2], bd[3], bd[4], bd[5]);
    for (i = 0; i < 16; i++) kwords[i] = key[i];
    snprintf(keyhex, sizeof keyhex,
             "%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
             kwords[0], kwords[1], kwords[2], kwords[3], kwords[4], kwords[5], kwords[6], kwords[7],
             kwords[8], kwords[9], kwords[10], kwords[11], kwords[12], kwords[13], kwords[14], kwords[15]);

    buf = xml_load(DEV_XML, &len);
    if (!buf) { mlog("persist_key: cannot read %s", DEV_XML); return -1; }
    bak = xml_load(DEV_XML ".micbak", &blen);
    blk = xml_find_device(buf, len, norm, &be);
    if (blk) {
        char kt[16];
        snprintf(kt, sizeof kt, "%d", ktype);
        if (xml_replace_tag(&buf, &len, norm, "link_key", keyhex) != 0 ||
            xml_replace_tag(&buf, &len, norm, "Link_key_present", "1") != 0 ||
            xml_replace_tag(&buf, &len, norm, "key_type", kt) != 0) {
            mlog("persist_key: entry lacks key fields");
            free(buf); free(bak);
            return -1;
        }
        if (name && name[0])
            xml_replace_tag(&buf, &len, norm, "device_name", escaped_name);
        if (xml_save(DEV_XML, buf, len) != 0) { free(buf); free(bak); return -1; }
        mlog("persist_key: updated entry %s", bds);
    } else {
        char nm[128] = "", cod[32] = "00:00:00", tr[32] = "0x40000006";
        char vid[32] = "0x0000", pidv[32] = "0x0000", ver[32] = "0x0000";
        char feat[64] = "00:00:00:00:00:00:00:00";
        char blkbuf[3072];
        char *bblk = NULL, *bbe = NULL, *ins, *cur = NULL, *dblk, *ibe;
        int idx = 0, bl2;
        if (bak && (bblk = xml_find_device(bak, blen, norm, &bbe)) != NULL) {
            xml_field(bblk, (int)(bbe - bblk), "device_name", nm, sizeof nm);
            xml_field(bblk, (int)(bbe - bblk), "class_of_device", cod, sizeof cod);
            xml_field(bblk, (int)(bbe - bblk), "trusted_services", tr, sizeof tr);
            xml_field(bblk, (int)(bbe - bblk), "vid", vid, sizeof vid);
            xml_field(bblk, (int)(bbe - bblk), "pid", pidv, sizeof pidv);
            xml_field(bblk, (int)(bbe - bblk), "version", ver, sizeof ver);
            xml_field(bblk, (int)(bbe - bblk), "features", feat, sizeof feat);
        }
        while ((dblk = xml_each_device(buf, len, &cur, &ibe)) != NULL) {
            char *v = xmemstr(dblk, (int)(ibe - dblk), "value");
            if (v) {
                v += 5;
                while (*v == ' ' || *v == '=' || *v == '"') v++;
                if (atoi(v) > idx) idx = atoi(v);
            }
        }
        idx++;
        bl2 = snprintf(blkbuf, sizeof blkbuf,
            "    <device value = \"%d\">\n"
            "      <bd_addr>%02X:%02X:%02X:%02X:%02X:%02X</bd_addr>\n"
            "      <device_name>%s</device_name>\n"
            "      <class_of_device>%s</class_of_device>\n"
            "      <link_key>%s</link_key>\n"
            "      <Link_key_present>1</Link_key_present>\n"
            "      <key_type>%d</key_type>\n"
            "      <trusted_services>%s</trusted_services>\n"
            "      <vid>%s</vid>\n"
            "      <pid>%s</pid>\n"
            "      <version>%s</version>\n"
            "      <features>%s</features>\n"
            "      <lmp_version>0</lmp_version>\n"
            "      <ble_addr_type>0</ble_addr_type>\n"
            "      <device_type>0</device_type>\n"
            "      <inq_result_type>0</inq_result_type>\n"
            "      <ble_Link_key_present>0</ble_Link_key_present>\n"
            "      <penc_ltk>00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00</penc_ltk>\n"
            "      <penc_rand>00:00:00:00:00:00:00:00</penc_rand>\n"
            "      <penc_ediv>0</penc_ediv>\n"
            "      <penc_sec_level>0</penc_sec_level>\n"
            "      <penc_key_size>0</penc_key_size>\n"
            "      <prid_irk>00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00</prid_irk>\n"
            "      <prid_addr_type>0</prid_addr_type>\n"
            "      <prid_static_addr>00:00:00:00:00:00</prid_static_addr>\n"
            "      <pcsrk_counter>0</pcsrk_counter>\n"
            "      <pcsrk_csrk>00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00</pcsrk_csrk>\n"
            "      <pcsrk_sec_level>0</pcsrk_sec_level>\n"
            "      <lcsrk_counter>0</lcsrk_counter>\n"
            "      <lcsrk_csrk>00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00</lcsrk_csrk>\n"
            "      <lcsrk_dis>0</lcsrk_dis>\n"
            "      <lcsrk_sec_level>0</lcsrk_sec_level>\n"
            "      <lenc_ltk>00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00</lenc_ltk>\n"
            "      <lenc_div>0</lenc_div>\n"
            "      <lenc_key_size>0</lenc_key_size>\n"
            "      <lenc_sec_level>0</lenc_sec_level>\n"
            "    </device>\n",
            idx, bd[0], bd[1], bd[2], bd[3], bd[4], bd[5],
            escaped_name[0] ? escaped_name : nm, cod, keyhex, ktype, tr, vid, pidv, ver, feat);
        (void)k;
        if (bl2 < 0 || bl2 >= (int)sizeof blkbuf) { free(buf); free(bak); return -1; }
        /* xml_load allocates exactly sz+1; the insert below writes len+bl2
         * bytes -- grow first or the memmove runs off the allocation */
        {
            char *grown = (char *)realloc(buf, (size_t)len + (size_t)bl2 + 1);
            if (!grown) { free(buf); free(bak); return -1; }
            buf = grown;
        }
        ins = xmemstr(buf, len, "</bt_devices>");
        if (!ins) { free(buf); free(bak); return -1; }
        memmove(ins + bl2, ins, (size_t)(len - (int)(ins - buf)));
        memcpy(ins, blkbuf, (size_t)bl2);
        len += bl2;
        buf[len] = 0;
        if (xml_save(DEV_XML, buf, len) != 0) { free(buf); free(bak); return -1; }
        mlog("persist_key: appended entry idx=%d for %s name='%s'", idx, bds, nm);
    }
    free(buf);
    free(bak);
    return 0;
}

/* remove a device entry from bt_devices.xml (backup first) */
static int xml_remove_device(const uint8_t *bd)
{
    int len, n0, nchk;
    char *buf, *blk, *be, norm[13];
    snprintf(norm, sizeof norm, "%02x%02x%02x%02x%02x%02x",
             bd[0], bd[1], bd[2], bd[3], bd[4], bd[5]);
    buf = xml_load(DEV_XML, &len);
    if (!buf) return -1;
    n0 = xml_count_devices(buf, len);
    blk = xml_find_device(buf, len, norm, &be);
    if (!blk) { free(buf); return 0; }   /* not present: nothing to do */
    {
        int blen;
        char *bak = xml_load(DEV_XML, &blen);
        if (bak) { xml_save(DEV_XML ".micbak", bak, blen); free(bak); }
    }
    memmove(blk, be, (size_t)(len - (int)(be - buf)));
    len -= (int)(be - blk);
    nchk = xml_count_devices(buf, len);
    if (nchk != n0 - 1 || !xmemstr(buf, len, "</bt_devices>")) {
        free(buf);
        return -1;
    }
    if (xml_save(DEV_XML, buf, len) != 0) { free(buf); return -1; }
    free(buf);
    return 0;
}

/* v2 paired semantics: does the XML DB hold a key entry for this bd?
 * (device block present AND Link_key_present != 0) */
/* -1 means unavailable/incomplete, 0 confirmed absent, 1 confirmed present.
 * Never turn a transient read failure into a native admission revocation. */
static int xml_key_state(const uint8_t *bd)
{
    int len;
    char *buf, *blk, *be, norm[13], lk[32];
    int rc = 0;
    snprintf(norm, sizeof norm, "%02x%02x%02x%02x%02x%02x",
             bd[0], bd[1], bd[2], bd[3], bd[4], bd[5]);
    buf = xml_load(DEV_XML, &len);
    if (!buf) return -1;
    char *begin = xmemstr(buf, len, "<bt_devices>");
    char *end = xmemstr(buf, len, "</bt_devices>");
    if (memchr(buf, 0, (size_t)len) || !begin || !end || begin > end) {
        free(buf); return -1;
    }
    begin += strlen("<bt_devices>");
    /* A partial device block cannot establish absence of a later address,
     * even if a stale closing container tag remains in an in-place write. */
    char *cursor = begin;
    while (cursor < end) {
        char *next = xmemstr(cursor, (int)(end - cursor), "<device");
        if (!next) break;
        if (next + 7 >= end || !(isspace((unsigned char)next[7]) || next[7] == '>')) {
            cursor = next + 7; continue;
        }
        char *closed = xmemstr(next, (int)(end - next), "</device>");
        if (!closed) {
            free(buf); return -1;
        }
        char *nested = next + 7;
        while ((nested = xmemstr(nested, (int)(closed - nested), "<device")) != NULL) {
            if (isspace((unsigned char)nested[7]) || nested[7] == '>') {
                free(buf); return -1;
            }
            nested += 7; /* device_name and device_type are ordinary fields */
        }
        cursor = closed + 9;
    }
    blk = xml_find_device(begin, (int)(end - begin), norm, &be);
    if (blk) {
        lk[0] = 0;
        char *close = xmemstr(blk, (int)(be - blk), "</Link_key_present>");
        if (!close || xml_field(blk, (int)(be - blk), "Link_key_present", lk, sizeof lk) < 0)
            rc = -1;
        else {
            char *tail; errno = 0;
            long present = strtol(lk, &tail, 10);
            int numeric = tail != lk;
            while (isspace((unsigned char)*tail)) tail++;
            rc = errno || !numeric || *tail || (present != 0 && present != 1) ? -1 : (int)present;
        }
    }
    free(buf);
    return rc;
}

static int xml_has_key(const uint8_t *bd) { return xml_key_state(bd) == 1; }

/* Backups are diagnostic only: never re-install an unverified old key. */

/* Caller holds st_mtx. The native callback reads only this value snapshot. */
static void tx_admission_update(struct mic_tx *tx)
{
    int policy = MIC_ADMISSION_NONE;
    if (!E.offline && tx->cfg_bd_valid && tx->paired)
        policy = tx->user_hold ? MIC_ADMISSION_HELD : MIC_ADMISSION_ALLOW;
    mic_admission_set((unsigned)(tx - TX), tx->cfg_bd, policy);
}

/* recompute paired / name from the XML DB (call from hk + after
 * pair/unpair/persist); name is blanked when unpaired (v2 semantics) */
static void update_paired(struct mic_tx *tx)
{
    int p = (tx->cfg_bd_valid && !E.offline) ? xml_key_state(tx->cfg_bd) : 0;
    pthread_mutex_lock(&E.st_mtx);
    if (p >= 0 && p != tx->paired) {
        tx->paired = p;
        if (p)
            lookup_name(tx->cfg_bd, tx->name, sizeof tx->name);
        else
            tx->name[0] = 0;
        mlog("paired=%d (xml key entry %s)", p, p ? "present" : "absent");
    }
    tx_admission_update(tx);
    pthread_mutex_unlock(&E.st_mtx);
}

/* ------------------------------------------------------------------ */
/* PCM sinks                                                           */
/* ------------------------------------------------------------------ */
static void fifo_write(const int16_t *s, int n)
{
    int r;
    if (E.pcm_fd < 0) return;
    r = (int)write(E.pcm_fd, s, (size_t)n * 2);
    (void)r;
    if (r < 0 && (errno == EPIPE || errno == EBADF)) {
        close(E.pcm_fd);
        E.pcm_fd = -1;
    }
    /* EAGAIN: reader too slow -> drop, per INTERFACE.md */
}

/* Video takes retain wall-clock gaps as silence after a bounded receive
 * allowance; stop fills the final tail. Source positions prevent late replay. */
static int rec_active_locked(void)
{
    return E.rec_finalizing || E.rec_f || (E.rec_aac && !mic_aac_snapshot(E.rec_aac).completed);
}

/* Keep rec_active_locked true during AAC flush for ownership/start guards.
 * Producers stop immediately when finish closes the sink to new input. */
static int rec_accepting_locked(void)
{
    return E.rec_f || (E.rec_aac && mic_aac_accepting(E.rec_aac));
}

static unsigned rec_channels_locked(void) { return E.rec_channels == 2 ? 2 : 1; }
static unsigned channel_mode_locked(void) { return E.channel_mode >= 1 && E.channel_mode <= 3 ? E.channel_mode : 2; }
static unsigned rec_channel_mode_locked(void) { return E.rec_channel_mode >= 1 && E.rec_channel_mode <= 3 ? E.rec_channel_mode : 2; }
static int rec_uses_slot_locked(unsigned slot) { return E.rec_channels == 2 || E.rec_slot == slot; }
static uint64_t rec_frames_locked(void)
{
    return E.rec_aac ? mic_aac_snapshot(E.rec_aac).submitted : E.rec_bytes / (2 * rec_channels_locked());
}

/* Stereo callback counts sample frames, never interleaved scalar values. */
static void route_stereo(int16_t *out, const int16_t *in, unsigned frames, unsigned mode)
{
    for (unsigned i = 0; i < frames; i++) {
        int32_t l = in[2*i], r = in[2*i+1];
        out[2*i] = (int16_t)(mode == 1 ? (l+r)/2 : mode == 3 ? r : l);
        out[2*i+1] = (int16_t)(mode == 1 ? (l+r)/2 : mode == 3 ? l : r);
    }
}

static int rec_stereo_output(void *unused, const int16_t *pcm, unsigned frames, unsigned real_mask)
{
    (void)unused;
    if (E.rec_error) return -1;
    int16_t routed[MIC_STEREO_SAMPLES * 2];
    if (frames > MIC_STEREO_SAMPLES) { E.rec_error = 1; return -1; }
    unsigned mode = rec_channel_mode_locked();
    if (mode != 2) {
        route_stereo(routed, pcm, frames, mode);
        pcm = routed;
    }
    if (E.rec_aac) {
        mic_pcm_origin origins[2] = {0};
        /* The assembler marks any own-history PCM phase merge explicitly. */
        memcpy(origins, record_stereo.output_origins, sizeof origins);
        if (mic_aac_write_observed(E.rec_aac, pcm, (int)frames, real_mask == 0, real_mask, origins) < 0)
            E.rec_error = 1;
    } else {
        size_t written = fwrite(pcm, 4, frames, E.rec_f);
        E.rec_bytes += written * 4;
        if (written != frames) E.rec_error = 1;
    }
    if (real_mask) E.rec_real_samples += frames; else E.rec_silent_samples += frames;
    return E.rec_error ? -1 : 0;
}

static void rec_stereo_drain_locked(uint64_t now, int force)
{
    if (E.rec_deadline_ms && now > E.rec_deadline_ms) now = E.rec_deadline_ms;
    if (now < E.rec_start_ms || E.rec_channels != 2 || !rec_accepting_locked()) return;
    unsigned active = 0;
    pthread_mutex_lock(&E.st_mtx);
    for (unsigned slot = 0; slot < 2; slot++)
        if (TX[slot].connected && !TX[slot].stream_lost && !TX[slot].user_hold)
            active |= 1u << slot;
    pthread_mutex_unlock(&E.st_mtx);
    mic_stereo_set_active(&record_stereo, active);
    int emitted;
    do {
        emitted = force ? mic_stereo_drain_samples(&record_stereo, now,
            (now-E.rec_start_ms)*48, rec_stereo_output, NULL) :
            mic_stereo_drain_before(&record_stereo, now, 0,
                (now-E.rec_start_ms)/5, rec_stereo_output, NULL);
        if (emitted < 0) E.rec_error = 1;
    } while (force && emitted > 0);
}

static void rec_fill_to_locked(uint64_t target)
{
    unsigned channels = rec_channels_locked();
    if (E.rec_aac) mic_aac_pad(E.rec_aac, target);
    static const int16_t zero[4800] = {0};
    while (!E.rec_aac && !E.rec_error && rec_frames_locked() < target) {
        size_t n = target - rec_frames_locked();
        if (n > 2400) n = 2400;
        size_t written = fwrite(zero, 2 * channels, n, E.rec_f);
        E.rec_bytes += written * 2 * channels; E.rec_silent_samples += written;
        if (written != n) E.rec_error = 1;
    }
}

static void rec_pad_locked(uint64_t now, int tail)
{
    if (E.rec_deadline_ms && now > E.rec_deadline_ms) now = E.rec_deadline_ms;
    if (!rec_accepting_locked() || (!E.rec_owner[0] && E.rec_channels != 2) || now < E.rec_start_ms) return;
    unsigned channels = rec_channels_locked();
    rec_stereo_drain_locked(now, tail);
    uint64_t elapsed = now - E.rec_start_ms;
    unsigned reserve = tail ? 0 : channels == 2 ? MIC_REC_SILENCE_GRACE_MS : MIC_REC_MONO_GRACE_MS;
    uint64_t target = (elapsed > reserve ? elapsed - reserve : 0) * 48;
    if (channels == 2 && !tail) target = target / 240 * 240;
    /* A bounded drain can leave real frames after a large hole. A later tick
     * will consume them; wall-clock padding must not erase queued audio. */
    if (channels == 2 && !tail && (record_stereo.channel[0].count || record_stereo.channel[1].count) &&
            target > record_stereo.next_index * 240) target = record_stereo.next_index * 240;
    uint64_t before = rec_frames_locked();
    rec_fill_to_locked(target);
    if (channels == 2) {
        uint64_t after = rec_frames_locked();
        for (unsigned slot = 0; slot < 2; slot++) record_stereo.channel[slot].silent_samples += after - before;
        mic_stereo_advance(&record_stereo, after / 240);
    }
}

/* rec_mtx held, addressed producer only. Return a prefix already committed as
 * PCM/silence. Missing metadata has an explicitly uncalibrated sequential
 * fallback; an observed offline transition reanchors only that fallback.
 * Real producers carry per-TX generation + sample provenance from timing_pcm. */
static unsigned rec_mono_place_locked(const mic_pcm_origin *origin, unsigned n,
                                      uint64_t now, int online)
{
    uint64_t elapsed = now >= E.rec_start_ms ? (now-E.rec_start_ms)*48 : 0;
    uint64_t cursor = rec_frames_locked(), position, prefix = 0;
    /* Only provenance from the already selected source generation may fill a
     * stopped take. A reconnect or unqualified producer cannot extend it. */
    if (record_mono.draining && (!origin || origin->generation != record_mono.generation ||
            timing_now_ns() >= record_mono.stop_until_ns)) return n;
    if (origin) {
        if (!origin->event_id || origin->samples != n || origin->slot != E.rec_slot ||
                !origin->generation || memcmp(origin->bd,E.rec_target,6)) {
            E.rec_error = 1; return n;
        }
        record_mono.have_origin = 1;
        if (origin->generation < record_mono.generation) {
            record_mono.late_samples += n; return n;
        }
        if (origin->generation != record_mono.generation) {
            record_mono.generation = origin->generation;
            record_mono.source_base = origin->first_sample;
            record_mono.output_base = elapsed > n ? elapsed-n : 0;
            if (record_mono.output_base < cursor) record_mono.output_base = cursor;
            record_mono.reanchors++;
        }
        uint64_t first = origin->first_sample;
        if (first < record_mono.source_base) {
            prefix = record_mono.source_base-first;
            if (prefix >= n) { record_mono.late_samples += n; return n; }
            first += prefix;
        }
        if (first-record_mono.source_base > UINT64_MAX-record_mono.output_base) {
            E.rec_error = 1; return n;
        }
        position = record_mono.output_base + first-record_mono.source_base;
    } else {
        if (!online || !record_mono.fallback_online) {
            record_mono.fallback_next = elapsed > n ? elapsed-n : 0;
            if (record_mono.fallback_next < cursor) record_mono.fallback_next = cursor;
        }
        record_mono.fallback_online = online;
        position = record_mono.fallback_next;
        record_mono.fallback_next += n;
    }
    if (record_mono.draining && position >= record_mono.stop_target) return n;
    /* Do not turn malformed source positions into unbounded file padding. */
    if (position > elapsed + MIC_REC_MONO_GRACE_MS*48u) { E.rec_error = 1; return n; }
    if (position < cursor) {
        uint64_t late = cursor-position;
        prefix += late < n-prefix ? late : n-prefix;
    } else if (position > cursor) rec_fill_to_locked(position);
    record_mono.late_samples += prefix;
    return (unsigned)prefix;
}

/* Seed only the contiguous suffix immediately before this slot's first live
 * block. Never retimestamp the live clock, rewind output, or retry on reconnect. */
static void rec_preroll_locked(unsigned slot, const mic_pcm_origin *origin,
                              uint64_t stamp, uint64_t index, int eligible)
{
    struct mic_preroll *h=&record_preroll[slot];
    unsigned count=h->count;h->count=0;
    if (!count || !eligible || !origin || record_clock.peer[slot].rebased ||
            origin->receive_ns/1000000<E.rec_start_ms ||
            origin->receive_ns/1000000-E.rec_start_ms>MIC_PREROLL_AGE_MS ||
            origin->generation!=E.rec_timing_generations[slot] ||
            !mic_preroll_follows(mic_preroll_at(h,count-1),origin,stamp)) return;
    for (unsigned i=0;i<count;i++) {
        const struct mic_preroll_frame *f=mic_preroll_at(h,i);
        unsigned distance=count-i;
        uint64_t received=f->origin.receive_ns/1000000;
        if (distance>index || index-distance<record_stereo.next_index ||
                received>E.rec_start_ms || E.rec_start_ms-received>MIC_PREROLL_AGE_MS ||
                f->origin.event_id>E.rec_timing_first ||
                memcmp(f->origin.bd,E.rec_targets[slot],6)) continue;
        uint64_t delta=(stamp+MIC_BT_PERIOD_US-f->stamp)%MIC_BT_PERIOD_US;
        int64_t old_stamp=record_clock.peer[slot].stamp_us-(int64_t)delta;
        if (!mic_stereo_push_timestamp(&record_stereo,slot,index-distance,received,
                1,old_stamp,f->pcm,1,&f->origin)) {
            record_preroll_frames[slot]++;
            if (!record_preroll_first[slot]) record_preroll_first[slot]=f->origin.event_id;
        }
    }
}

static void rec_write_observed(struct mic_tx *tx, const int16_t *s, int n, const mic_pcm_origin *origin)
{
    if (n <= 0) return;
    pthread_mutex_lock(&E.rec_mtx);
    unsigned slot=(unsigned)(tx-TX);
    uint64_t now=now_ms();
    const struct mic_timing_event *event=&TS[slot].context;
    pthread_mutex_lock(&E.st_mtx);
    int muted = !!(tx->tx_flags & 1);
    int real=tx->connected && !tx->stream_lost && !tx->user_hold &&
        tx->tx_status_ms && now>=tx->tx_status_ms && now-tx->tx_status_ms<10000 && !(tx->tx_flags&1);
    pthread_mutex_unlock(&E.st_mtx);
    mic_pcm_origin policy_origin;
    if (origin) { policy_origin = *origin; policy_origin.muted = muted; origin = &policy_origin; }
    int history_eligible=real && !pcm_concealed && n==240 && origin && origin->slot==slot &&
        origin->event_id && origin->samples==240 && !origin->synthetic && !origin->merged &&
        origin->receive_ns && origin->receive_ns==event->receive_ns &&
        origin->source==4 && origin->generation==TS[slot].generation &&
        !memcmp(origin->bd,tx->cfg_bd,6) && event->clock_valid && !event->synthetic &&
        event->btclk<(1u<<28) && event->intra<1250;
    uint64_t stamp=((uint64_t)event->btclk*625/2+event->intra)%MIC_BT_PERIOD_US;
    mic_preroll_store(&recent_pcm[slot],s,origin,stamp,history_eligible);
    if (!history_eligible) record_preroll[slot].count=0;
    if (E.rec_channels == 2) {
        if (rec_accepting_locked() && n == 240 &&
                !memcmp(E.rec_targets[slot], tx->cfg_bd, 6)) {
            uint64_t index;
            uint64_t received = event->receive_ns ? event->receive_ns / 1000000 : now;
            if (record_tail.draining) {
                /* Waiting must not reanchor a stopped take or accept unrelated
                 * sources. Probe the clock on a copy before changing its state. */
                struct mic_stereo_clock clock = record_clock;
                const struct mic_stereo_channel *channel = &record_stereo.channel[slot];
                const mic_pcm_origin *previous = &channel->last_origin;
                uint64_t source_index;
                int eligible = (record_tail.wait_mask & (1u << slot)) && real && !pcm_concealed &&
                    channel->have_stamp && !channel->sync_pending && !channel->suspended && !channel->pending_count &&
                    origin && origin->event_id && origin->samples == 240 && origin->slot == slot &&
                    origin->source == 4 && !origin->synthetic && !origin->merged &&
                    origin->generation == record_tail.generations[slot] &&
                    TS[slot].generation == record_tail.generations[slot] &&
                    !memcmp(origin->bd,E.rec_targets[slot],6) &&
                    previous->event_id && origin->event_id > previous->event_id &&
                    origin->first_sample > previous->first_sample &&
                    event->clock_valid && !event->synthetic &&
                    origin->receive_ns && origin->receive_ns == event->receive_ns &&
                    timing_now_ns() < record_tail.until_ns;
                if (eligible && mic_stereo_clock_index(&clock,slot,origin->generation,received,
                        1,event->btclk,event->intra,&index) == 0 &&
                        clock.peer[slot].stamp_us > channel->last_stamp_us &&
                        mic_stereo_distance(clock.peer[slot].stamp_us,channel->last_stamp_us) <=
                            MIC_STEREO_MAX_AHEAD * UINT64_C(5000) &&
                        mic_stereo_source_index(channel,slot,origin,&source_index) &&
                        source_index < (record_tail.target+239)/240) {
                    /* The paired writer may have relocated away from the raw
                     * grid. Bound its actual source position, as normal input
                     * does, without changing the fixed stop endpoint. */
                    record_clock = clock;
                    if (!mic_stereo_push_timestamp(&record_stereo,slot,index,received,1,
                            clock.peer[slot].stamp_us,s,1,origin)) record_tail.received_frames[slot]++;
                    rec_stereo_drain_locked(now,0);
                }
                pthread_cond_broadcast(&rec_tail_changed);
                pthread_mutex_unlock(&E.rec_mtx);
                return;
            }
            /* Rejected PLC is not a clock/tick event. In particular a held
             * peer must not make us drain its next real position early. */
            if (pcm_concealed && (pcm_concealed != 1 || !real)) {
                pthread_mutex_unlock(&E.rec_mtx);
                return;
            }
            rec_pad_locked(now, 0);
            if (record_clock.peer[slot].seen && record_clock.peer[slot].generation != TS[slot].generation)
                mic_stereo_reset_channel(&record_stereo, slot, 1);
            if (pcm_concealed) {
                mic_stereo_push_concealed(&record_stereo, slot, received, s, origin);
                rec_stereo_drain_locked(now, 0);
                pthread_mutex_unlock(&E.rec_mtx);
                return;
            }
            int mapped = mic_stereo_clock_index(&record_clock, slot, TS[slot].generation, received,
                    event->clock_valid && !event->synthetic, event->btclk, event->intra, &index);
            if (mapped >= 0) {
                rec_preroll_locked(slot,origin,stamp,index,history_eligible && mapped==0);
                mic_stereo_push_timestamp(&record_stereo, slot, index, received,
                    mapped == 0, record_clock.peer[slot].stamp_us, s, real, origin);
            } else record_preroll[slot].count=0;
            rec_stereo_drain_locked(now, 0);
        }
        pthread_mutex_unlock(&E.rec_mtx);
        return;
    }
    if (memcmp(E.rec_target, tx->cfg_bd, 6)) {
        pthread_mutex_unlock(&E.rec_mtx);
        return; /* an unrelated TX cannot advance this owner's mono timeline */
    }
    if (rec_accepting_locked()) {
        int silent = 0;
        mic_pcm_origin adjusted;
        if (E.rec_owner[0]) {
            uint64_t now = now_ms();
            rec_pad_locked(now, 0);
            pthread_mutex_lock(&E.st_mtx);
            int online = tx->connected && !tx->stream_lost && !tx->user_hold;
            silent = pcm_concealed || !tx->connected || tx->stream_lost ||
                memcmp(E.rec_target, tx->cfg_bd, 6) || !tx->tx_status_ms ||
                now_ms() - tx->tx_status_ms >= 10000 || (tx->tx_flags & 1);
            pthread_mutex_unlock(&E.st_mtx);
            unsigned skip = rec_mono_place_locked(origin, (unsigned)n, now, online);
            if (E.rec_error || skip >= (unsigned)n) { pthread_mutex_unlock(&E.rec_mtx); return; }
            s += skip; n -= (int)skip;
            if (record_mono.draining) {
                uint64_t remaining = record_mono.stop_target - rec_frames_locked();
                if ((uint64_t)n > remaining) n = (int)remaining;
                if (!n) { pthread_mutex_unlock(&E.rec_mtx); return; }
            }
            if (origin && (skip || origin->samples != (unsigned)n)) {
                adjusted = *origin; adjusted.first_sample += skip; adjusted.samples = (unsigned)n;
                origin = &adjusted;
            }
        }
        if (E.rec_aac) {
            mic_pcm_origin origins[2] = {0};
            unsigned slot = (unsigned)(tx - TX);
            if (origin) origins[slot] = *origin;
            int queued = mic_aac_write_observed(E.rec_aac, s, n, silent, 1u << slot, origins);
            if (queued < 0) E.rec_error = 1;
            if (record_mono.draining) {
                if (queued > 0) record_mono.tail_received += (unsigned)n;
                pthread_cond_broadcast(&rec_tail_changed);
            }
            pthread_mutex_unlock(&E.rec_mtx);
            return;
        }
        int16_t zero[2400] = {0};
        if (silent && n > 2400) n = 2400;
        size_t written = fwrite(silent ? zero : s, 2, (size_t)n, E.rec_f);
        E.rec_bytes += (uint64_t)written * 2;
        if (silent) E.rec_silent_samples += written; else E.rec_real_samples += written;
        if (written != (size_t)n) E.rec_error = 1;
        if (record_mono.draining) {
            record_mono.tail_received += written;
            pthread_cond_broadcast(&rec_tail_changed);
        }
    }
    pthread_mutex_unlock(&E.rec_mtx);
}

/* Tests/legacy internal producers without decoder provenance leave it unknown. */
static void rec_write(struct mic_tx *tx, const int16_t *s, int n)
{
    rec_write_observed(tx, s, n, NULL);
}

static void wav_header(FILE *f, uint32_t bytes, unsigned channels)
{
    uint8_t h[44];
    memset(h, 0, 44);
    memcpy(h, "RIFF", 4);
    *(uint32_t *)(h + 4) = 36 + bytes;
    memcpy(h + 8, "WAVEfmt ", 8);
    *(uint32_t *)(h + 16) = 16;
    *(uint16_t *)(h + 20) = 1;         /* PCM */
    *(uint16_t *)(h + 22) = (uint16_t)channels;
    *(uint32_t *)(h + 24) = SRATE;
    *(uint32_t *)(h + 28) = SRATE * 2 * channels;
    *(uint16_t *)(h + 32) = (uint16_t)(2 * channels);
    *(uint16_t *)(h + 34) = 16;
    memcpy(h + 36, "data", 4);
    *(uint32_t *)(h + 40) = bytes;
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
}

static int tx_format_active(struct mic_tx *tx); /* st_mtx held; defined with TX controls below */

/* Prepare storage without feed_mtx or rec_mtx: an SSD stall must not stop
 * either decoder, the live FIFO, or the Bluetooth callback. Nothing in E is
 * published until rec_start_prepared validates and installs these resources. */
struct rec_prepared {
    FILE *file;
    mic_aac *aac, *previous_aac;
    char path[256];
    struct stat aac_identity;
    int aac_identity_valid;
};

static void rec_discard_prepared(struct rec_prepared *p)
{
    if (p->file) {
        int present = mic_record_file_present(p->file, p->path);
        fclose(p->file);
        if (present) unlink(p->path);
    }
    if (p->aac) {
        mic_aac_free(p->aac);
        struct stat st;
        if (p->aac_identity_valid && !lstat(p->path, &st) &&
                st.st_dev == p->aac_identity.st_dev && st.st_ino == p->aac_identity.st_ino)
            unlink(p->path);
    }
    mic_aac_free(p->previous_aac);
    memset(p, 0, sizeof *p);
}

static int rec_prepare(struct rec_prepared *p, const char *path, int aac, unsigned channels)
{
    static const uint8_t zero44[44] = {0};
    memset(p, 0, sizeof *p);
    if (channels != 1 && channels != 2) return -1;
    if (!path || !path[0]) {
        const char *rdir = "/mnt/media_rw/ssd/mic";
        mkdir(rdir, 0777);
        snprintf(p->path, sizeof p->path, "%s/rec_%llu.wav", rdir,
                 (unsigned long long)now_epoch_ms());
    } else {
        if (strlen(path) >= sizeof p->path) return -1;
        snprintf(p->path, sizeof p->path, "%s", path);
    }
    if (aac) {
        p->aac = channels == 2 ? mic_aac_start_channels(p->path, 2) : mic_aac_start(p->path);
        if (!p->aac) return -1;
        p->aac_identity_valid = !fstat(fileno(p->aac->file), &p->aac_identity);
    } else {
        /* Never truncate an existing take, even for two starts in one tick. */
        int fd = open(p->path, O_WRONLY | O_CREAT | O_EXCL, 0664);
        p->file = fd >= 0 ? fdopen(fd, "wb") : NULL;
        if (!p->file) {
            if (fd >= 0) { close(fd); unlink(p->path); }
            mlog("rec_start: fopen %s: %s", p->path, strerror(errno));
            return -1;
        }
        if (fwrite(zero44, 1, 44, p->file) != 44 || fflush(p->file) != 0) {
            rec_discard_prepared(p);
            return -1;
        }
    }
    return 0;
}

/* rec_mtx held. Keep rejection precedence before any storage side effects,
 * then repeat this check when committing a prepared recording. */
static int rec_start_check(struct mic_tx *tx, unsigned channels)
{
    if (rec_active_locked() || atomic_load(&engine_stopping)) return -1;
    pthread_mutex_lock(&E.st_mtx);
    int formatting = channels == 2 ? tx_format_active(&TX[0]) || tx_format_active(&TX[1]) : tx_format_active(tx);
    pthread_mutex_unlock(&E.st_mtx);
    return formatting ? -4 : 0;
}

/* Production callers hold feed_mtx only for this state transition. */
static int rec_start_prepared(struct mic_tx *tx, int sec, unsigned channels,
                              unsigned mode, struct rec_prepared *p)
{
    if ((channels != 1 && channels != 2) || mode > 3 || (!p->file && !p->aac)) return -1;
    pthread_mutex_lock(&E.rec_mtx);
    int rc = rec_start_check(tx, channels);
    if (rc) { pthread_mutex_unlock(&E.rec_mtx); return rc; }
    p->previous_aac = E.rec_aac;
    E.rec_aac = p->aac; p->aac = NULL;
    E.rec_f = p->file; p->file = NULL;
    snprintf(E.rec_path, sizeof E.rec_path, "%s", p->path);
    E.rec_bytes = 0;
    E.rec_owner[0] = 0;
    E.rec_slot = (unsigned)(tx - TX);
    E.rec_channels = channels;
    E.rec_channel_mode = channels == 1 ? 2 : mode ? mode : channel_mode_locked();
    for (unsigned slot = 0; slot < 2; slot++) memcpy(E.rec_targets[slot], TX[slot].cfg_bd, 6);
    memcpy(E.rec_target, tx->cfg_bd, 6);
    E.rec_start_ms = now_ms();
    mic_stereo_init(&record_stereo, 3);
    mic_stereo_advance(&record_stereo, 0);
    mic_stereo_clock_init(&record_clock, E.rec_start_ms);
    memset(record_preroll_frames,0,sizeof record_preroll_frames);
    memset(record_preroll_first,0,sizeof record_preroll_first);
    memset(&record_tail,0,sizeof record_tail);
    for (unsigned slot=0;slot<2;slot++) {
        record_preroll[slot]=recent_pcm[slot];
        if (channels!=2) record_preroll[slot].count=0;
        /* A new take cannot replay source observations covered by the prior
         * closed take, even if two commands arrive within the cache window. */
        while (record_preroll[slot].count &&
                (mic_preroll_at(&record_preroll[slot],0)->origin.event_id<=E.rec_timing_last ||
                 mic_preroll_at(&record_preroll[slot],0)->origin.receive_ns/1000000>E.rec_start_ms ||
                 E.rec_start_ms-mic_preroll_at(&record_preroll[slot],0)->origin.receive_ns/1000000>MIC_PREROLL_AGE_MS)) {
            record_preroll[slot].head=(record_preroll[slot].head+1)%MIC_PREROLL_BLOCKS;
            record_preroll[slot].count--;
        }
    }
    E.rec_start_ns = timing_now_ns(); /* recorder command boundary, NOT ADC sample zero */
    pthread_mutex_lock(&MT.mutex);
    E.rec_timing_generation = TS[E.rec_slot].generation;
    memset(&record_mono,0,sizeof record_mono);
    record_mono.generation = TS[E.rec_slot].generation;
    record_mono.source_base = TS[E.rec_slot].next_sample;
    record_mono.fallback_online = 1;
    for (unsigned slot = 0; slot < 2; slot++) {
        E.rec_timing_generations[slot] = TS[slot].generation;
        E.rec_timing_end_generations[slot] = 0;
    }
    E.rec_timing_first = MT.serial;
    E.rec_history_after=E.rec_timing_first;
    for (unsigned slot=0;slot<2;slot++) if(record_preroll[slot].count) {
        uint64_t first=mic_preroll_at(&record_preroll[slot],0)->origin.event_id;
        if(first && first-1<E.rec_history_after) E.rec_history_after=first-1;
    }
    pthread_mutex_unlock(&MT.mutex);
    E.rec_timing_last = E.rec_timing_end_generation = 0;
    E.rec_timing_closed = 0;
    E.rec_real_samples = E.rec_silent_samples = 0;
    E.rec_completed = E.rec_error = 0;
    E.rec_deadline_ms = sec > 0 ? now_ms() + (uint64_t)sec * 1000 : 0;
    pthread_mutex_unlock(&E.rec_mtx);
    return 0;
}

static int rec_start_format_mode(struct mic_tx *tx, int sec, const char *path, int aac, unsigned channels, unsigned mode)
{
    if ((channels != 1 && channels != 2) || mode > 3) return -1;
    pthread_mutex_lock(&E.rec_mtx);
    int rc = rec_start_check(tx, channels);
    pthread_mutex_unlock(&E.rec_mtx);
    if (rc) return rc;
    struct rec_prepared p;
    if (rec_prepare(&p, path, aac, channels)) return -1;
    rc = rec_start_prepared(tx, sec, channels, mode, &p);
    rec_discard_prepared(&p);
    return rc;
}

static int rec_start_format_channels(struct mic_tx *tx, int sec, const char *path, int aac, unsigned channels)
{
    /* Existing owned callers retain normal L/R unless they explicitly opt in. */
    return rec_start_format_mode(tx, sec, path, aac, channels, 2);
}

static int rec_start_format_tx(struct mic_tx *tx, int sec, const char *path, int aac)
{
    return rec_start_format_channels(tx, sec, path, aac, 1);
}

/* st_mtx held: the legacy GUI automatically chooses stereo or the sole TX. */
static struct mic_tx *rec_default_target(unsigned *channels)
{
    *channels = TX[0].connected && TX[1].connected && TX[0].cfg_bd_valid && TX[1].cfg_bd_valid ? 2 : 1;
    return TX[0].connected ? &TX[0] : TX[1].connected ? &TX[1] :
        TX[1].cfg_bd_valid && !TX[0].cfg_bd_valid ? &TX[1] : &TX[0];
}

int miccore_rec_start(int sec, const char *path)
{
    uint8_t targets[2][6];
    unsigned channels, current_channels;
    int connected[2];
    pthread_mutex_lock(&E.feed_mtx);
    pthread_mutex_lock(&E.st_mtx);
    struct mic_tx *tx = rec_default_target(&channels);
    for (unsigned slot = 0; slot < 2; slot++) {
        memcpy(targets[slot], TX[slot].cfg_bd, 6);
        connected[slot] = TX[slot].connected;
    }
    pthread_mutex_unlock(&E.st_mtx);
    pthread_mutex_lock(&E.rec_mtx);
    int checked = rec_start_check(tx, channels);
    pthread_mutex_unlock(&E.rec_mtx);
    pthread_mutex_unlock(&E.feed_mtx);
    if (checked) return checked;
    struct rec_prepared p;
    if (rec_prepare(&p, path, 0, channels)) return -1;
    pthread_mutex_lock(&E.feed_mtx);
    pthread_mutex_lock(&E.st_mtx);
    int same = rec_default_target(&current_channels) == tx && current_channels == channels &&
        !memcmp(targets[0], TX[0].cfg_bd, 6) && !memcmp(targets[1], TX[1].cfg_bd, 6) &&
        connected[0] == TX[0].connected && connected[1] == TX[1].connected;
    pthread_mutex_unlock(&E.st_mtx);
    int result = same ? rec_start_prepared(tx, sec, channels, 0, &p) : -1;
    pthread_mutex_unlock(&E.feed_mtx);
    if (!result) mlog("rec_start: %s sec=%d", p.path, sec);
    rec_discard_prepared(&p);
    return result;
}

/* A stop boundary is fixed before waiting: delayed source positions may fill
 * its suffix, but neither the wait nor a new source generation extends it.
 * Shutdown must not wait for feed callbacks while it owns feed_mtx. */
static void rec_mono_wait_tail_locked(void)
{
    if (!E.rec_owner[0] || E.rec_channels != 1 || !record_mono.have_origin ||
            !rec_accepting_locked() || E.rec_error || atomic_load(&engine_stopping)) return;
    uint64_t end = now_ms();
    if (E.rec_deadline_ms && end > E.rec_deadline_ms) end = E.rec_deadline_ms;
    uint64_t target = end > E.rec_start_ms ? (end-E.rec_start_ms)*48 : 0;
    if (target <= rec_frames_locked()) return;
    E.rec_deadline_ms = end;
    record_mono.stop_target = target;
    pthread_once(&rec_tail_once, rec_tail_init);
    struct timespec until;
    if (rec_tail_cond_rc || clock_gettime(CLOCK_MONOTONIC, &until)) { E.rec_error = 1; return; }
    record_mono.stop_until_ns = (uint64_t)until.tv_sec*1000000000 + until.tv_nsec +
        (uint64_t)MIC_REC_MONO_GRACE_MS*1000000;
    until.tv_sec = record_mono.stop_until_ns/1000000000;
    until.tv_nsec = record_mono.stop_until_ns%1000000000;
    record_mono.draining = 1;
    while (rec_frames_locked() < target && !E.rec_error && !atomic_load(&engine_stopping)) {
        int rc = pthread_cond_timedwait(&rec_tail_changed, &E.rec_mtx, &until);
        if (rc == ETIMEDOUT) { record_mono.tail_timed_out = 1; break; }
        if (rc) { E.rec_error = 1; break; }
    }
    record_mono.draining = 0;
    pthread_cond_broadcast(&rec_finalized);
}

/* A proven source hole cannot be filled by a later ordered callback. Wait only
 * for missing positions not yet crossed by an eligible active writer. */
static int rec_stereo_tail_pending_locked(void)
{
    unsigned active = 0;
    pthread_mutex_lock(&E.st_mtx);
    for (unsigned slot=0;slot<2;slot++)
        if (TX[slot].connected && !TX[slot].stream_lost && !TX[slot].user_hold && !(TX[slot].tx_flags&1))
            active |= 1u << slot;
    pthread_mutex_unlock(&E.st_mtx);
    uint64_t limit = (record_tail.target+239)/240;
    for (unsigned slot=0;slot<2;slot++) if (record_tail.wait_mask & active & (1u<<slot)) {
        const struct mic_stereo_channel *c = &record_stereo.channel[slot];
        uint64_t start = record_stereo.next_index;
        if (c->have_index && c->write_index > start) start = c->write_index;
        for (uint64_t index=start;index<limit;index++) {
            if (mic_stereo_at(&record_stereo,slot,index)) continue;
            return 1;
        }
    }
    return 0;
}

static void rec_stereo_wait_tail_locked(void)
{
    if (E.rec_channels != 2 || !rec_accepting_locked() || E.rec_error || E.rec_timing_closed) return;
    uint64_t end = now_ms();
    if (E.rec_deadline_ms && end > E.rec_deadline_ms) end = E.rec_deadline_ms;
    record_tail.target = end > E.rec_start_ms ? (end-E.rec_start_ms)*48 : 0;
    E.rec_deadline_ms = end;
    rec_stereo_drain_locked(end,0);
    if (rec_frames_locked() >= record_tail.target || atomic_load(&engine_stopping)) return;
    for (unsigned slot=0;slot<2;slot++) {
        const struct mic_stereo_clock_peer *p = &record_clock.peer[slot];
        const struct mic_stereo_channel *c = &record_stereo.channel[slot];
        const mic_pcm_origin *origin = &c->last_origin;
        record_tail.generations[slot] = p->generation;
        if (p->seen && p->raw_valid && !p->rebased && !c->sync_pending && !c->suspended &&
                origin->event_id && origin->source==4 && origin->slot==slot &&
                !origin->synthetic && !origin->merged && origin->generation==p->generation &&
                !memcmp(origin->bd,E.rec_targets[slot],6)) record_tail.wait_mask |= 1u<<slot;
    }
    if (!rec_stereo_tail_pending_locked()) return;
    pthread_once(&rec_tail_once,rec_tail_init);
    struct timespec until;
    if (rec_tail_cond_rc || clock_gettime(CLOCK_MONOTONIC,&until)) { E.rec_error=1; return; }
    record_tail.until_ns = (uint64_t)until.tv_sec*1000000000+until.tv_nsec+
        (uint64_t)MIC_STEREO_WAIT_MS*1000000;
    until.tv_sec=record_tail.until_ns/1000000000;
    until.tv_nsec=record_tail.until_ns%1000000000;
    record_tail.draining=1;
    while (rec_stereo_tail_pending_locked() && !E.rec_error && !atomic_load(&engine_stopping)) {
        int rc=pthread_cond_timedwait(&rec_tail_changed,&E.rec_mtx,&until);
        if (rc==ETIMEDOUT) { record_tail.timed_out=1; break; }
        if (rc) { E.rec_error=1; break; }
    }
    record_tail.draining=0;
    pthread_cond_broadcast(&rec_finalized);
}

/* Caller owns rec_mtx on entry and return. WAV finalization temporarily drops
 * it after detaching the sink: neither TX may wait behind SSD flush/fsync.
 * rec_finalizing retains the take and ownership until the result is published. */
static int rec_stop_locked(char *path_out, int path_sz, uint64_t *bytes_out)
{
    FILE *f;
    if (E.rec_finalizing || record_mono.draining || record_tail.draining) return 1;
    rec_mono_wait_tail_locked();
    rec_stereo_wait_tail_locked();
    if (!E.rec_timing_closed && (E.rec_f || E.rec_aac)) {
        pthread_mutex_lock(&MT.mutex);
        E.rec_timing_last = MT.serial;
        E.rec_timing_end_generation = TS[E.rec_slot].generation;
        for (unsigned slot = 0; slot < 2; slot++) E.rec_timing_end_generations[slot] = TS[slot].generation;
        pthread_mutex_unlock(&MT.mutex);
        E.rec_timing_closed = 1;
    }
    if (E.rec_aac) {
        rec_pad_locked(now_ms(), 1);
        mic_aac_finish(E.rec_aac);
        mic_aac_stats s = mic_aac_snapshot(E.rec_aac);
        if (path_out) snprintf(path_out, path_sz, "%s", E.rec_path);
        if (bytes_out) *bytes_out = s.bytes;
        return !s.completed ? 1 : s.error || E.rec_error ? -2 : 0;
    }
    if (!E.rec_f) {
        if (!E.rec_completed) return -1;
        if (path_out) snprintf(path_out, path_sz, "%s", E.rec_path);
        if (bytes_out) *bytes_out = E.rec_bytes;
        return E.rec_error ? -2 : 0;
    }
    rec_pad_locked(now_ms(), 1);
    f = E.rec_f;
    E.rec_f = NULL;
    E.rec_finalizing = 1;
    E.rec_deadline_ms = 0;
    uint64_t bytes = E.rec_bytes;
    unsigned channels = rec_channels_locked();
    char path[sizeof E.rec_path];
    snprintf(path, sizeof path, "%s", E.rec_path);
    pthread_mutex_unlock(&E.rec_mtx);
    int error = 0;
    struct stat identity, current;
    int identity_valid = !fstat(fileno(f), &identity);
    if (fflush(f) != 0) error = 1;
    wav_header(f, (uint32_t)bytes, channels);
    if (fflush(f) != 0 || fsync(fileno(f)) != 0) error = 1;
    if (ferror(f)) error = 1;
    if (fclose(f) != 0) error = 1;
    if (!identity_valid || lstat(path, &current) || !S_ISREG(current.st_mode) ||
            identity.st_dev != current.st_dev || identity.st_ino != current.st_ino) {
        error = 1;
        mlog("rec_stop: output path missing or replaced: %s", path);
    }
    mlog("rec_stop: %s %llu bytes", path, (unsigned long long)bytes);
    pthread_mutex_lock(&E.rec_mtx);
    E.rec_error |= error;
    E.rec_finalizing = 0;
    E.rec_completed = 1;
    pthread_cond_broadcast(&rec_finalized);
    if (path_out) snprintf(path_out, path_sz, "%s", E.rec_path);
    if (bytes_out) *bytes_out = E.rec_bytes;
    return E.rec_error ? -2 : 0;
}

int miccore_rec_stop(char *path_out, int path_sz, uint64_t *bytes_out)
{
    int rc;
    pthread_mutex_lock(&E.rec_mtx);
    rc = rec_stop_locked(path_out, path_sz, bytes_out);
    pthread_mutex_unlock(&E.rec_mtx);
    return rc;
}

static void rec_check_deadline(uint64_t now)
{
    pthread_mutex_lock(&E.rec_mtx);
    rec_pad_locked(now, 0);
    if (rec_active_locked() && (E.rec_error || (E.rec_deadline_ms && now >= E.rec_deadline_ms)))
        rec_stop_locked(NULL, 0, NULL);
    pthread_mutex_unlock(&E.rec_mtx);
}

#include "mic_pcm_stereo.h"

/* push one decoded frame to all sinks; n = samples */
static void pcm_dispatch(struct mic_tx *tx, const int16_t *s, int n)
{
    mic_pcm_origin origin = {0};
    if (n > 0) origin = timing_pcm_origin(timing_pcm((unsigned)(tx - TX), (unsigned)n, pcm_concealed));
    double sum = 0;
    int i;
    for (i = 0; i < n; i++) sum += (double)s[i] * s[i];
    if (n > 0) tx->level = sqrt(sum / n) / 32768.0;
    if (tx == &TX[0] || !TX[0].connected) fifo_write(s, n);
    pcm_stereo_feed(tx, s, n, &origin);
    rec_write_observed(tx, s, n, &origin);
}

/* fill for dropped/stalled frames: ETSI PLC concealment when available
 * (coordinated with the product line 2026-10-02), plain silence otherwise.
 * Either way the 5ms time grid and the drops counter are unchanged. */
static void pcm_fill(struct mic_tx *tx, int frames)
{
    static const int16_t z[NSAMP] = {0};
    int i;
    pcm_concealed = 1;
    for (i = 0; i < frames; i++) {
        tx->drops++;
        if (decoder_conceal(&decoders[tx - TX]) == 0) {
            pcm_concealed = 1;
            pcm_dispatch(tx, decoders[tx - TX].pcm, decoders[tx - TX].samples);
        } else {
            pcm_concealed = 2;
            mic_pcm_origin origin = timing_pcm_origin(timing_pcm((unsigned)(tx - TX), (unsigned)decoders[tx - TX].samples, 1));
            if (tx == &TX[0] || !TX[0].connected) fifo_write(z, decoders[tx - TX].samples);
            rec_write_observed(tx, z, decoders[tx - TX].samples, &origin);
        }
    }
    tx->level = 0.0;
    pcm_concealed = 0;
}

/* ------------------------------------------------------------------ */
/* status TLVs (Wl cmd 0x19 payload, libwirelessmicservice 0x39a20)    */
/* ------------------------------------------------------------------ */
static void handle_status_tlvs(struct mic_tx *tx, const uint8_t *p, int n)
{
    int i = 0;
    while (i < n) {
        uint8_t t;
        if (p[i] == 0xff) { i++; continue; }   /* filler */
        if (i + 2 > n) break;
        t = p[i];
        {
            int ln = p[i + 1];
            if (i + 2 + ln > n) break;
            if (ln >= 1) {
                int v = p[i + 2] ? 1 : 0;
                int known = 1;
                pthread_mutex_lock(&E.st_mtx);
                if (t == 0x10) {              /* battery percent */
                    if (tx->battery != p[i + 2]) mlog("battery=%d%%", p[i + 2]);
                    tx->battery = p[i + 2];
                } else if (t == 0x05) {       /* charging bool */
                    if (tx->charging != v) mlog("charging=%d", v);
                    tx->charging = v;
                } else if (t == 0x01) {       /* mic_mute bool */
                    if (tx->mute != v) mlog("mute=%d", v);
                    tx->mute = v;
                } else if (t == 0x06) {       /* recording bool */
                    if (tx->recording != v) mlog("recording=%d", v);
                    tx->recording = v;
                } else known = 0;
                pthread_mutex_unlock(&E.st_mtx);
                if (!known && t != 0x00) {
                    static uint8_t seen_t[32];
                    static int nseen_t;
                    int j, s = 0;
                    for (j = 0; j < nseen_t; j++) if (seen_t[j] == t) { s = 1; break; }
                    if (!s && nseen_t < 32) {
                        seen_t[nseen_t++] = t;
                        mlog("status TLV t=0x%02x len=%d (unparsed)", t, ln);
                    }
                }
            }
            i += 2 + ln;
        }
    }
}

/* ---------- v4: Mic 2 (wl 1.0) status side channel ---------------- *
 * Ground truth: libwirelessmicservice.so (Action 5) wl_1_0_protocol_msg
 * _entry @0x3cc50 + libwireless_mic.so Wl_ParsePacket_Core @0x4900.
 * Every 119B audio block (55 77 05 cs | 10 SB seq | 111B LC3) carries ONE
 * side byte SB (p[5]). The SB stream is itself a wl packet stream:
 *   55 | len(6b) | (src<<4)|dst | (ver<<6)|(sum(data)&0x3f) | data
 * data = {cmd, a, b, payload}; cmd 0x19 + 15B payload = TX status:
 *   {01, 77, crc8(seed 0x77, [3..14]), flags16le^0x800, capture, battery,
 *    gain+12, bits^0x0c, capacity16le, ...}
 * The service memcpy()s payload[1..14] into wl_msg_mic_status_t (14B):
 *   +2 u16 flags (bit0 mute, bit4 charging, bit5 recording, ...),
 *   +5 battery %, +6 mic_gain, +8 u16 capacity -- same struct the wl 2.0
 *   TLV parser (0x39a20) fills. TX pushes it unsolicited (~every 46
 *   blocks); the camera never has to ask. */
static void sc_status_15(struct mic_tx *tx, const uint8_t *b)
{
    unsigned flags;
    int batt, chg, mute, rec, gain, cap;
    if (b[0] != 0x01 || b[1] != 0x77) return;
    if (duss_crc8(b + 3, 12, 0x77) != b[2]) return;
    flags = ((unsigned)b[3] | ((unsigned)b[4] << 8)) ^ 0x800;
    batt = b[6];
    gain = (int)b[7] - 12;
    cap = b[9] | (b[10] << 8);
    mute = flags & 1;
    chg = (flags >> 4) & 1;
    rec = (flags >> 5) & 1;
    pthread_mutex_lock(&E.st_mtx);
    /* NOT a percentage: a 1..7 level (see battery_level) */
    if (tx->battery_level != batt) mlog("wl1 status: battery_level=%d%s", batt,
                                      batt >= 6 ? " (low)" : "");
    if (tx->charging != chg) mlog("wl1 status: charging=%d", chg);
    if (tx->mute != mute) mlog("wl1 status: mute=%d", mute);
    if (tx->recording != rec) mlog("wl1 status: recording=%d", rec);
    if (tx->mic_gain != gain || tx->capacity != cap)
        mlog("wl1 status: gain=%ddB capacity=%d flags=0x%04x raw=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
             gain, cap, flags, b[3], b[4], b[5], b[6], b[7], b[8], b[9],
             b[10], b[11], b[12], b[13], b[14]);
    if (b[5] == 1 && tx->connected) mlog("wl1 status: capture event");
    tx->battery_level = batt;
    /* WL1 has no percentage: level 1..7 is INVERTED (1=full, 7=critical).
     * User-calibrated: 1=100%, 4=50%, 7≈0% (linear 100..0 over 7 levels). */
    static const int wl1_batt_pct[] = {0, 100, 84, 67, 50, 33, 16, 5};
    if (tx->battery < 0 && batt >= 0 && batt <= 7)
        tx->battery = wl1_batt_pct[batt];
    tx->charging = chg;
    tx->mute = mute;
    tx->recording = rec;
    tx->mic_gain = gain;
    tx->capacity = cap;
    tx->tx_flags = flags;
    if (tx->tx_format_result == 1) {
        unsigned format_state = (flags >> 6) & 3;
        if (now_ms() - tx->tx_format_ms > 60000) tx->tx_format_result = 4;
        else if (format_state == 2 || format_state == 3)
            tx->tx_format_result = (int)format_state;
    }
    tx->tx_extra = b[8] ^ 0x0c;
    memcpy(tx->tx_version, b + 11, sizeof tx->tx_version);
    tx->tx_status_ms = now_ms();
    pthread_mutex_unlock(&E.st_mtx);
}

static void sc_push(struct mic_tx *tx, uint8_t v)
{
    int i = 0;
    if (tx->sc_len >= (int)sizeof tx->sc) tx->sc_len = 0;   /* garbage overflow */
    tx->sc[tx->sc_len++] = v;
    while (i < tx->sc_len) {
        int L, k;
        unsigned sum = 0;
        if (tx->sc[i] != 0x55) { i++; continue; }
        if (tx->sc_len - i < 4) break;
        L = tx->sc[i + 1] & 0x3f;
        if (L < 5) { i++; continue; }
        if (tx->sc_len - i < L) break;                  /* wait for more */
        for (k = 4; k < L; k++) sum += tx->sc[i + k];
        if ((sum & 0x3f) != (tx->sc[i + 3] & 0x3f)) { i++; continue; }
        if (tx->sc[i + 4] == 0x19 && L - 7 == 15)
            sc_status_15(tx, tx->sc + i + 7);
        else {
            static uint8_t seen[16];
            static int nseen;
            int j, hit = 0;
            for (j = 0; j < nseen; j++) if (seen[j] == tx->sc[i + 4]) hit = 1;
            if (!hit && nseen < 16) {
                seen[nseen++] = tx->sc[i + 4];
                mlog("wl1 side-channel: cmd 0x%02x len %d (first sighting)",
                     tx->sc[i + 4], L);
            }
        }
        i += L;
    }
    if (i > 0) {
        memmove(tx->sc, tx->sc + i, (size_t)(tx->sc_len - i));
        tx->sc_len -= i;
    }
}

/* ================================================================== */
/* v3 UNIFIED SPP STREAM PARSER + PASSIVE RESPONDER SET               */
/* ================================================================== *
 * Ground truth (artifacts/feeddump_live.bin, 2026-10-03, fw 316 TX):
 *   AA <l16be: ver(2b)|count(4b)|len(10b)> <payload> A5
 *     ver=0: payload = count x 125B audio units (6B BT-clock hdr
 *            {btclk u32le, intra u16le} + 119B Wl block) + optional
 *            `len`-byte tail == a TX->camera DUSS control frame
 *            (55 <len16le|0x400> <crc8> <snd> <rcv> <seq16> <flags8>
 *             <set> <cmd> <payload> <crc16le>; observed snd=0x3a,
 *            rcv=0x02/0x07, set=0x12 w/ flags 0x80)
 *     ver=1: payload = subpackets {id:u8, len16be (low 2b=subver),
 *            content}: id=1 audio (Wl stream), id=2 DUSS control
 *            (REPORT_audio_start 1.2 -- not yet seen live, parsed
 *            per spec)
 *   bare Wl blocks (55 77 05 .. 119B) with/without the 6B clock hdr
 *
 * Single state machine; an incomplete frame at the head of sbuf is
 * NEVER consumed, so frames spanning feed() chunks survive (the old
 * pre-scan lost those). Control frames go to duss_dispatch(), audio
 * to the Wl block path.
 * ================================================================== */

/* ---------- reply queue: sends never run on the UIPC feed path ---- *
 * p_dg_send -> BSA_DgSend writes the UIPC socket we may currently be
 * reading from; doing that inside app_audio_play_data risks bsa client
 * reentrancy. A worker thread drains AA-wrapped, send-ready frames. */
#define RQ_CAP 16
static struct {
    pthread_mutex_t mtx;
    pthread_cond_t cv;
    pthread_t th;
    int running;
    int stopping;
    struct { uint8_t b[96], bd[6]; int n; int gap; int addressed; } q[RQ_CAP];
    int h, cnt;
    volatile uint64_t sent, lost;
} RQ = { .mtx = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

/* full-frame hex logger (acceptance-2 evidence: constructed bytes) */
static void hexlog(const char *tag, const uint8_t *b, int n)
{
    char hx[3 * 96 + 1];
    int i, o = 0;
    if (n > 96) n = 96;
    for (i = 0; i < n; i++) o += snprintf(hx + o, sizeof hx - o, "%02x", b[i]);
    mlog("%s [%s] (%dB)", tag, hx, n);
}

static void rq_push_to(const uint8_t *bd, const uint8_t *frame, int n, int gap_ms)
{
    if (n <= 0 || n > 96) return;
    pthread_mutex_lock(&RQ.mtx);
    if (RQ.stopping) {
        pthread_mutex_unlock(&RQ.mtx);
        return;
    }
    if (RQ.cnt >= RQ_CAP) {           /* drop oldest, keep newest */
        RQ.h = (RQ.h + 1) % RQ_CAP;
        RQ.cnt--;
        RQ.lost++;
    }
    {
        int t = (RQ.h + RQ.cnt) % RQ_CAP;
        memcpy(RQ.q[t].b, frame, (size_t)n);
        RQ.q[t].n = n;
        RQ.q[t].gap = gap_ms;
        RQ.q[t].addressed = bd != NULL;
        if (bd) memcpy(RQ.q[t].bd, bd, 6);
        RQ.cnt++;
    }
    pthread_cond_signal(&RQ.cv);
    pthread_mutex_unlock(&RQ.mtx);
}

static void rq_push_gap(struct mic_tx *tx, const uint8_t *frame, int n, int gap_ms)
{
    rq_push_to(tx->cfg_bd_valid ? tx->cfg_bd : NULL, frame, n, gap_ms);
}

static void rq_push(struct mic_tx *tx, const uint8_t *frame, int n)
{
    rq_push_gap(tx, frame, n, 0);
}

static int timed_dg_send(uint8_t *bd, uint8_t *frame, int n)
{
    uint64_t begin = timing_now_ns();
    int result = p_dg_send(bd, frame, n);
    uint64_t end = timing_now_ns();
    /* Our outbound AA wrapper has zero audio units and a DUSS tail. These
     * bounds describe the send API call, not over-the-air transmission. */
    if (n >= 17 && frame[0] == 0xaa && frame[1] < 4 && frame[3] == 0x55 && frame[n-1] == 0xa5)
    {
        pthread_mutex_lock(&E.st_mtx);
        struct mic_tx *tx = tx_find_bd(bd);
        if (tx) timing_sync((unsigned)(tx - TX), frame + 3, n - 4, 1, begin, end, result, bd);
        pthread_mutex_unlock(&E.st_mtx);
    }
    return result;
}

static void *rq_thread(void *arg)
{
    (void)arg;
    for (;;) {
        uint8_t b[96], bd[6];
        int n = 0, gap = 0, addressed = 0;
        pthread_mutex_lock(&RQ.mtx);
        while (RQ.cnt == 0 && !RQ.stopping)
            pthread_cond_wait(&RQ.cv, &RQ.mtx);
        if (RQ.stopping && RQ.cnt == 0) {
            RQ.h = RQ.cnt = 0;
            pthread_mutex_unlock(&RQ.mtx);
            break;
        }
        n = RQ.q[RQ.h].n;
        gap = RQ.q[RQ.h].gap;
        memcpy(b, RQ.q[RQ.h].b, (size_t)n);
        addressed = RQ.q[RQ.h].addressed;
        memcpy(bd, RQ.q[RQ.h].bd, 6);
        RQ.h = (RQ.h + 1) % RQ_CAP;
        RQ.cnt--;
        pthread_mutex_unlock(&RQ.mtx);
        RQ.sent++;
        hexlog("reply_tx", b, n);           /* acceptance-2 evidence */
        if (p_dg_send && addressed) {
            int rc = timed_dg_send(bd, b, n);
            (void)rc;
        }
        /* per-entry pacing: the WL2 settings download needs >=1 ms
         * between its AA frames (TX wl_msg_set_core handling) */
        if (gap > 0) engine_wait_ms((unsigned)gap);
    }
    return NULL;
}

/* Lifecycle caller serializes start/stop; never called by the reply worker. */
static int rq_start(void)
{
    pthread_mutex_lock(&RQ.mtx);
    int rc = 0;
    if (!RQ.running) {
        RQ.stopping = 0;
        rc = pthread_create(&RQ.th, NULL, rq_thread, NULL);
        if (!rc) RQ.running = 1;
        else RQ.stopping = 1;
    }
    pthread_mutex_unlock(&RQ.mtx);
    return rc;
}

static void rq_stop(void)
{
    pthread_mutex_lock(&RQ.mtx);
    RQ.stopping = 1;
    int running = RQ.running;
    pthread_cond_broadcast(&RQ.cv);
    pthread_mutex_unlock(&RQ.mtx);
    if (running) pthread_join(RQ.th, NULL);
    pthread_mutex_lock(&RQ.mtx);
    RQ.running = 0;
    RQ.h = RQ.cnt = 0;
    pthread_mutex_unlock(&RQ.mtx);
}

/* queue a DUSS reply wrapped AA ver=0 with an explicit receiver byte */
static void reply_duss_r(struct mic_tx *tx, uint8_t cmd_id, const uint8_t *payload, int plen,
                         uint8_t receiver)
{
    uint8_t duss[80], aa[96];
    int dl, al;
    if (plen < 0 || plen > 64) return;
    dl = duss_build_r(duss, cmd_id, payload, plen, receiver);
    al = aa_wrap(aa, sizeof aa, duss, dl);
    if (al <= 0) return;
    rq_push(tx, aa, al);
}

/* product-style control reply (sender=0x07, receiver=0x18, 0x40 0x12) */
static void reply_duss(struct mic_tx *tx, uint8_t cmd_id, const uint8_t *payload, int plen)
{
    reply_duss_r(tx, cmd_id, payload, plen, 0x18);
}

/* ---------- the three passive responders (REPORT_ctrl_frames) ----- */

/* state==0x10 equivalent: stream seen recently and link not torn down */
static int link_up_gate(struct mic_tx *tx)
{
    int ok;
    pthread_mutex_lock(&E.st_mtx);
    ok = tx->connected || tx->state == ST_STREAMING;
    pthread_mutex_unlock(&E.st_mtx);
    return ok;
}

/* 0x15 (time-sync data, 20B body) -> 0x56 verbatim echo */
static void resp_0x56_echo(struct mic_tx *tx, const uint8_t *body, int blen)
{
    if (blen != 20 || memcmp(body, tx->cfg_bd, 6)) return;
    if (blen >= 6 + 4 + 2 + 8) {         /* {bd, btclk, intra, systime} */
        pthread_mutex_lock(&E.st_mtx);
        tx->ts_btclk = (uint32_t)(body[6] | (body[7] << 8) |
                                (body[8] << 16) | ((uint32_t)body[9] << 24));
        tx->ts_intra = (uint16_t)(body[10] | (body[11] << 8));
        tx->ts_valid = 1;
        pthread_mutex_unlock(&E.st_mtx);
    }
    reply_duss(tx, 0x56, body, blen);
    mlog("resp: 0x15 -> 0x56 echo (%dB body)", blen);
}

/* 0x17 (RSSI report) -> 0x55 {quality=clamp(dBm+100,0,127), bd} */
static void resp_0x55(struct mic_tx *tx)
{
    uint8_t pl[8];
    int dbm = tx->rssi ? tx->rssi : -60;     /* engine RSSI poll value */
    int q = dbm + 100;
    if (q < 0) q = 0;
    if (q > 127) q = 127;
    pl[0] = (uint8_t)q;
    memcpy(pl + 1, tx->cfg_bd, 6);
    reply_duss(tx, 0x55, pl, 7);
    mlog("resp: 0x17 -> 0x55 quality=0x%02x (dBm=%d)", q, dbm);
}

/* 0x57 (get_status query from TX) -> 16+6n answer, count=1 */
static void resp_0x57(struct mic_tx *tx)
{
    uint8_t pl[22];
    uint32_t btclk;
    uint16_t intra;
    pthread_mutex_lock(&E.st_mtx);
    btclk = tx->ts_valid ? tx->ts_btclk : 0;
    intra = tx->ts_valid ? tx->ts_intra : 0;
    pthread_mutex_unlock(&E.st_mtx);
    pl[0] = 0x00;                                   /* status: OK */
    pl[1] = (uint8_t)(btclk & 0xff);
    pl[2] = (uint8_t)((btclk >> 8) & 0xff);
    pl[3] = (uint8_t)((btclk >> 16) & 0xff);
    pl[4] = (uint8_t)((btclk >> 24) & 0xff);
    pl[5] = (uint8_t)(intra & 0xff);
    pl[6] = (uint8_t)(intra >> 8);
    {
        uint64_t st = now_epoch_ms();
        int i;
        for (i = 0; i < 8; i++) pl[7 + i] = (uint8_t)((st >> (8 * i)) & 0xff);
    }
    pl[15] = 1;                                     /* count = 1 */
    memcpy(pl + 16, tx->cfg_bd, 6);
    reply_duss(tx, 0x57, pl, 22);
    mlog("resp: 0x57 -> answer btclk=0x%08x intra=%u systime=%llu count=1",
         btclk, intra, (unsigned long long)now_epoch_ms());
}

/* 0x58 subtype 02 (TX time-sync answer) -> product chain 0x58/02 =>
 * cmd 0x15 => 0x56 echo; we replay the 0x56 leg with the fresh clock and
 * the current system time.  Live ground truth (mic.log 02:47:59, fw 316):
 * TX uplink = {status:00, subtype:02, btclk u32le@2, intra u16le@6} (8B,
 * no rsv byte).  Camera's own downlink form {02, rsv, btclk@2, intra@6}
 * is accepted too. */
static void resp_0x56_from_ts(struct mic_tx *tx, const uint8_t *body, int blen)
{
    uint8_t pl[20];
    uint32_t btclk = 0;
    uint16_t intra = 0;
    /* Both supported wire layouts put the six clock bytes at +2. Reject
     * failed status, truncated payloads and software-generated observations;
     * none can complete a real peer's clock exchange. */
    if (blen < 8 || !((body[0] == 0 && body[1] == 2) ||
                     (body[0] == 2 && body[1] == 0))) return;
    {
        int cb = 2;
        btclk = (uint32_t)(body[cb] | (body[cb + 1] << 8) |
                           (body[cb + 2] << 16) | ((uint32_t)body[cb + 3] << 24));
        intra = (uint16_t)(body[cb + 4] | (body[cb + 5] << 8));
        /* Native 0x58 handling rejects a zero clock before completing the
         * exchange (audited handler +0x3bc). Real TX replies can contain zero
         * even with status=0: do not overwrite a good clock or consume this
         * peer's request latch, and never echo that invalid sample as 0x56. */
        if (!btclk) return;
        pthread_mutex_lock(&E.st_mtx);
        tx->ts_btclk = btclk;
        tx->ts_intra = intra;
        tx->ts_valid = 1;
        pthread_mutex_unlock(&E.st_mtx);
    }
    memcpy(pl, tx->cfg_bd, 6);
    pl[6] = (uint8_t)(btclk & 0xff);
    pl[7] = (uint8_t)((btclk >> 8) & 0xff);
    pl[8] = (uint8_t)((btclk >> 16) & 0xff);
    pl[9] = (uint8_t)((btclk >> 24) & 0xff);
    pl[10] = (uint8_t)(intra & 0xff);
    pl[11] = (uint8_t)(intra >> 8);
    {
        /* patch 2: systime = the latched 0x58/01 arrival time (the
         * product pairs request/answer on one clock sample); fall back
         * to now when the request was never seen. */
        pthread_mutex_lock(&E.st_mtx);
        uint64_t st = tx->ts_58_01_valid ? tx->ts_58_01_ms : now_epoch_ms();
        tx->ts_58_01_valid = 0; /* one response consumes this TX's own latch */
        pthread_mutex_unlock(&E.st_mtx);
        int i;
        for (i = 0; i < 8; i++) pl[12 + i] = (uint8_t)((st >> (8 * i)) & 0xff);
    }
    reply_duss(tx, 0x56, pl, 20);
    mlog("resp: 0x58/02 -> 0x56 btclk=0x%08x intra=%u", btclk, intra);
}

/* RSSI-event leg of the product keepalive chain: in the product every
 * BSA_SEC_RSSI_EVT (monitor period=1s) makes bt_bsa_app send cmd 0x17
 * and dji_network answer 0x55 to the TX (REPORT_ctrl_frames 3). The
 * engine's RSSI source is the 1 Hz TmReadRawRssi(0x582) poll; each
 * successful poll replays that one 0x55. keepalive_055=0 disables. */
static void ka_055_tick(struct mic_tx *tx)
{
    if (!E.ka_055 || !tx->cfg_bd_valid) return;
    resp_0x55(tx);
}

/* ---------- bring-up responders (run BEFORE the link-up gate) ------- *
 * Final patch set (ctrl-frames agent, 2026-10-03): the three replies
 * the TX waits for during handshake -- without them the connection
 * stalls.  TX uplink frames carry fl=0x80 ("answer me"); the camera
 * always answers with fl=0x40.  Live TX mic-set payloads have a
 * leading 00 byte (feeddump/mic.log 02:47), so subtype matching
 * accepts both {01..} and {00,01..} forms. */

/* patch 1: TX 0x58/01 (time-sync request) -> record the arrival time
 * (the later 0x56 echoes it as systime) and answer 0x58/02 with a
 * ONE-byte payload {02} -- the product answerer (bt_bsa_app @0x444dd0)
 * writes no btclk into the answer (verified).  Receiver 0x3A per the
 * field template: AA 00 0E 55 0E 04 66 07 3A <seq> 40 12 58 02 03 EE A5 */
static void resp_0x58_01(struct mic_tx *tx, int native_reply)
{
    uint8_t pl[1];
    uint64_t t = now_epoch_ms();
    pthread_mutex_lock(&E.st_mtx);
    tx->ts_58_01_ms = t;
    tx->ts_58_01_valid = 1;
    pthread_mutex_unlock(&E.st_mtx);
    /* PLAY_DATA still executes the original parser after our feed callback.
     * Its audited 0x58/01 branch latches time and calls CLOCK_SEND(bd, 2).
     * Enqueuing another reply races that exchange and duplicates air traffic.
     * Keep our timestamp for the service's later 0x56 leg, but one owner sends
     * this reply. Standalone/unverified/alternate-layout input retains fallback. */
    if (native_reply) {
        mlog("resp: TX%u 0x58/01 -> native responder (systime latched %llu)",
             (unsigned)(tx - TX) + 1, (unsigned long long)t);
        return;
    }
    pl[0] = 0x02;
    reply_duss_r(tx, 0x58, pl, 1, 0x3a);
    mlog("resp: 0x58/01 -> 0x58/02 {02} (systime latched %llu)",
         (unsigned long long)t);
}

/* patch 3: TX 0x65/{00} (its ACK/registration) -> idempotent replay of
 * the connect-OK pair the TX may have missed: sync-addr 0x65{03,02,bd,bd}
 * + link-up 0x54{01,10,bd} (the dji_network conn-OK actions; also closes
 * the kick ladder "fire once and forget" gap). */
static void resp_link_up_replay(struct mic_tx *tx)
{
    uint8_t pl[16];
    pl[0] = 0x03; pl[1] = 0x02;
    memcpy(pl + 2, tx->cfg_bd, 6);
    memcpy(pl + 8, tx->cfg_bd, 6);
    reply_duss_r(tx, 0x65, pl, 2 + 12, 0x3a);
    pl[0] = 0x01; pl[1] = 0x10;
    memcpy(pl + 2, tx->cfg_bd, 6);
    reply_duss_r(tx, 0x54, pl, 8, 0x3a);
    mlog("resp: 0x65/00 -> replay sync-addr + link-up");
}

/* ---------- DUSS uplink dispatch ----------------------------------- */
static void wl2_wait_frame_tick(struct mic_tx *tx);   /* WL2 capability-sync retry counter */

static void duss_dispatch(struct mic_tx *tx, const uint8_t *d, int n)
{
    timing_sync((unsigned)(tx - TX), d, n, 0, 0, 0, 0, NULL);
    int dl = ((d[2] & 3) << 8) | d[1];
    uint8_t set = d[9], cmd = d[10];
    uint16_t seq = (uint16_t)(d[6] | (d[7] << 8));
    const uint8_t *pl = d + 11;
    int plen = dl - 13;
    int is_mic = (set == 0x12);

    wl2_wait_frame_tick(tx);       /* any uplink frame counts toward the retry */
    if (!is_mic) {
        /* TX announcements in unknown sets (0x00/0x02/0x04 observed,
         * constant payloads ~0.8-1.5 Hz): rate-limit to one line per
         * (set,cmd) per 30 s, first sighting always logged. */
        static struct { uint8_t set, cmd; uint64_t at; } seen[12];
        static int nseen;
        uint64_t now = now_ms();
        int i, hit = -1;
        for (i = 0; i < nseen; i++)
            if (seen[i].set == set && seen[i].cmd == cmd) { hit = i; break; }
        if (hit < 0 && nseen < 12) {
            seen[nseen].set = set; seen[nseen].cmd = cmd; seen[nseen].at = now;
            hit = nseen++;
        }
        if (hit >= 0) {
            uint64_t age = now - seen[hit].at;
            if (age < 2)
                mlog("duss_rx set=0x%02x cmd=0x%02x seq=%u plen=%d (first sighting)",
                     set, cmd, seq, plen);
            else if (age >= 30000) {
                seen[hit].at = now;
                mlog("duss_rx set=0x%02x cmd=0x%02x seq=%u plen=%d (recurrent)",
                     set, cmd, seq, plen);
            }
        }
        return;
    }

    /* mic set 0x12: log every frame (they are rare and load-bearing) */
    mlog("duss_rx set=12 cmd=0x%02x seq=%u plen=%d "
         "snd=%02x rcv=%02x fl=%02x %02x%02x%02x%02x%02x%02x%02x%02x",
         cmd, seq, plen, d[4], d[5], d[8],
         pl[0], plen > 1 ? pl[1] : 0, plen > 2 ? pl[2] : 0, plen > 3 ? pl[3] : 0,
         plen > 4 ? pl[4] : 0, plen > 5 ? pl[5] : 0, plen > 6 ? pl[6] : 0,
         plen > 7 ? pl[7] : 0, plen > 8 ? pl[8] : 0);

    /* bring-up exchanges first: these arrive BEFORE the first audio
     * block flips tx->connected, and the TX stalls without the replies.
     * Answered unconditionally -- a reply with the SPP down just drops
     * (p_dg_send -1) and is harmless. */
    if (tx->user_hold) return;   /* user disconnect: don't help the TX re-link */
    if ((cmd == 0x58 || cmd == 0x15) &&
        (timing_synthetic_depth || TS[tx - TX].context.synthetic)) return;
    switch (cmd) {
    case 0x58:                                  /* time-sync handshake */
        if ((plen == 1 && pl[0] == 0x01) ||
            (plen == 2 && pl[0] == 0x00 && pl[1] == 0x01)) {
            int native_reply = E.native_clock_requests &&
                TS[tx - TX].context.source == 4 && d[4] == 0x3a && d[5] == 7 &&
                plen == 2 && pl[0] == 0 && pl[1] == 1;
            resp_0x58_01(tx, native_reply);
        } else if (((plen >= 1 && pl[0] == 0x02) ||
                    (plen >= 2 && pl[1] == 0x02)) && plen >= 8) {
            resp_0x56_from_ts(tx, pl, plen);        /* patch 2: echo as 0x56 */
        }
        return;
    case 0x65:                                  /* TX ACK / registration */
        if (plen >= 1 && pl[0] == 0x00)
            resp_link_up_replay(tx);
        return;
    default:
        break;
    }

    if (!link_up_gate(tx)) {
        mlog("duss_rx: mic cmd 0x%02x before link-up gate -- no reply", cmd);
        return;
    }
    switch (cmd) {
    case 0x15:                                  /* time-sync data */
        resp_0x56_echo(tx, pl, plen);
        break;
    case 0x17:                                  /* RSSI report */
        resp_0x55(tx);
        break;
    case 0x57:                                  /* TX asking camera */
        resp_0x57(tx);
        break;
    default:
        break;                                  /* 0x54 etc: log only */
    }
}

/* validate a candidate DUSS frame: 55, len16 shape, crc8, exact length.
 * Min total length is 13 (11B header + 2B crc16, e.g. the product
 * get_status query 55 0D 04 .. 57 has an EMPTY payload). */
static int duss_valid(const uint8_t *d, int n)
{
    int dl;
    if (n < 13 || d[0] != 0x55) return 0;
    if ((d[2] & 0xfc) != 0x04) return 0;
    dl = ((d[2] & 3) << 8) | d[1];
    if (dl != n) return 0;
    if (duss_crc8(d, 3, 0x77) != d[3]) return 0;
    return 1;
}

/* ---------- WL2 (wl 2.0) compact downlink -------------------------- *
 * Camera->TX control rides the same AA ver=0 SPP downlink as DUSS
 * (p_dg_send path, reply-queue worker):
 *   55 | len(=payload+4) | (type<<4)|dst | (ver<<6)|(sum(payload)&0x3f)
 *   | payload;  type 5 = capability/settings, dst 3 = TX0 (single TX). */
#define WL2_TYPE_CAP 5
#define WL2_DST_TX0  3

static int wl_compact_build(uint8_t *out, int outsz, int type, int dst,
                            const uint8_t *payload, int plen)
{
    unsigned sum = 0;
    int i;
    if (!out || !payload || plen < 0 || plen + 4 > outsz) return -1;
    out[0] = 0x55;
    out[1] = (uint8_t)(plen + 4);
    out[2] = (uint8_t)((type << 4) | dst);
    for (i = 0; i < plen; i++) sum += payload[i];
    out[3] = (uint8_t)(sum & 0x3f);          /* ver bits stay 0 */
    memcpy(out + 4, payload, (size_t)plen);
    return plen + 4;
}

/* Action5 wl_capability_sync payload, byte-identical to the verified
 * capture: preamble, rx_enable/gain-band tags, host_name "X2D II"+NUL,
 * then the rx_cap identity block (sent verbatim). */
static const uint8_t wl2_capsync_payload[] = {
    0x20, 0x00, 0x00, 0x02, 0x02,               /* preamble */
    0x01, 0x01, 0x03,                            /* tag1 rx_enable */
    0x01, 0x00,                                  /* tag3 band */
    0x04, 0x01, 0x03,                            /* tag4 = 3 */
    0x05, 0x01, 0x01,                            /* tag5 = 1 */
    0x06, 0x01, 0x01,                            /* tag6 = 1 */
    0x07, 0x02, 0x46, 0x00,                      /* tag7 = 70 */
    0x08, 0x07, 'X', '2', 'D', ' ', 'I', 'I', 0, /* tag8 host_name */
    0x01, 0x09, 0x01, 0x0b, 0x00, 0x01, 0x00, 0xbc, 0x02, 0x00, 0x00
};                                            /* tag1 len9 rx_cap identity */

/* wl_msg_set_core wire values (Action5 set_tx_in_gain @0x2796c value+12;
 * set_tx_rec_btn_en @0x28db4 / auto_shutdown @0x29b04 invert enable). */
static uint8_t wl2_set_wire_value(int tag, int value)
{
    if (tag == 3) return (uint8_t)(value + 12);
    if (tag == 14 || tag == 18 || tag == 19) return (uint8_t)!value;
    return (uint8_t)value;
}

/* Full settings download answering the TX 0x20 capability reply:
 * one AA frame per setting, >=1 ms apart. Order and defaults per the
 * Action5 initial-download sequence. */
static const struct { uint8_t tag, value; } wl2_default_settings[] = {
    {0x06, 0},   /* record off */
    {0x0a, 1},   /* lowcut on */
    {0x08, 1},   /* vibrate on */
    {0x03, 0},   /* in_gain 0 dB (wire +12) */
    {0x10, 0},   /* highcut off */
    {0x05, 0},   /* mute off */
    {0x11, 0},   /* denoise off */
    {0x0f, 0},   /* led level: off (usf default) */
    {0x0e, 0},   /* record button disabled (wire inverted) */
    {0x12, 1},   /* auto shutdown enabled (wire inverted) */
    {0x0d, 0},   /* auto record off */
};
#define WL2_SETTING_COUNT (sizeof wl2_default_settings / sizeof wl2_default_settings[0])

static void wl2_send_capability_sync(struct mic_tx *tx, const char *why)
{
    uint8_t wl[8 + sizeof wl2_capsync_payload], aa[16 + sizeof wl2_capsync_payload];
    int wl_n = wl_compact_build(wl, sizeof wl, WL2_TYPE_CAP, tx_wire_destination(tx),
                                wl2_capsync_payload, (int)sizeof wl2_capsync_payload);
    int aa_n = wl_n > 0 ? aa_wrap(aa, sizeof aa, wl, wl_n) : -1;
    if (aa_n <= 0) return;
    rq_push(tx, aa, aa_n);
    mlog("cap-sync sent (%s, %dB AA)", why, aa_n);
}

static void wl2_send_settings(struct mic_tx *tx)
{
    size_t i;
    unsigned sent = 0;
    pthread_mutex_lock(&E.st_mtx);
    int preserve_gain = tx->wl_protocol == 1;
    pthread_mutex_unlock(&E.st_mtx);
    for (i = 0; i < WL2_SETTING_COUNT; i++) {
        /* Mic 2 gain is controlled by the user, not connection defaults.
         * Skip it even without telemetry: replaying a cached value could
         * overwrite a newer addressed gain command during delayed sync. */
        if (preserve_gain && wl2_default_settings[i].tag == 3) continue;
        uint8_t pl[5] = {0x19, 0x00, 0x00, wl2_default_settings[i].tag,
                         wl2_set_wire_value(wl2_default_settings[i].tag,
                                            wl2_default_settings[i].value)};
        uint8_t wl[16], aa[24];
        int wl_n = wl_compact_build(wl, sizeof wl, WL2_TYPE_CAP, tx_wire_destination(tx), pl, 5);
        int aa_n = wl_n > 0 ? aa_wrap(aa, sizeof aa, wl, wl_n) : -1;
        if (aa_n > 0) {
            rq_push_gap(tx, aa, aa_n, i + 1 < WL2_SETTING_COUNT ? 1 : 0);
            sent++;
        }
    }
    mlog("cap-sync settings sent (%u frames%s)", sent,
         preserve_gain ? ", native TX gain preserved" : "");
}

/* Caller holds st_mtx (or single-threaded start/shutdown). One sync
 * cycle per WL2 connection; audio keeps flowing if it never completes. */
static void wl2_capsync_reset_locked(struct mic_tx *tx)
{
    tx->cap_sync_sent = 0;
    tx->cap_sync_done = 0;
    tx->cap_sync_resends = 0;
    tx->nocap_frames = 0;
    tx->first_audio_ms = 0;
}

/* hk thread: ~2s after the first WL2 audio batch, announce our rx
 * capability once; the TX 0x20 reply completes the exchange
 * (wl2_capability_receive -> wl2_send_settings). */
static void wl2_capsync_tick(struct mic_tx *tx, uint64_t now)
{
    int fire = 0, wl1_fire = 0;
    pthread_mutex_lock(&E.st_mtx);
    if ((tx->wl_protocol == 2 || tx->first_audio_ms)
            && (tx->state == ST_STREAMING || tx->state == ST_CONNECTING)
            && tx->paired
            && tx->cfg_bd_valid && !tx->user_hold && !tx->cap_sync_sent
            && !tx->cap_sync_done     /* a TX-volunteered 0x20 already closed it */
            && tx->first_audio_ms && now - tx->first_audio_ms >= 2000) {
        tx->cap_sync_sent = 1;
        tx->nocap_frames = 0;
        if (tx->wl_protocol == 1) wl1_fire = 1;  /* Mic 2: no cap exchange */
        else fire = 1;
    }
    pthread_mutex_unlock(&E.st_mtx);
    if (fire) wl2_send_capability_sync(tx, "2s anchor");
    /* WL1 (Mic 2): the TX never replies to the capability frame -- go
     * straight to the settings sync for complete 0x19 status reports.
     * Preserve native gain instead of applying the WL2 default. */
    if (wl1_fire) {
        wl2_send_settings(tx);
        pthread_mutex_lock(&E.st_mtx);
        tx->cap_sync_done = 1;
        pthread_mutex_unlock(&E.st_mtx);
        mlog("cap-sync: WL1 direct settings sync (no capability exchange)");
    }
}

/* Non-capability uplink frame while awaiting the TX 0x20 reply: every
 * 20 frames resend the sync once, at most 3 times, then keep waiting
 * silently (audio is never affected). */
static void wl2_wait_frame_tick(struct mic_tx *tx)
{
    int fire = 0, n = 0;
    pthread_mutex_lock(&E.st_mtx);
    if (tx->wl_protocol == 2 && tx->cap_sync_sent && !tx->cap_sync_done
            && tx->cap_sync_resends < 3 && ++tx->nocap_frames >= 20) {
        tx->nocap_frames = 0;
        tx->cap_sync_resends++;
        n = tx->cap_sync_resends;
        fire = 1;
    }
    pthread_mutex_unlock(&E.st_mtx);
    if (fire)
        wl2_send_capability_sync(tx, n >= 3 ? "retry final" : "retry");
}


/* ---------- Wl block handling (bare or AA-stripped) --------------- */
/* st_mtx held. Arm before calling native open: audio may arrive before that
 * call returns. One receive batch is not proof that the bootstrap completed. */
static void tx_arm_bootstrap_locked(struct mic_tx *tx)
{
    tx->kick_stage = 1;
    tx->kick_tries = 0;
    tx->kick_ms = now_ms() + 300;
    tx->kick_audio_first_ns = tx->kick_audio_last_ns = 0;
}

static void tx_bootstrap_audio_locked(struct mic_tx *tx, unsigned cmd)
{
    if (tx->kick_stage != 1 && tx->kick_stage != 2) {
        tx->kick_stage = 0; /* Incoming stream supersedes HFP/post-bond dialing. */
        return;
    }
    if (cmd != 0x10) return;
    const struct mic_timing_event *e = &TS[tx - TX].context;
    uint64_t received = e->receive_ns;
    if (!received || e->synthetic || e->merged ||
        (e->source != 2 && e->source != 4) || !tx->cfg_bd_valid ||
        memcmp(e->bd, tx->cfg_bd, 6) || received > timing_now_ns()) return;
    /* Require advancing, address-qualified receptions across 100 ms. All
     * blocks in one callback share receive_ns, even if dispatch takes longer.
     * A gap over 250 ms starts the observation again; this is only a bootstrap
     * cancellation guard, not an audio/ADC synchronization or health verdict. */
    if (received <= tx->kick_audio_last_ns) return;
    if (!tx->kick_audio_first_ns || received - tx->kick_audio_last_ns > 250000000ULL)
        tx->kick_audio_first_ns = received;
    tx->kick_audio_last_ns = received;
    if (received - tx->kick_audio_first_ns >= 100000000ULL) {
        tx->kick_stage = tx->kick_tries = 0;
        mlog("connect: TX%d sustained audio -> retire SPP bootstrap", (int)(tx - TX) + 1);
    }
}

static int wl_sequence_clock_valid(const struct mic_tx *tx, const struct mic_timing_event *event)
{
    return event->clock_valid && !event->synthetic && !event->merged &&
        (event->source == 2 || event->source == 4) && event->receive_ns &&
        tx->cfg_bd_valid && !memcmp(event->bd, tx->cfg_bd, 6);
}

/* Sequence alone cannot distinguish a >1 s gap from an old callback, nor a
 * full 256-frame wrap from a duplicate. Use clock evidence only when both
 * transport addresses are verified and the bounded 5 ms cadence agrees.
 * This does not unwrap unknown BT counter widths or establish ADC time. */
static int wl_sequence_step(const struct mic_tx *tx, unsigned seq)
{
    unsigned step = (seq - tx->last_seq) & 255;
    const struct mic_timing_event *event = &TS[tx - TX].context;
    if (tx->seq_clock_valid && wl_sequence_clock_valid(tx, event)) {
        int64_t current = (uint64_t)event->btclk * 625 / 2 + event->intra;
        int64_t previous = (uint64_t)tx->seq_btclk * 625 / 2 + tx->seq_intra;
        int64_t delta = current - previous;
        uint64_t distance = delta < 0 ? (uint64_t)-delta : (uint64_t)delta;
        unsigned frames = (unsigned)((distance + 2500) / 5000);
        if (frames && frames <= 1000) {
            /* After accepting a long gap, its old duplicate may have a small
             * positive modular sequence step. It must not decode twice. */
            if (delta < 0 && (frames & 255) == ((256-step) & 255)) return -1;
            if (delta > 0 && frames >= 200 && (frames & 255) == step &&
                    event->receive_ns >= tx->seq_receive_ns) {
                uint64_t arrived = (event->receive_ns - tx->seq_receive_ns) / 1000;
                uint64_t error = arrived > distance ? arrived-distance : distance-arrived;
                if (error <= 500000) return (int)frames;
            }
        }
    }
    return step && step < 200 ? (int)step : -1;
}

static void wl_block_dispatch(struct mic_tx *tx, uint8_t *p)
{
    uint8_t cmd = p[4];
    if (tx->user_hold) {
        /* user said disconnect: a TX re-page reopening SPP on its own
         * (passive branch) must not flip us back to streaming */
        static uint64_t held_log_ms;
        uint64_t t = now_ms();
        if (t - held_log_ms >= 30000) {
            held_log_ms = t;
            mlog("feed: stream while user_hold -> ignored");
        }
        return;
    }
    /* Action 5 wl_comm_data_recv_entry@0x3bf70 selects WL1 only when
     * both version bits are zero. WL2 byte5 is not a WL1 side byte. */
    int protocol = (p[3] & 0xc0) ? 2 : 1;
    int step = 1;
    if (cmd == 0x10) {
        TS[tx - TX].context.seq = p[6];
        if (tx->have_seq && tx->wl_protocol == protocol &&
                (tx->connected || tx->stream_lost)) {
            step = wl_sequence_step(tx, p[6]);
            if (step < 0) {
                timing_packet_note((unsigned)(tx - TX), p[6] == tx->last_seq ?
                    "duplicate" : "backward_sequence", 0);
                return; /* no health refresh, retry reset or false new session */
            }
        }
    }
    pthread_mutex_lock(&E.st_mtx);
    if (tx->wl_protocol != protocol ||
            (!tx->connected && (!tx->stream_lost || cmd == 0x10))) {
        /* Status while audio is lost must preserve the last accepted audio
         * sequence; otherwise its following stale duplicate revives the link. */
        timing_new_stream((unsigned)(tx - TX));
        tx->kick_audio_first_ns = tx->kick_audio_last_ns = 0;
        tx->wl_protocol = protocol;
        memset(&tx->wl2_cap, 0, sizeof tx->wl2_cap);
        tx->sc_len = tx->have_seq = 0;
        tx->tx_status_ms = 0;
        tx->tx_flags = tx->tx_extra = 0;
        tx->battery = tx->battery_level = tx->charging = tx->mute = tx->recording = -1;
        tx->mic_gain = -128;
        tx->capacity = -1;
        if (tx->tx_format_result == 1) tx->tx_format_result = 4;
        wl2_capsync_reset_locked(tx);     /* new session: rerun the sync */
    }
    /* any accepted stream data = streaming (also from IDLE: the TX may
     * reconnect on its own via the stock HFP->SPP branch) */
    if (!tx->stream_lost || cmd == 0x10) {
        if (tx->state != ST_STREAMING) tx->state = ST_STREAMING;
        tx->connected = 1;
        tx->stream_lost = 0;
        tx->retries = 0;
        tx->bond_fails = 0;
        tx->connect_fails = 0;
        tx->device_offline = 0;
        tx_bootstrap_audio_locked(tx, cmd);
        tx->dial_pending = 0;
        tx->dial_until_ms = 0;
    }
    pthread_mutex_unlock(&E.st_mtx);

    if (protocol >= 1) {
        pthread_mutex_lock(&E.st_mtx);
        if (!tx->first_audio_ms) tx->first_audio_ms = now_ms();
        pthread_mutex_unlock(&E.st_mtx);
        if (protocol == 2) wl2_wait_frame_tick(tx);
    }

    if (cmd == 0x10) {               /* audio */
        uint8_t seq = p[6];
        TS[tx - TX].context.seq = seq;
        int plen = 119 - 8;
        tx->last_audio_ms = now_ms();
        tx->last_pad_ms = tx->last_audio_ms;
        if (tx->have_seq && step > 1) {
            timing_packet_note((unsigned)(tx - TX), "sequence_gap", (unsigned)(step - 1));
            int want = step - 1 > 40 ? 40 : step - 1; /* bounded decoder work */
            pcm_fill(tx, want);
            /* The concealment budget must not compress the source timeline.
             * No extra PCM is fabricated here: paired sinks retain the hole,
             * and timing's concealed counter counts only dispatched PLC. */
            unsigned skipped = (unsigned)(step - 1 - want);
            if (skipped) {
                tx->drops += skipped;
                pthread_mutex_lock(&MT.mutex);
                TS[tx - TX].next_sample += (uint64_t)skipped * NSAMP;
                pthread_mutex_unlock(&MT.mutex);
                timing_packet_note((unsigned)(tx - TX), "concealment_limit", 0);
            }
        }
        tx->have_seq = 1;
        tx->last_seq = seq;
        const struct mic_timing_event *event = &TS[tx - TX].context;
        tx->seq_clock_valid = wl_sequence_clock_valid(tx, event);
        tx->seq_btclk = event->btclk; tx->seq_intra = event->intra;
        tx->seq_receive_ns = event->receive_ns;
        if (protocol == 1) sc_push(tx, p[5]);    /* WL1 only */
        if (!decoders[tx - TX].context) { timing_packet_note((unsigned)(tx - TX), "decoder_unavailable", 0); return; }
        memset(decoders[tx - TX].pcm, 0, sizeof decoders[tx - TX].pcm);
        if (decoder_decode(&decoders[tx - TX], p + 7, plen) == 0) {
            tx->frames++;
            pcm_dispatch(tx, decoders[tx - TX].pcm, decoders[tx - TX].samples);
        } else timing_packet_note((unsigned)(tx - TX), "decode_failed", 0);
    } else if (cmd == 0x19) {        /* status */
        handle_status_tlvs(tx, p + 7, 119 - 8);
    }
}

/* scan an arbitrary region for Wl blocks (cksum-gated) */
static void wl_scan_region(struct mic_tx *tx, uint8_t *p, int n, int clock_unit)
{
    long pos = 0;
    while (pos + 119 <= n) {
        uint8_t *b = (uint8_t *)memchr(p + pos, 0x55, (size_t)(n - pos - 118));
        unsigned s;
        int i;
        if (!b) return;
        if (b[1] != 0x77 || b[2] != 0x05) { pos = b - p + 1; continue; }
        s = 0;
        for (i = 4; i <= 117; i++) s += b[i];
        if ((s & 0x3f) != (b[3] & 0x3f)) {
            TS[tx - TX].checksum_rejections++; pos = b - p + 1; continue;
        }
        TS[tx - TX].context.clock_valid = clock_unit && (b - p) % 125 == 6;
        if (TS[tx - TX].context.clock_valid) {
            uint8_t *header = b - 6;
            TS[tx - TX].context.btclk = (uint32_t)header[0] | (uint32_t)header[1] << 8 |
                (uint32_t)header[2] << 16 | (uint32_t)header[3] << 24;
            TS[tx - TX].context.intra = (uint16_t)(header[4] | (header[5] << 8));
        }
        wl_block_dispatch(tx, b);
        pos = b - p + 119;
    }
}

static void wl_scan(struct mic_tx *tx, uint8_t *p, int n) { wl_scan_region(tx, p, n, 0); }

static void wl_units(struct mic_tx *tx, uint8_t *p, int n)
{
    /* Preserve the existing lenient fallback, but never infer a time header
     * from six arbitrary bytes preceding a checksum-valid Wl block. */
    if (n <= 0 || n % 125) { wl_scan(tx, p, n); return; }
    wl_scan_region(tx, p, n, 1);
}

/* feed_mtx held: used on intact stock uplink datagrams, never stream fragments. */
static size_t wl2_capability_receive(struct mic_tx *tx, const uint8_t *p, size_t n, const uint8_t bd[6])
{
    struct wl2_capability cap;
    pthread_mutex_lock(&E.st_mtx);
    int eligible = tx->wl_protocol == 2 && tx->connected && tx->paired && !tx->user_hold
        && tx->cfg_bd_valid && !memcmp(bd, tx->cfg_bd, 6);
    pthread_mutex_unlock(&E.st_mtx);
    if (!eligible) return 0;
    size_t consumed = wl2_parse_capability_envelope(p, n, &cap);
    if (!consumed) return 0;
    pthread_mutex_lock(&E.st_mtx);
    int publish = tx->wl_protocol == 2 && tx->connected && tx->paired && !tx->user_hold
        && tx->cfg_bd_valid && !memcmp(bd, tx->cfg_bd, 6);
    int want_settings = publish && !tx->cap_sync_done;
    if (publish)
        tx->wl2_cap = cap;
    pthread_mutex_unlock(&E.st_mtx);
    if (want_settings) {
        /* the TX expects the camera's answer to its 0x20 reply to be the
         * complete settings set; only then it reports real 0x19 status */
        mlog("cap-sync reply (0x20, %zuB envelope)", consumed);
        wl2_send_settings(tx);
        pthread_mutex_lock(&E.st_mtx);
        tx->cap_sync_done = 1;
        pthread_mutex_unlock(&E.st_mtx);
    }
    return consumed;
}

/* WL2 status record: [len][sum&63][0x19][x][TLVs], same TLV ids as the
 * WL1 0x19 stream (battery=T0x10 u8, charging=T0x05, mute=T0x01,
 * recording=T0x06, gain=T0x11 stored as v-12). Accepted only after the
 * capability exchange completed; coexists with the WL1 side-band parser. */
static size_t wl2_status_receive(struct mic_tx *tx, const uint8_t *p, size_t n, const uint8_t bd[6])
{
    int i, batt = -1, chg = -1, mute = -1, rec = -1, gain = -200;
    unsigned sum = 0;
    pthread_mutex_lock(&E.st_mtx);
    int eligible = tx->wl_protocol == 2 && tx->cap_sync_done && tx->connected
        && !tx->user_hold && tx->cfg_bd_valid && !memcmp(bd, tx->cfg_bd, 6);
    pthread_mutex_unlock(&E.st_mtx);
    if (!eligible || !p || n < 4 || n > 255 || p[0] != n || p[2] != 0x19)
        return 0;
    for (i = 2; i < (int)n; i++) sum += p[i];
    if ((sum & 63) != p[1]) return 0;
    i = 4;
    while (i < (int)n) {
        int t, ln;
        if (p[i] == 0xff) { i++; continue; }      /* filler */
        if (i + 2 > (int)n) break;
        t = p[i];
        ln = p[i + 1];
        if (i + 2 + ln > (int)n) break;
        if (ln == 1) {
            int v = p[i + 2] ? 1 : 0;
            if (t == 0x10) batt = p[i + 2];
            else if (t == 0x05) chg = v;
            else if (t == 0x01) mute = v;
            else if (t == 0x06) rec = v;
            else if (t == 0x11) gain = (int)p[i + 2] - 12;
        }
        i += 2 + ln;
    }
    pthread_mutex_lock(&E.st_mtx);
    if (batt >= 0) {
        if (tx->battery != batt) mlog("real status battery=%d%%", batt);
        tx->battery = batt;
    }
    if (chg >= 0) {
        if (tx->charging != chg) mlog("real status charging=%d", chg);
        tx->charging = chg;
    }
    if (mute >= 0) {
        if (tx->mute != mute) mlog("real status mute=%d", mute);
        tx->mute = mute;
    }
    if (rec >= 0) {
        if (tx->recording != rec) mlog("real status recording=%d", rec);
        tx->recording = rec;
    }
    if (gain != -200) tx->mic_gain = gain;
    if (batt >= 0 || chg >= 0 || mute >= 0 || rec >= 0) tx->tx_status_ms = now_ms();
    pthread_mutex_unlock(&E.st_mtx);
    return n;
}

/* feed/uplink datagram head: 0x20 capability reply (both wire forms)
 * first, then the 0x19 status record; 0 = not a record. */
static size_t wl2_uplink_records(struct mic_tx *tx, const uint8_t *p, size_t n, const uint8_t bd[6])
{
    size_t consumed = wl2_capability_receive(tx, p, n, bd);
    if (consumed) return consumed;
    return wl2_status_receive(tx, p, n, bd);
}

/* Preserve any audio units following a capability reply in the same datagram. */
static void uplink_body_dispatch(struct mic_tx *tx, uint8_t *p, size_t n, const uint8_t bd[6])
{
    size_t consumed = wl2_uplink_records(tx, p, n, bd);
    wl_units(tx, p + consumed, (int)(n - consumed));
}

/* ---------- AA frame payload dispatch ------------------------------ */
static void dispatch_aa(struct mic_tx *tx, uint8_t *payload, int plen, unsigned ver,
                        unsigned cnt, unsigned tlen)
{
    if (ver == 1) {
        /* subpackets {id:u8, len16be (low 2b subver), content}; content
         * length = len16 >> 2 (14-bit). Lenient: any structural mismatch
         * falls back to audio scan of the whole payload. */
        int off = 0, ok = 1, any = 0;
        while (off < plen) {
            uint8_t id;
            unsigned l16, clen;
            if (off + 3 > plen) { ok = 0; break; }
            id = payload[off];
            l16 = ((unsigned)payload[off + 1] << 8) | payload[off + 2];
            clen = l16 >> 2;
            if (id != 1 && id != 2) { ok = 0; break; }
            if ((unsigned long)off + 3 + clen > (unsigned long)plen) {
                /* try the un-shifted reading before giving up */
                clen = l16 & 0x3fff;
                if ((unsigned long)off + 3 + clen > (unsigned long)plen) { ok = 0; break; }
            }
            if (id == 2 && clen >= 13 && duss_valid(payload + off + 3, (int)clen))
                duss_dispatch(tx, payload + off + 3, (int)clen);
            else
                wl_units(tx, payload + off + 3, (int)clen);
            any = 1;
            off += 3 + (int)clen;
        }
        if (!ok || !any)
            wl_scan(tx, payload, plen);            /* degrade to audio scan */
        return;
    }

    /* ver=0: cnt x 125B audio units, then a tlen-byte tail */
    if (cnt > 0)
        wl_units(tx, payload, (int)(cnt * 125));
    if (tlen > 0) {
        uint8_t *tail = payload + cnt * 125;
        if (duss_valid(tail, (int)tlen))
            duss_dispatch(tx, tail, (int)tlen);
        else if (!wl2_uplink_records(tx, tail, tlen, tx->cfg_bd))
            wl_scan(tx, tail, (int)tlen);
    }
}

/* ---------- the single stream state machine ------------------------ */
static void parse_stream(struct mic_tx *tx)
{
    int pos = 0;
    while (pos < tx->slen) {
        uint8_t b = tx->sbuf[pos];
        unsigned l16, ver, cnt, tlen, plen, end;

        if (b == 0xAA) {
            if (tx->slen - pos < 3) break; /* preserve a split AA header */
            l16 = ((unsigned)tx->sbuf[pos + 1] << 8) | tx->sbuf[pos + 2];
            ver = l16 >> 14;
            cnt = (l16 >> 10) & 0xF;
            tlen = l16 & 0x3FF;
            plen = cnt * 125 + tlen;
            if (plen >= 4 && plen <= (unsigned)15 * 125 + 1023) {
                end = (unsigned)pos + 3 + plen;         /* index of A5 */
                if (end >= (unsigned)tx->slen) break;     /* wait: incomplete */
                if (tx->sbuf[end] == 0xA5) {
                    timing_receive_context((unsigned)(tx - TX), (size_t)pos, end + 1 - (unsigned)pos, tx->cfg_bd);
                    dispatch_aa(tx, tx->sbuf + pos + 3, (int)plen, ver, cnt, tlen);
                    pos = (int)end + 1;
                    continue;
                }
            }
            TS[tx - TX].stream_resync_bytes++;
            pos++;                                     /* junk 0xAA */
            continue;
        }
        if (b == 0x55 && tx->slen - pos < 3) break;
        if (b == 0x55 &&
            tx->sbuf[pos + 1] == 0x77 && tx->sbuf[pos + 2] == 0x05) {
            if (tx->slen - pos < 119) break;             /* wait: incomplete */
            {
                unsigned s = 0;
                int i;
                for (i = 4; i <= 117; i++) s += tx->sbuf[pos + i];
                if ((s & 0x3f) == (tx->sbuf[pos + 3] & 0x3f)) {
                    timing_receive_context((unsigned)(tx - TX), (size_t)pos, 119, tx->cfg_bd);
                    wl_block_dispatch(tx, tx->sbuf + pos);
                    pos += 119;
                    continue;
                }
            }
            TS[tx - TX].checksum_rejections++;
            TS[tx - TX].stream_resync_bytes++;
            pos++;                                     /* bad cksum */
            continue;
        }
        TS[tx - TX].stream_resync_bytes++;
        pos++;
    }
    if (pos > 0) {
        timing_consume((unsigned)(tx - TX), (size_t)pos);
        memmove(tx->sbuf, tx->sbuf + pos, (size_t)(tx->slen - pos));
        tx->slen -= pos;
    }
}

void miccore_feed_bd(const uint8_t bd[6], const uint8_t *buf, int len)
{
    uint64_t receive_ns = timing_now_ns();
    if (!atomic_load(&feed_enabled) || !buf || len <= 0 || len > (int)sizeof TX[0].sbuf) return;
    pthread_mutex_lock(&E.feed_mtx);
    uint64_t acquired_ns = timing_now_ns();
    if (!atomic_load(&feed_enabled)) { pthread_mutex_unlock(&E.feed_mtx); return; }
    pthread_mutex_lock(&E.st_mtx);
    struct mic_tx *tx = tx_find_bd(bd);
    pthread_mutex_unlock(&E.st_mtx);
    if (!tx) {
        pthread_mutex_unlock(&E.feed_mtx);
        return;
    }
    unsigned slot = (unsigned)(tx - TX);
    timing_ingress_begin(slot, 0, (size_t)len, receive_ns, acquired_ns);
    if (tx->slen + len > (int)sizeof tx->sbuf) { TS[slot].stream_overflows++; tx->slen = 0; }
    if (!tx->slen) TS[slot].span_count = 0;
    timing_append_span(slot, (size_t)len, receive_ns, timing_synthetic_depth != 0);
    TS[slot].spans[TS[slot].span_count - 1].address_verified = bd != NULL;
    memcpy(tx->sbuf + tx->slen, buf, (size_t)len);
    tx->slen += len;
    parse_stream(tx);
    timing_ingress_end(slot, 0, acquired_ns, timing_now_ns());
    pthread_mutex_unlock(&E.feed_mtx);
}

void miccore_feed(const uint8_t *buf, int len)
{
    miccore_feed_bd(NULL, buf, len);
}

/* ------------------------------------------------------------------ */
/* status JSON                                                         */
/* ------------------------------------------------------------------ */
/* out requires 6 * strlen(value) + 3 bytes. */
static void mic_json_string(char *out, const char *value)
{
    *out++ = '"';
    for (; *value; value++) {
        unsigned char c = (unsigned char)*value;
        if (c == '"' || c == '\\') { *out++ = '\\'; *out++ = (char)c; }
        else if (c < 32) { snprintf(out, 7, "\\u%04x", c); out += 6; }
        else *out++ = (char)c;
    }
    *out++ = '"';
    *out = 0;
}
#include "tx_controls.h"
#include "mic_recording_observation.h"
#include "mic_recording_timeline.h"

/* Caller holds feed_mtx so slot identity and timing cannot change mid-snapshot. */
static int status_json_for_slot(unsigned slot, char *out, int outsz)
{
    if (!out || outsz <= 0) return -1;
    int n;
    struct mic_tx *tx = &TX[slot];
    char configured_bd[32] = "null", record_bd[32] = "null";
    unsigned record_slot, channels, channel_mode, record_channel_mode;
    char record_bds[80] = "[]", alignment[1800] = "null", mono_alignment[512] = "null";
    char bds[24] = "";
    char bdq[32] = "null";
    char name[64] = "";
    char nmq[387] = "null";
    char local_path[1539];
    uint64_t local_bytes, remaining_ms, local_start, local_real, local_silent;
    int local_recording, local_error, local_completed, local_owned;
    char batt[16] = "null";
    char blvl[16] = "null", blow[8] = "null";
    char rssis[16] = "null";
    char chgs[16] = "null", mutes[16] = "null", recs[16] = "null";
    int paired, charging, mute, recording;
    char tx_json[2048], recording_json[640], timing_json[1800], recording_timing[640], pcm_json[1024];
    if (pcm_stereo_status(pcm_json, sizeof pcm_json) < 0) return -1;

    pthread_mutex_lock(&E.rec_mtx);
    mic_json_string(local_path, E.rec_path);
    record_slot = E.rec_slot;
    channels = rec_channels_locked();
    channel_mode = channel_mode_locked();
    record_channel_mode = rec_channel_mode_locked();
    if (E.rec_path[0] && channels == 1 && E.rec_owner[0])
        snprintf(mono_alignment,sizeof mono_alignment,
            "{\"algorithm\":\"source_sample_cursor_v1\",\"mapping_verified\":false,"
            "\"silence_grace_ms\":%u,\"late_samples\":%llu,\"reanchors\":%llu,"
            "\"stop_tail\":{\"waiting\":%s,\"wait_limit_ms\":%u,\"target_samples\":%llu,"
            "\"received_samples\":%llu,\"timed_out\":%s}}",
            MIC_REC_MONO_GRACE_MS,(unsigned long long)record_mono.late_samples,
            (unsigned long long)record_mono.reanchors,record_mono.draining ? "true":"false",
            MIC_REC_MONO_GRACE_MS,(unsigned long long)record_mono.stop_target,
            (unsigned long long)record_mono.tail_received,record_mono.tail_timed_out ? "true":"false");
    if (E.rec_path[0]) snprintf(record_bd, sizeof record_bd, "\"%02x:%02x:%02x:%02x:%02x:%02x\"",
        E.rec_target[0], E.rec_target[1], E.rec_target[2], E.rec_target[3], E.rec_target[4], E.rec_target[5]);
    if (E.rec_path[0]) {
        if (channels == 2) {
            snprintf(record_bds, sizeof record_bds,
                "[\"%02x:%02x:%02x:%02x:%02x:%02x\",\"%02x:%02x:%02x:%02x:%02x:%02x\"]",
                E.rec_targets[0][0], E.rec_targets[0][1], E.rec_targets[0][2],
                E.rec_targets[0][3], E.rec_targets[0][4], E.rec_targets[0][5],
                E.rec_targets[1][0], E.rec_targets[1][1], E.rec_targets[1][2],
                E.rec_targets[1][3], E.rec_targets[1][4], E.rec_targets[1][5]);
            snprintf(alignment, sizeof alignment,
                "{\"mode\":\"relative_bt_with_receiver_fallback\",\"mapping_verified\":false,"
                "\"channel_input_samples\":[{\"real\":%llu,\"silent\":%llu},{\"real\":%llu,\"silent\":%llu}],"
                "\"receiver_fallback_frames\":[%llu,%llu],\"clock_rebases\":[%llu,%llu],"
                "\"clock_catchup\":[%s,%s],\"clock_catchup_dropped_frames\":[%llu,%llu],"
                "\"wait_ms\":%u,\"silence_grace_ms\":%u,\"late_frames\":[%llu,%llu],\"queue_overflow_frames\":[%llu,%llu],"
                "\"algorithm\":\"paired_ring_v1\",\"capacity_frames\":%u,\"suspended\":[%s,%s],"
                "\"sync_pending\":[%s,%s],\"timestamp_relocations\":[%llu,%llu],"
                "\"sync_dropped_frames\":[%llu,%llu],\"resume_events\":[%llu,%llu],"
                "\"phase_delay_samples\":[%u,%u],\"phase_corrections\":%llu,"
                "\"phase_padding_samples\":[%llu,%llu],\"concealed_samples\":[%llu,%llu],"
                "\"pre_start\":{\"max_age_ms\":%u,\"queued_frames\":[%llu,%llu],\"first_event_ids\":[%llu,%llu]},"
                "\"stop_tail\":{\"waiting\":%s,\"wait_limit_ms\":%u,\"target_samples\":%llu,"
                "\"received_frames\":[%llu,%llu],\"timed_out\":%s}}",
                (unsigned long long)record_stereo.channel[0].real_samples,
                (unsigned long long)record_stereo.channel[0].silent_samples,
                (unsigned long long)record_stereo.channel[1].real_samples,
                (unsigned long long)record_stereo.channel[1].silent_samples,
                (unsigned long long)record_clock.peer[0].fallback_frames,
                (unsigned long long)record_clock.peer[1].fallback_frames,
                (unsigned long long)record_clock.peer[0].rebases,
                (unsigned long long)record_clock.peer[1].rebases,
                record_clock.peer[0].shared_catchup ? "true" : "false",
                record_clock.peer[1].shared_catchup ? "true" : "false",
                (unsigned long long)record_clock.peer[0].catchup_drops,
                (unsigned long long)record_clock.peer[1].catchup_drops,
                record_stereo.wait_ms,
                MIC_REC_SILENCE_GRACE_MS,
                (unsigned long long)record_stereo.channel[0].late,
                (unsigned long long)record_stereo.channel[1].late,
                (unsigned long long)record_stereo.channel[0].overflow,
                (unsigned long long)record_stereo.channel[1].overflow,
                MIC_STEREO_CAP,
                record_stereo.channel[0].suspended ? "true" : "false",
                record_stereo.channel[1].suspended ? "true" : "false",
                record_stereo.channel[0].sync_pending ? "true" : "false",
                record_stereo.channel[1].sync_pending ? "true" : "false",
                (unsigned long long)record_stereo.channel[0].relocations,
                (unsigned long long)record_stereo.channel[1].relocations,
                (unsigned long long)record_stereo.channel[0].sync_drops,
                (unsigned long long)record_stereo.channel[1].sync_drops,
                (unsigned long long)record_stereo.channel[0].resumes,
                (unsigned long long)record_stereo.channel[1].resumes,
                record_stereo.phase.delay_samples[0], record_stereo.phase.delay_samples[1],
                (unsigned long long)record_stereo.phase.corrections,
                (unsigned long long)record_stereo.phase.padded_samples[0],
                (unsigned long long)record_stereo.phase.padded_samples[1],
                (unsigned long long)record_stereo.channel[0].concealed_samples,
                (unsigned long long)record_stereo.channel[1].concealed_samples,
                MIC_PREROLL_AGE_MS,(unsigned long long)record_preroll_frames[0],
                (unsigned long long)record_preroll_frames[1],
                (unsigned long long)record_preroll_first[0],(unsigned long long)record_preroll_first[1],
                record_tail.draining ? "true":"false",MIC_STEREO_WAIT_MS,
                (unsigned long long)record_tail.target,
                (unsigned long long)record_tail.received_frames[0],
                (unsigned long long)record_tail.received_frames[1],record_tail.timed_out ? "true":"false");
        } else snprintf(record_bds, sizeof record_bds, "[%s]", record_bd);
    }
    local_recording = E.rec_f != NULL || E.rec_finalizing;
    local_completed = E.rec_completed;
    local_error = E.rec_error;
    local_bytes = E.rec_bytes;
    local_owned = E.rec_owner[0] != 0;
    local_start = E.rec_start_ms;
    local_real = E.rec_real_samples;
    local_silent = E.rec_silent_samples;
    snprintf(recording_timing, sizeof recording_timing,
        "{\"recorder_start_monotonic_ns\":%llu,\"start_generation\":%llu,\"after_event_id\":%llu,"
        "\"end_generation\":%llu,\"end_event_id\":%llu,\"closed\":%s,\"sample0_monotonic_ns\":null,"
        "\"slot_start_generations\":[%llu,%llu],\"slot_end_generations\":[%llu,%llu],\"history_after_event_id\":%llu}",
        (unsigned long long)E.rec_start_ns, (unsigned long long)E.rec_timing_generation,
        (unsigned long long)E.rec_timing_first, (unsigned long long)E.rec_timing_end_generation,
        (unsigned long long)E.rec_timing_last, E.rec_timing_closed ? "true" : "false",
        (unsigned long long)E.rec_timing_generations[0], (unsigned long long)E.rec_timing_generations[1],
        (unsigned long long)E.rec_timing_end_generations[0], (unsigned long long)E.rec_timing_end_generations[1],
        (unsigned long long)E.rec_history_after);
    if (E.rec_aac) {
        mic_aac_stats s = mic_aac_snapshot(E.rec_aac);
        local_recording = !s.completed; local_completed = s.completed;
        local_error |= s.error; local_bytes = s.bytes;
        local_real = s.actual_samples; local_silent = s.silent_samples;
        snprintf(recording_json, sizeof recording_json,
            "\"format\":\"aac_adts\",\"bitrate\":%u,\"frame_length\":%u,"
            "\"encoder_delay_samples\":%u,\"input_samples\":%llu,\"packets\":%llu,\"bytes\":%llu,"
            "\"queue_overflows\":%llu,\"overflow_samples\":%llu,\"encoder_flushed\":%s,\"completed\":%s,\"error\":%s,",
            s.bitrate, s.frame_length, s.encoder_delay_samples,
            (unsigned long long)s.input_samples, (unsigned long long)s.packets, (unsigned long long)s.bytes,
            (unsigned long long)s.overflows, (unsigned long long)s.overflow_samples,
            s.encoder_flushed ? "true" : "false", s.completed ? "true" : "false", local_error ? "true" : "false");
    } else snprintf(recording_json, sizeof recording_json, "\"format\":\"wav\",");
    uint64_t now = now_ms();
    remaining_ms = E.rec_deadline_ms > now ? E.rec_deadline_ms - now : 0;
    pthread_mutex_unlock(&E.rec_mtx);
    if (timing_summary(slot, timing_json, sizeof timing_json) < 0) return -1;
    pthread_mutex_lock(&E.st_mtx);
    if (tx_status_json(tx, tx_json, sizeof tx_json) < 0) {
        pthread_mutex_unlock(&E.st_mtx);
        out[0] = 0;
        return -1;
    }
    if (tx->cfg_bd_valid) snprintf(configured_bd, sizeof configured_bd, "\"%02x:%02x:%02x:%02x:%02x:%02x\"",
        tx->cfg_bd[0], tx->cfg_bd[1], tx->cfg_bd[2], tx->cfg_bd[3], tx->cfg_bd[4], tx->cfg_bd[5]);
    paired = tx->paired;
    charging = tx->charging;
    mute = tx->mute;
    recording = tx->recording;
    /* v2: unpaired -> bd/name/battery/rssi all null (GUI must not show
     * model/MAC/signal of a device that is no longer paired) */
    if (paired) {
        if (tx->cfg_bd_valid) {
            snprintf(bds, sizeof bds, "%02x:%02x:%02x:%02x:%02x:%02x",
                     tx->cfg_bd[0], tx->cfg_bd[1], tx->cfg_bd[2],
                     tx->cfg_bd[3], tx->cfg_bd[4], tx->cfg_bd[5]);
            snprintf(bdq, sizeof bdq, "\"%s\"", bds);
        }
        snprintf(name, sizeof name, "%s", tx->name);
        mic_json_string(nmq, name);
        snprintf(batt, sizeof batt, tx->battery >= 0 ? "%d" : "null", tx->battery);
        if (tx->battery_level >= 1) {
            snprintf(blvl, sizeof blvl, "%d", tx->battery_level);
            snprintf(blow, sizeof blow, tx->battery_level >= 6 ? "true" : "false");
        }
        snprintf(rssis, sizeof rssis, tx->rssi ? "%d" : "null", tx->rssi);
        snprintf(chgs, sizeof chgs, charging >= 0 ? (charging ? "true" : "false") : "null");
        snprintf(mutes, sizeof mutes, mute >= 0 ? (mute ? "true" : "false") : "null");
        snprintf(recs, sizeof recs, recording >= 0 ? (recording ? "true" : "false") : "null");
    }
    n = snprintf(out, (size_t)outsz,
        "{\"tx_status_api\":1,\"stereo_recording_api\":1,\"tx_slots\":2,\"tx_slot\":%u,\"configured_bd\":%s,"
        "\"channel_mode_api\":1,\"channel_mode\":%u,\"local_record_channel_mode\":%u,"
        "\"pcm_stereo_api\":1,\"pcm_stereo\":%s,"
        "\"local_record_bd\":%s,\"local_record_slot\":%u,\"local_record_bds\":%s,\"recording_alignment\":%s,"
        "\"recording_mono_alignment\":%s,"
        "\"protocol_version\":4,\"session_api\":1,\"record_formats\":[\"wav\",\"aac_adts\"],\"can_cancel\":true,\"can_target_tx\":true,\"paired\":%s,\"connected\":%s,\"bd\":%s,\"name\":%s,\"battery\":%s,"
        "\"battery_level\":%s,\"battery_low\":%s,"
        "\"charging\":%s,\"mute\":%s,\"recording\":%s,"
        "\"local_recording\":%s,\"local_completed\":%s,\"local_record_error\":%s,"
        "\"local_record_path\":%s,\"local_record_bytes\":%llu,\"local_record_remaining_ms\":%llu,"
        "\"local_owned\":%s,\"recording_start_monotonic_ms\":%llu,"
        "\"clock_domain\":\"camera:CLOCK_MONOTONIC\",\"sample_rate\":48000,\"channels\":%u,\"sample_bytes\":2,"
        "%s\"actual_samples\":%llu,\"silent_samples\":%llu,\"tx_report_monotonic_ms\":%llu,"
        "\"engine_ready\":%s,\"engine_pid\":%d,\"engine_mode\":\"%s\",\"hook_mode\":\"%s\",\"monotonic_ms\":%llu,"
        "\"tx\":%s,\"timing_version\":1,\"sample0_monotonic_ns\":null,\"timing\":%s,\"recording_timing\":%s,"
        "\"rssi\":%s,\"frames\":%llu,\"drops\":%llu,\"level\":%.3f,"
        "\"state\":\"%s\",\"retrying\":%s,\"connection_queued\":%s,\"ts\":%llu}",
        slot, configured_bd, channel_mode, record_channel_mode, pcm_json, record_bd, record_slot, record_bds, alignment, mono_alignment,
        paired ? "true" : "false",
        (paired && tx->connected) ? "true" : "false",
        bdq, nmq, batt, blvl, blow, chgs, mutes, recs,
        local_recording ? "true" : "false", local_completed ? "true" : "false",
        local_error ? "true" : "false", local_path,
        (unsigned long long)local_bytes, (unsigned long long)remaining_ms,
        local_owned ? "true" : "false", (unsigned long long)local_start, channels,
        recording_json, (unsigned long long)local_real, (unsigned long long)local_silent,
        (unsigned long long)tx->tx_status_ms,
        atomic_load(&feed_enabled) && !atomic_load(&engine_stopping) ? "true" : "false",
        (int)getpid(), E.offline ? "offline" : E.standalone ? "standalone" : "in_process",
#ifdef MICCORE_NATIVE_HOOK
        "native",
#else
        "external-or-none",
#endif
        (unsigned long long)now_ms(), tx_json, timing_json, recording_timing, rssis,
        (unsigned long long)tx->frames,
        (unsigned long long)tx->drops,
        tx->level,
        (tx->device_offline || tx->stream_lost) ? "offline" : ST_NAME[tx->state],
        (tx->stream_lost && !tx->device_offline && !tx->user_hold &&
         (tx->state == ST_RECONNECTING || tx->state == ST_CONNECTING)) ? "true" : "false",
        tx->autocmd_pending && !tx->user_hold ? "true" : "false",
        (unsigned long long)now_epoch_ms());
    pthread_mutex_unlock(&E.st_mtx);
    if (n < 0 || n >= outsz) { out[0] = 0; return -1; }
    return n;
}

/* Preserve the legacy status schema and choose TX2 when it is the only slot.
 * Full dual status is opt-in to avoid expanding old clients' fixed buffers. */
int miccore_status_json(char *out, int outsz)
{
    pthread_mutex_lock(&E.feed_mtx);
    unsigned slot = !TX[0].cfg_bd_valid && TX[1].cfg_bd_valid ? 1 : 0;
    int result = status_json_for_slot(slot, out, outsz);
    pthread_mutex_unlock(&E.feed_mtx);
    return result;
}

static int status_json_for_address(const uint8_t *bd, char *out, int outsz)
{
    pthread_mutex_lock(&E.feed_mtx);
    struct mic_tx *tx = tx_find_bd(bd);
    int result = tx ? status_json_for_slot((unsigned)(tx - TX), out, outsz) : -2;
    pthread_mutex_unlock(&E.feed_mtx);
    return result;
}

static int status_all_json(char *out, int outsz)
{
    if (!out || outsz <= 0) return -1;
    char peer[8192];
    size_t used = 0;
    int result = -1;
    pthread_mutex_lock(&E.feed_mtx);
    if (tx_json_append(out, (size_t)outsz, &used,
        "{\"tx_status_api\":1,\"transmitters\":[") < 0) goto done;
    for (unsigned slot = 0; slot < 2; slot++) {
        if (status_json_for_slot(slot, peer, sizeof peer) < 0 ||
            tx_json_append(out, (size_t)outsz, &used, "%s%s", slot ? "," : "", peer) < 0) goto done;
    }
    if (tx_json_append(out, (size_t)outsz, &used, "]}") < 0) goto done;
    result = (int)used;
done:
    pthread_mutex_unlock(&E.feed_mtx);
    if (result < 0) out[0] = 0;
    return result;
}

static void status_write(void)
{
    char buf[8192]; /* dual recording metadata plus escaped paths/settings */
    int n = miccore_status_json(buf, sizeof buf);
    int fd;
    if (n <= 0) return;
    fd = open(STATUS_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    if (write(fd, buf, (size_t)n) == n) {
        buf[0] = '\n';
        write(fd, buf, 1);
    }
    close(fd);
    rename(STATUS_TMP, STATUS_PATH);
}

/* ------------------------------------------------------------------ */
/* connect / disconnect / kickstart                                    */
/* ------------------------------------------------------------------ */
static int ctl_pair(const uint8_t *bd, char *resp, int rsz);

/* control_mtx held, outside any callback/feed/state lock. The native stack
 * remains the only writer of its tables; its CLOSE callback wakes this wait.
 * A queued OPEN cannot overrule a newer explicit connect (hold already clear). */
static void tx_close_held_transport(struct mic_tx *tx, int state)
{
    if (!tx->user_hold || !(state & 2) || !bb_base || E.offline) return;
    tx->dg_close_pending = 1;
    int rc = dg_close_and_wait(tx->cfg_bd);
    if (!rc) tx->dg_close_pending = 0;
    mlog("hold: TX%u incoming SPP close rc=%d; bond retained", (unsigned)(tx - TX) + 1, rc);
}

/* Caller holds control_mtx; callback threads only enqueue copied events.
 * Taking feed before state matches the parser's lock order. An OPEN releases
 * dialing, but only actual uplink audio may mark a lost stream available. */
static void tx_transport_event(struct mic_tx *tx, unsigned kind, int state, uint64_t now)
{
    pthread_mutex_lock(&E.feed_mtx);
    pthread_mutex_lock(&E.st_mtx);
    tx->dial_pending = 0;
    tx->dial_until_ms = 0;
    if (!state) tx->dg_close_pending = 0;
    if (tx->user_hold || (!(state & 2) && kind != MIC_DG_OPENED)) {
        int was_streaming = tx->connected || tx->state == ST_STREAMING;
        tx->connected = 0;
        tx->stream_lost = 1;
        tx->level = 0;
        tx->slen = tx->have_seq = tx->sc_len = 0;
        tx->ts_valid = tx->ts_58_01_valid = 0;
        tx->kick_stage = tx->kick_tries = 0;
        if (tx->user_hold || tx->device_offline) {
            tx->state = ST_IDLE;
        } else if (was_streaming || tx->state == ST_CONNECTING || tx->state == ST_RECONNECTING) {
            tx->state = ST_RECONNECTING;
            if (was_streaming) tx->retries = 0;
            /* Do not reset the budget on repeated failed OPEN callbacks. */
            tx->retry_gate_ms = now + 1000;
        }
    }
    pthread_mutex_unlock(&E.st_mtx);
    pthread_mutex_unlock(&E.feed_mtx);
    tx_close_held_transport(tx, state);
}

static void dg_events_drain(void)
{
    struct mic_dg_event event, latest[2];
    int seen[2] = {0, 0};
    unsigned dropped;
    while (mic_dg_event_pop(&event, &dropped)) {
        if (dropped) mlog("dg_events: %u events overwritten; resync from native table", dropped);
        struct mic_tx *tx = tx_find_bd(event.bd);
        if (tx && event.sequence > tx->dg_event_after) {
            unsigned slot = (unsigned)(tx - TX); latest[slot] = event; seen[slot] = 1;
        }
    }
    int changed = 0;
    for (unsigned slot = 0; slot < 2; slot++) if (seen[slot]) {
        struct mic_tx *tx = &TX[slot];
        /* A later OPEN may already be in the native table while its callback
         * is finishing. Never let an older CLOSE erase that new stream. */
        int state = dg_target_state(tx->cfg_bd);
        unsigned kind = latest[slot].kind;
        mlog("dg_event: TX%u %s handle=%u native_state=%d", slot + 1,
             kind == MIC_DG_OPENED ? "OPEN" : kind == MIC_DG_CLOSED ? "CLOSE" : "OPEN_FAILED",
             latest[slot].handle, state);
        if (kind != MIC_DG_OPENED || (state & 2)) {
            tx_transport_event(tx, kind, state, now_ms());
            struct mic_tx *other = &TX[slot ^ 1];
            if (other->autocmd_pending && !other->user_hold) other->autocmd_at = now_ms();
            changed = 1;
        }
        tx->dg_seen_valid = 1; tx->dg_seen_state = state;
    }
    /* Publish before housekeeping can enter an RSSI/native API wait. */
    if (changed) status_write();
}

static void dg_table_edge(struct mic_tx *tx, uint64_t now)
{
    if (!bb_base || !tx->cfg_bd_valid) return;
    int state = dg_target_state(tx->cfg_bd);
    if (tx->dg_seen_valid && tx->dg_seen_state == state) {
        /* A rejected/timed-out close is retried on housekeeping, without
         * inventing another table edge or treating it as a new connection. */
        tx_close_held_transport(tx, state);
        return;
    }
    int previous = tx->dg_seen_valid ? tx->dg_seen_state : -1;
    tx->dg_seen_valid = 1; tx->dg_seen_state = state;
    mlog("dg_edge: TX%u %d -> %d", (unsigned)(tx - TX) + 1, previous, state);
    if (state & 2) tx_transport_event(tx, MIC_DG_OPENED, state, now);
    else if (previous & 2 && previous >= 0) tx_transport_event(tx, MIC_DG_CLOSED, state, now);
}

/* control_mtx held. Serialize OUR asynchronous HFP/SPP dial windows, not
 * established streams. The native OPEN_EVT or verified incoming audio lets
 * the next peer proceed immediately; otherwise the caller's bounded attempt
 * expires normally. Never erase stack-owned pending DG slots to release this
 * scheduling gate: late callbacks still belong to their original address. */
static int tx_dial_blocked(struct mic_tx *tx, uint64_t now)
{
    if (tx->cfg_bd_valid && (dg_target_state(tx->cfg_bd) & 2)) return 0;
    struct mic_tx *other = &TX[(unsigned)(tx - TX) ^ 1];
    int opened = other->cfg_bd_valid && (dg_target_state(other->cfg_bd) & 2);
    pthread_mutex_lock(&E.st_mtx);
    /* Deadline is only a missing-event/HFP-timeout watchdog in hook mode. */
    if (other->dial_pending && now >= other->dial_until_ms) other->dial_pending = 0;
    int blocked = other->cfg_bd_valid && !other->user_hold && !other->connected &&
        (mic_dg_events_enabled() ? other->dial_pending : other->dial_until_ms > now) && !opened;
    pthread_mutex_unlock(&E.st_mtx);
    return blocked;
}

/* Keep boot staggering after app_init as well as on an already-ready stack.
 * Pending-but-not-due peers must not bypass this via the idle retry path. */
static void schedule_pending_connects(uint64_t first_ms)
{
    for (unsigned slot = 0; slot < 2; slot++) if (TX[slot].autocmd_pending) {
        TX[slot].autocmd_at = first_ms;
        first_ms += 2000;
    }
}

/* A native server slot becomes an opened connection in place. Only unopened
 * servers are available to accept the next TX; never count an occupied server
 * or erase a pending client. Caller holds control_mtx. */
static int ensure_idle_dg_listeners(unsigned needed)
{
    if (!bb_base || !p_dg_listen || !bb_stack_inited()) return -1;
    if (needed > 2) return -1;
    for (unsigned attempt = 0; attempt < needed; attempt++) {
        unsigned idle = 0;
        for (unsigned i = 0; i < DG_CONS; i++) {
            volatile uint8_t *e = dg_entry(i);
            if (e[0] && e[2] && !e[1]) idle++;
        }
        if (idle >= needed) return 0;
        int rc = p_dg_listen();
        mlog("listen: available=%u required=%u app_dg_listen=%d", idle, needed, rc);
        if (rc) return rc;
    }
    return 0;
}

/* Action 5 replenishes listeners for configured disconnected peers, including
 * an offline TX that wakes after the bounded outbound retries have stopped.
 * Listening does not scan or rotate bonds. Manual disconnect stays held. */
static void maintain_dg_listeners(void)
{
    if (E.offline || !E.auto_connect || !bb_base || !p_dg_listen) return;
    unsigned needed = 0;
    for (unsigned slot = 0; slot < 2; slot++) {
        struct mic_tx *tx = &TX[slot];
        if (tx->cfg_bd_valid && tx->paired && !tx->user_hold &&
                !(dg_target_state(tx->cfg_bd) & 2)) needed++;
    }
    if (needed) ensure_idle_dg_listeners(needed);
}

static void ensure_dg_visibility(void)
{
    /* The product boots with visibility off (wms_start -> bt_set_visible(0)).
     * After every camera reboot the TX cannot see or page us until someone
     * flips discoverable+connectable back on -- do it here, once, before
     * arming the listener. */
    if (!E.visibility_set) {
        E.visibility_set = 1;
        if (p_dm_set_vis) {
            p_dm_set_vis(1, 1);
            mlog("connect: app_dm_set_visibility(1,1) restored");
        }
        /* The caller owns CONNECTING/RECONNECTING and its retry budget;
         * restoring visibility must not reset that phase to IDLE. */
    }
}

static int ensure_dg_listener(struct mic_tx *tx)
{
    if (!bb_base || !p_dg_listen) return -1;
    ensure_dg_visibility();

    /* ACL-aware connect: if the TX already has a live ACL to us
     * (verified via RSSI probe), do NOT tear it down.  The engine's
     * disconnect cycle used to kill the ACL the TX just established,
     * then reconnect from scratch -- fighting the TX's own connect
     * attempt.  On a warm ACL, open SPP directly. */
    {
        unsigned char rp[MAXPL];
        static int warm_attempts = 0;
        int np = bsa_sync(0x582, tx->cfg_bd, 6, rp, sizeof rp, 1500);
        int acl_up = (np >= 12 && rd16b(rp) == 0);
        if (acl_up) {
            int rssi = (int)(int8_t)rp[10];
            warm_attempts++;
            mlog("connect: ACL alive (rssi=%d) -> warm path #%d, skip teardown",
                 rssi, warm_attempts);
            pthread_mutex_lock(&E.st_mtx);
            tx_arm_bootstrap_locked(tx);
            pthread_mutex_unlock(&E.st_mtx);
            int drc = dg_open_once(tx->cfg_bd);
            mlog("connect: warm app_dg_open_ex2 = %d", drc);
            if (drc != 0) {
                pthread_mutex_lock(&E.st_mtx);
                tx->kick_stage = 0;
                pthread_mutex_unlock(&E.st_mtx);
            }
            return drc;
        }
        warm_attempts = 0;  /* ACL down = reset counter */
    }
    return ensure_idle_dg_listeners(1);
}

/* Background recovery follows A5's listener replenishment. Paging an absent
 * bonded TX repeatedly can starve the other audio link even though that link
 * remains connected. Do not probe RSSI or request a warm/cold open here.
 * Initial single-peer startup and explicit user connects keep their dial path;
 * a lost stream, or an absent second peer beside healthy audio, waits inbound.
 * control_mtx held; native calls happen outside feed/st/record locks. */
static int tx_background_listen(struct mic_tx *tx, uint64_t now)
{
    if (E.offline || !bb_base || !p_dg_listen || !bb_stack_inited() ||
            !tx->cfg_bd_valid || !tx->paired || tx->user_hold || dg_target_state(tx->cfg_bd)) return 0;
    struct mic_tx *other = &TX[(unsigned)(tx - TX) ^ 1];
    pthread_mutex_lock(&E.st_mtx);
    int eligible = !tx->connected && (tx->stream_lost ||
        (other->connected && !other->stream_lost && !other->user_hold && other->last_audio_ms &&
         now >= other->last_audio_ms && now - other->last_audio_ms <= 5000));
    pthread_mutex_unlock(&E.st_mtx);
    if (!eligible) return 0;
    ensure_dg_visibility();
    int rc = ensure_idle_dg_listeners(1);
    pthread_mutex_lock(&E.st_mtx);
    /* A native OPEN/audio callback may have won while listen was executing. */
    if (!tx->connected) {
        tx->dial_pending = 0; tx->dial_until_ms = 0;
        tx->kick_stage = tx->kick_tries = 0;
        if (!rc) {
            tx->state = ST_IDLE; tx->device_offline = 1;
            tx->autocmd_pending = 0;
        } else {
            /* Keep the listener retry bounded by the housekeeper cadence.
             * A listen failure does not authorize a disruptive outbound page. */
            tx->state = ST_RECONNECTING;
            tx->retry_gate_ms = now + 1000;
        }
    }
    pthread_mutex_unlock(&E.st_mtx);
    if (!rc) mlog("recovery: TX%u listening for bonded peer; no background page", (unsigned)(tx - TX) + 1);
    return 1;
}

/* A successful bond must progress to SPP even in passive mode. Give
 * the TX/stock HFP branch its existing 12s head start, then use the
 * SAME bt_bsa_app session for DgOpen (Action 5 WMS 0x11 -> 0x13). */
static void schedule_after_pair(struct mic_tx *tx)
{
    pthread_mutex_lock(&E.st_mtx);
    /* Applies in PASSIVE mode too.  The old `!E.passive` guard assumed the
     * TX always dials in after a fresh bond (03:14 session); live 10-03
     * 04:32 (stock stack) and 10-04 00:55 showed it does NOT -> bond OK,
     * then nothing ever opened SPP and the state sat in "connecting".
     * The stage-4 handler re-checks state != STREAMING after the 12 s
     * window, so a TX that does dial in is never stomped. */
    if (tx->state != ST_STREAMING && !tx->user_hold) {
        tx->state = ST_CONNECTING;
        tx->connect_ms = now_ms();
        tx->dial_until_ms = tx->connect_ms + 30000;
        tx->dial_pending = 1;
        tx->kick_stage = 4;
        tx->kick_tries = 1;
        tx->kick_ms = tx->connect_ms + 12000;
        pthread_mutex_unlock(&E.st_mtx);
        mlog("bond complete: 12s branch window, then 0x13-style direct open");
        return;
    }
    pthread_mutex_unlock(&E.st_mtx);
}

/* control_mtx held. HFP rejection or an expired branch window must not leave
 * a paired TX waiting forever. Request SPP without requiring an existing ACL;
 * BSA owns paging and dg_open_once reuses any pending/open native slot. */
static int tx_request_spp(struct mic_tx *tx, const char *reason)
{
    pthread_mutex_lock(&E.st_mtx);
    if (tx->connected || tx->state == ST_STREAMING) {
        pthread_mutex_unlock(&E.st_mtx);
        return 0;
    }
    tx_arm_bootstrap_locked(tx);
    pthread_mutex_unlock(&E.st_mtx);
    int rc = p_dg_open && tx->cfg_bd_valid ? dg_open_once(tx->cfg_bd) : -1;
    mlog("connect: TX%d %s -> SPP request = %d", (int)(tx - TX) + 1, reason, rc);
    pthread_mutex_lock(&E.st_mtx);
    if (tx->connected || tx->state == ST_STREAMING) {
        pthread_mutex_unlock(&E.st_mtx);
        return 0; /* Incoming audio won the native call; retain its state. */
    }
    if (!rc) {
        tx->dial_pending = 1;
        tx->dial_until_ms = tx->connect_ms + (tx->connect_fails < 3 ? 30000 : 120000);
    } else {
        tx->kick_stage = 0;
        tx->state = ST_IDLE;
        tx->connect_ms = tx->dial_until_ms = 0;
        tx->dial_pending = 0;
        tx->idle_since_ms = tx->idle_try_ms = now_ms();
        tx->connect_fails++;
        if (tx->connect_fails >= 3 || tx->bond_fails >= 3) tx->device_offline = 1;
        mlog("connect: TX%d SPP rejected (fail %d/3, offline=%d); bond retained",
             (int)(tx - TX) + 1, tx->connect_fails, tx->device_offline);
    }
    pthread_mutex_unlock(&E.st_mtx);
    return rc ? -1 : 0;
}

static int tx_connect(struct mic_tx *tx)
{
    int rc;
    if (E.offline) return -1;
    if (tx->dg_close_pending) {
        if (!bb_base || dg_target_state(tx->cfg_bd)) return -4;
        tx->dg_close_pending = 0;
    }
    if (!p_dg_open && E.standalone) {
        /* v4 micd: the link is driven by bt_bsa_app's own DJI branch
         * (TX HFP connect -> AG close -> app_dg_open_ex2). Nothing to
         * call; just wait for the stream on @/dsock/57012. */
        pthread_mutex_lock(&E.st_mtx);
        if (tx->state == ST_CONNECTING || tx->state == ST_STREAMING) {
            rc = tx->state == ST_CONNECTING ? 2 : 1;
            pthread_mutex_unlock(&E.st_mtx);
            return rc;
        }
        tx->state = ST_CONNECTING;
        tx->connect_ms = now_ms();
        tx->dg_event_after = mic_dg_event_cursor();
        pthread_mutex_unlock(&E.st_mtx);
        tx->have_seq = 0;
        mlog("connect: STANDALONE (stock HFP->SPP branch); waiting for TX");
        return 0;
    }
    if (!p_dg_open) {
        mlog("connect: no bt_bsa_app in-process API");
        return -1;
    }
    if (!tx->cfg_bd_valid) {
        mlog("connect: no bd configured");
        return -1;
    }
    if (tx_dial_blocked(tx, now_ms()) && tx->state != ST_STREAMING && tx->state != ST_CONNECTING) {
        int key = xml_key_state(tx->cfg_bd);
        if (key <= 0) return key < 0 ? -6 : -3;
        pthread_mutex_lock(&E.st_mtx);
        tx->state = ST_IDLE;
        tx->connect_ms = tx->dial_until_ms = 0;
        tx->kick_stage = tx->kick_tries = 0;
        tx->autocmd_pending = 2;
        tx->autocmd_at = now_ms() + 1000;
        pthread_mutex_unlock(&E.st_mtx);
        return 3; /* accepted, but no dial and no retry budget spent yet */
    }
    if (!bb_stack_inited()) {
        if (p_app_init && !E.app_init_done) {
            E.app_init_done = 1;
            mlog("connect: stack not inited -> bootstrap app_init()");
            int arc = p_app_init();
            *(uint8_t *)((char *)bb_base + OFF_INITED) = 1;  /* like cmd 0x00 handler */
            mlog("connect: app_init() = %d inited=%d", arc, bb_stack_inited());
        }
        if (!bb_stack_inited()) {
            mlog("connect: stack still not inited");
            return -1;
        }
    }
    pthread_mutex_lock(&E.st_mtx);
    if (tx->state == ST_CONNECTING || tx->state == ST_STREAMING) {
        pthread_mutex_unlock(&E.st_mtx);
        /* v3: distinguish "engine already connecting" from "busy
         * streaming" so the ctl `connect` answer is informative when it
         * collides with the idle auto-retry ladder */
        return tx->state == ST_CONNECTING ? 2 : 1;
    }
    tx->state = ST_CONNECTING;
    tx->connect_ms = now_ms();
    tx->dg_event_after = mic_dg_event_cursor();
    tx->dial_until_ms = tx->connect_ms + 30000;
    tx->dial_pending = 1;
    tx->autocmd_pending = 0;
    pthread_mutex_unlock(&E.st_mtx);
    tx->have_seq = 0;

    /* Reconnect only uses a confirmed existing bond. Unreadable records are
     * temporarily unavailable, not a request to rotate/re-pair a key. */
    int key = xml_key_state(tx->cfg_bd);
    if (key <= 0) {
        pthread_mutex_lock(&E.st_mtx);
        tx->state = ST_IDLE;
        tx->dial_until_ms = 0;
        tx->dial_pending = 0;
        pthread_mutex_unlock(&E.st_mtx);
        return key < 0 ? -6 : -3;
    }
    /* Reuse an existing/pending SPP or open directly on a warm ACL.
     * Re-engaging HFP here can trigger another native DgOpen in parallel. */
    if (dg_target_state(tx->cfg_bd) || (!E.passive && p_ag_open && acl_is_up(tx->cfg_bd))) {
        return tx_request_spp(tx, "warm link");
    }
    if (E.passive) {
        if (ensure_dg_listener(tx) != 0) {
            pthread_mutex_lock(&E.st_mtx);
            tx->state = ST_IDLE;
            tx->dial_until_ms = 0;
            pthread_mutex_unlock(&E.st_mtx);
            return -1;
        }
        tx->kick_stage = 0;          /* responders own the dance, no ladder */
        tx->kick_tries = 0;
        mlog("connect: PASSIVE (branch-driven); waiting for TX dance");
        /* same 12 s window + 0x13-style direct open as after a bond: a
         * bonded TX that does not dial in must still get an SPP */
        schedule_after_pair(tx);
        return 0;
    }
    /* 316: TX enters via HFP -> native 特判 -> SPP.  Two paths:
     * (a) TX dials in on its own (boot / wake / pairing window)
     * (b) We dial the TX's HF endpoint via app_ag_open -> SLC ->
     *     特判 evt fires -> AgClose + DgOpen -> SPP -> streaming
     * Give the native branch a bounded window. Any later SPP request
     * must reuse its existing/pending slot instead of opening it twice. */
    if (p_hfp_set_enable) {
        p_hfp_set_enable(1);
        mlog("connect: bt_hfp_set_enable(1) (AG armed)");
    }
    if (p_ag_open) {
        int ag = p_ag_open(tx->cfg_bd);
        mlog("connect: app_ag_open(bd) = %d (HFP engage -> 特判)", ag);
        if (ag != 0) return tx_request_spp(tx, "HFP rejected");
        tx->kick_stage = 3;
        tx->kick_tries = 1;
        tx->kick_ms = now_ms() + 15000;
        return 0;
    }
    return tx_request_spp(tx, "direct connect");
}

/* Legacy C entry points may select the only configured peer, including TX2.
 * With two peers the socket API must carry an explicit address. */
static int tx_legacy_control(int (*operation)(struct mic_tx *))
{
    if (pthread_mutex_trylock(&control_mtx)) return 1;
    pthread_mutex_lock(&E.st_mtx);
    struct mic_tx *tx = tx_find_bd(NULL);
    pthread_mutex_unlock(&E.st_mtx);
    int result = tx ? operation(tx) : -5;
    pthread_mutex_unlock(&control_mtx);
    return result;
}

int miccore_connect(void)
{
    return tx_legacy_control(tx_connect);
}

/* product stop path (feature_bt_audio_stop@0x489c40): before tearing the
 * link down, tell the TX to sleep: 0x54 {conn_id=0, action=0x20, bd},
 * then a 200 ms grace ("wait for mic disconnect success") so the TX
 * processes the notice -- without it the TX keeps its blue link LED on
 * and can invalidate its link keys. AA ver=0 wrapped, receiver=0x18. */
static void announce_sleep_0x54_20(struct mic_tx *tx)
{
    uint8_t duss[32], aa[48], pl[8];
    int dl, al;
    if (!p_dg_send || !tx->cfg_bd_valid) return;
    pl[0] = 0x00;                       /* conn_id */
    pl[1] = 0x20;                       /* action: sleep/standby */
    memcpy(pl + 2, tx->cfg_bd, 6);
    dl = duss_build_r(duss, 0x54, pl, 8, 0x18);
    al = aa_wrap(aa, sizeof aa, duss, dl);
    if (al <= 0) return;
    {
        int rc = p_dg_send(tx->cfg_bd, aa, al);
        mlog("sleep-announce: 0x54{00,20,bd} send rc=%d", rc);
        hexlog("sleep-announce bytes", aa, al);
    }
    usleep(200 * 1000);                 /* product: 200 ms grace */
}

static int tx_disconnect(struct mic_tx *tx)
{
    int rc = -1;
    int acl_alive = 0;
    unsigned char r[MAXPL];
    int n;

    if (E.offline) return -1;

    /* v5: do NOT tell the TX to sleep on a plain disconnect.  The product
     * 0x54/0x20 standby notice (announce_sleep_0x54_20) puts the TX into
     * a state that app_ag_open/the 特判 can no longer wake over the still
     * -live ACL -- the root cause of "click disconnect, then connect the
     * paired device fails" (live 10-04 06:3x: ACL status=0 but app_ag_open
     * re-engage=1004, 30 s timeout -> idle).  Disconnect now only tears
     * down SPP and leaves the TX awake + ACL up so reconnect reopens the
     * data channel on the existing link.  The sleep notice is reserved for
     * true engine teardown (miccore_shutdown) where the mic should power
     * down.  A deliberate "power off mic" is a separate action, not this. */

    /* Bug1 + v2: go idle FIRST.
     * - stops the housekeeper's 1 Hz RSSI polls (0x582), which interleave
     *   with the TmDisconnect wait on the shared bsa client socket;
     * - clears battery/rssi so status shows null after disconnect. */
    pthread_mutex_lock(&E.st_mtx);
    tx->state = ST_IDLE;
    tx->connected = 0;
    tx->dial_until_ms = 0;
    tx->dial_pending = 0;
    tx->autocmd_pending = 0;
    tx->battery = -1;
    tx->battery_level = -1;
    tx->rssi = 0;
    tx->charging = -1;                    /* v3 TLV fields reset */
    tx->mute = -1;
    tx->recording = -1;
    tx->tx_status_ms = 0;
    if (tx->tx_format_result == 1) tx->tx_format_result = 4;
    memset(&tx->wl2_cap, 0, sizeof tx->wl2_cap);
    wl2_capsync_reset_locked(tx);
    tx->ts_valid = 0;
    tx->ts_58_01_valid = 0;
    pthread_mutex_unlock(&E.st_mtx);
    tx->kick_stage = 0;
    tx->kick_tries = 0;

    if (!tx->cfg_bd_valid) return 0;

    if (bb_base) {
        tx->dg_close_pending = 1;
        rc = dg_close_and_wait(tx->cfg_bd);
        if (rc == 0) {
            tx->dg_close_pending = 0;
            mlog("disconnect: native transport closed; bond retained");
        }
        return rc;
    }

    /* Close SPP without deleting the bond or repeatedly tearing down ACL.
     * The stack owns open DG slots until its close callback retires them. */
    if (p_dg_close) rc = p_dg_close(tx->cfg_bd);
    if (rc == 0) {
        mlog("disconnect: close accepted without native table; awaiting ACL confirmation");
    }
    n = bsa_sync(0x582, tx->cfg_bd, 6, r, sizeof r, 1500);
    if (n >= 2 && rd16b(r) != 0) return 0; /* already disconnected */
    acl_alive = n >= 12 && rd16b(r) == 0;
    if (acl_alive && tx->user_hold) {
        n = bsa_sync(0x583, tx->cfg_bd, 6, r, sizeof r, 4000);
        if (n >= 2 && rd16b(r) == 0) return 0;
    }
    mlog("disconnect: close not confirmed; bond retained");
    return -1;
}

int miccore_disconnect(void)
{
    return tx_legacy_control(tx_disconnect);
}

static int tx_kickstart(struct mic_tx *tx)
{
    uint8_t duss[64], aa[80], pl[16];
    int dl, al;
    int fails = 0;

    if (E.offline || !tx->cfg_bd_valid || !p_dg_send) return -1;
    if (p_ccode) {
        int rc = p_ccode(tx->cfg_bd, 0);   /* ccode 0 = FCC */
        mlog("kickstart: ccode_sync(0) = %d", rc);
    }
    if (p_dg_send) {
        /* version-sync announce 0x65/04 {04, cur_ver=1} -- THE opener TX waits for */
        pl[0] = 0x04; pl[1] = 0x01;
        dl = duss_build(duss, 0x65, pl, 2);
        al = aa_wrap(aa, sizeof aa, duss, dl);
        if (al > 0) {
            int rc = p_dg_send(tx->cfg_bd, aa, al);
            mlog("kickstart: version-sync send = %d (%dB)", rc, al);
            if (rc != 0) fails++;
        }
        /* time-sync request 0x58/01 {01} -- engages TX clock exchange;
         * TX's 0x58/02 answer then drives our 0x56 echo (resp_0x56_from_ts) */
        pl[0] = 0x01;
        dl = duss_build(duss, 0x58, pl, 1);
        al = aa_wrap(aa, sizeof aa, duss, dl);
        if (al > 0) {
            int rc = timed_dg_send(tx->cfg_bd, aa, al);
            mlog("kickstart: time-sync-req send = %d (%dB)", rc, al);
            if (rc != 0) fails++;
        }
        /* sync-addr 0x65/03 {03,02,bd,bd} -- FIXED: subtype prefix was missing */
        pl[0] = 0x03; pl[1] = 0x02;
        memcpy(pl + 2, tx->cfg_bd, 6);
        memcpy(pl + 8, tx->cfg_bd, 6);
        dl = duss_build(duss, 0x65, pl, 2 + 12);
        al = aa_wrap(aa, sizeof aa, duss, dl);
        if (al > 0) {
            int rc = p_dg_send(tx->cfg_bd, aa, al);
            mlog("kickstart: sync-addr send = %d (%dB)", rc, al);
            if (rc != 0) fails++;
        }
        /* link-up 0x54 {01,10,bd} */
        pl[0] = 0x01; pl[1] = 0x10;
        memcpy(pl + 2, tx->cfg_bd, 6);
        dl = duss_build(duss, 0x54, pl, 8);
        al = aa_wrap(aa, sizeof aa, duss, dl);
        if (al > 0) {
            int rc = p_dg_send(tx->cfg_bd, aa, al);
            mlog("kickstart: link-up send = %d (%dB)", rc, al);
            if (rc != 0) fails++;
        }
    }
    return fails;    /* 0 = all frames sent; >0 = DG not open yet (race) */
}

int miccore_kickstart(void)
{
    return tx_legacy_control(tx_kickstart);
}

/* ------------------------------------------------------------------ */
/* control commands (scan / pair / unpair) over bsa daemon socket      */
/* ------------------------------------------------------------------ */
static int scan_results_json(char *resp, int rsz)
{
    int off = 1;
    if (rsz < 3) return -1;
    resp[0] = '[';
    for (int i = 0; i < B.nseen; i++) {
        if (strncmp(B.seen[i].name, "DJI", 3)) continue;
        char name[6 * sizeof B.seen[i].name + 3], entry[512];
        mic_json_string(name, B.seen[i].name);
        int n = snprintf(entry, sizeof entry,
            "%s{\"bd\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"name\":%s,\"rssi\":%d,\"cod\":%u}",
            off > 1 ? "," : "", B.seen[i].bd[0], B.seen[i].bd[1], B.seen[i].bd[2],
            B.seen[i].bd[3], B.seen[i].bd[4], B.seen[i].bd[5], name, B.seen[i].rssi, B.seen[i].cod);
        if (n < 0 || n >= (int)sizeof entry || off + n + 2 > rsz) break;
        memcpy(resp + off, entry, (size_t)n);
        off += n;
    }
    resp[off++] = ']';
    resp[off] = 0;
    return 0;
}

static int ctl_scan(int sec, char *resp, int rsz)
{
    unsigned char p[40], r[MAXPL];
    int n;
    int units = sec < 1 ? 1 : (sec > 48 ? 48 : sec);

    if (bsa_conn() < 0) {
        snprintf(resp, (size_t)rsz, "FAIL no-bsa-socket");
        return -1;
    }
    memset(p, 0, sizeof p);
    *(uint32_t *)p = (uint32_t)units;    /* duration in 1.28s units */
    p[0x20] = 0x01;                      /* BR mode (DJI TX is classic) */
    B.nseen = 0;
    B.scan_mode = 1;                     /* armed before start: records during
                                            sync-response wait are captured */
    B.disc_done = 0;
    n = bsa_sync(0x0C8, p, 40, r, sizeof r, 5000);
    if (n < 2 || rd16b(r) != 0) {
        B.scan_mode = 0;
        snprintf(resp, (size_t)rsz, "FAIL disc-start-status=%u", n >= 2 ? rd16b(r) : 0xFFFF);
        return -1;
    }
    pthread_mutex_lock(&B.mtx);
    bsa_pump2(units * 1280 + 8000, 1);
    if (!B.disc_done) {
        unsigned char z[4] = {0};
        bsa_send(0x0C9, z, 4);
        bsa_pump2(1500, 1);
    }
    pthread_mutex_unlock(&B.mtx);
    B.scan_mode = 0;
    if (atomic_load(&discovery_cancel)) {
        snprintf(resp, (size_t)rsz, "FAIL canceled");
        return -1;
    }
    return scan_results_json(resp, rsz);
}

static int ctl_pair(const uint8_t *bd, char *resp, int rsz)
{
    unsigned char z2[2] = {0, 0}, r[MAXPL];
    uint64_t deadline;
    int i, nz;

    int key = xml_key_state(bd);
    if (key == 1) {
        snprintf(resp, (size_t)rsz, "OK already-paired");
        return 0;
    }
    if (key < 0) {
        snprintf(resp, (size_t)rsz, "FAIL pairing-database-unavailable");
        return -1;
    }

    pthread_mutex_lock(&B.mtx);
    if (bsa_conn() < 0) {
        pthread_mutex_unlock(&B.mtx);
        snprintf(resp, (size_t)rsz, "FAIL no-bsa-socket");
        return -1;
    }
    /* steal SEC event stream (last-wins), like btman bond */
    bsa_send(0x12C, z2, 2);
    bsa_pump2(500, 0);
    B.bond_ok = 0;
    B.bond_active = 1;
    memcpy(B.bond_target, bd, 6);
    memset(B.bond_key, 0, 16);
    bsa_send(0x12D, bd, 6);
    deadline = now_ms() + 30000;
    while (!B.bond_ok && !atomic_load(&discovery_cancel) && now_ms() < deadline)
        if (bsa_pump2(1000, 0) < 0 && B.fd < 0) break;
    int canceled = atomic_load(&discovery_cancel);
    if (!B.bond_ok) {
        B.bond_active = 0;
        bsa_send(0x12E, bd, 6); /* SecBondCancel, verified 6-byte address payload */
        bsa_pump2(500, 0);
        pthread_mutex_unlock(&B.mtx);
        snprintf(resp, (size_t)rsz, canceled ? "FAIL canceled" : "FAIL bond-timeout");
        return -1;
    }
    B.bond_active = 0;
    if (B.bond_ok != 1) {
        pthread_mutex_unlock(&B.mtx);
        snprintf(resp, (size_t)rsz, "FAIL bond-timeout-or-rejected");
        return -1;
    }
    if (memcmp(B.bond_bd, bd, 6) != 0) {
        pthread_mutex_unlock(&B.mtx);
        snprintf(resp, (size_t)rsz, "FAIL key-for-other-bd");
        return -1;
    }
    nz = 0;
    for (i = 0; i < 16; i++) if (B.bond_key[i]) nz = 1;
    if (!nz) {
        pthread_mutex_unlock(&B.mtx);
        /* Reused key: retain the live database, never resurrect a possibly
         * revoked or superseded key from the diagnostic backup. */
        if (xml_has_key(bd)) {
            snprintf(resp, (size_t)rsz, "OK already-paired");
            struct mic_tx *tx = tx_find_bd(bd);
            if (tx) update_paired(tx);
            return 0;
        }
        snprintf(resp, (size_t)rsz, "FAIL no-link-key");
        return -1;
    }
    if (persist_key(bd, B.bond_key, B.bond_key_type,
                    B.bond_name[0] ? B.bond_name : NULL) != 0) {
        pthread_mutex_unlock(&B.mtx);
        snprintf(resp, (size_t)rsz, "FAIL xml-persist");
        return -1;
    }
    memset(B.bond_key, 0, sizeof B.bond_key);
    pthread_mutex_unlock(&B.mtx);
    snprintf(resp, (size_t)rsz, "OK paired");
    /* refresh engine name + paired flag */
    struct mic_tx *tx = tx_find_bd(bd);
    if (tx) update_paired(tx);
    return 0;
    (void)r;
}

static int select_paired_device(struct mic_tx *tx, const uint8_t *bd);

static int ctl_unpair(const uint8_t *bd, char *resp, int rsz)
{
    unsigned char p[7], r[MAXPL];
    int n;
    struct mic_tx *tx = tx_find_bd(bd);
    int ours = tx != NULL;
    pthread_mutex_lock(&E.rec_mtx);
    int recording = ours && rec_active_locked() && rec_uses_slot_locked((unsigned)(tx - TX));
    if (ours && !recording) {
        pthread_mutex_lock(&E.st_mtx);
        tx->user_hold = 1;
        tx_admission_update(tx);
        pthread_mutex_unlock(&E.st_mtx);
    }
    pthread_mutex_unlock(&E.rec_mtx);
    if (recording) {
        snprintf(resp, (size_t)rsz, "FAIL recording-active");
        return -1;
    }
    memcpy(p, bd, 6);
    p[6] = 1;
    /* If this is our live device, confirm transport closure before
     * removing its bond, exactly like disconnect. */
    if (ours && !E.offline) {
        /* Keep background reconnect suspended while removing this device. */
        mlog("unpair: user_hold=1");
        if (tx_disconnect(tx) != 0) {
            snprintf(resp, (size_t)rsz, "FAIL disconnect-failed");
            return -1;
        }
    }
    n = bsa_sync(0x134, p, 7, r, sizeof r, 4000);
    if (n < 2 || rd16b(r) != 0) {
        snprintf(resp, (size_t)rsz, "FAIL sec-remove-status=%u", n >= 2 ? rd16b(r) : 0xFFFF);
        return -1;
    }
    int xr = xml_remove_device(bd);
    if (ours) {
        /* v2: unpaired -> battery/rssi/name cleared, paired=false */
        pthread_mutex_lock(&E.st_mtx);
        /* SecRemove succeeded: explicit removal is authoritative even if
         * the on-disk snapshot cannot be edited/read at this instant. */
        tx->paired = 0;
        tx_admission_update(tx);
        tx->battery = -1;
        tx->battery_level = -1;
        tx->rssi = 0;
        tx->name[0] = 0;
        tx->charging = -1;                /* v3 TLV fields reset */
        tx->mute = -1;
        tx->recording = -1;
        tx->tx_status_ms = 0;
        if (tx->tx_format_result == 1) tx->tx_format_result = 4;
        pthread_mutex_unlock(&E.st_mtx);
        if (!xr) update_paired(tx);
    }
    if (!xr && ours && select_paired_device(tx, NULL)) {
        snprintf(resp, (size_t)rsz, "FAIL config-persist");
        return -1;
    }
    snprintf(resp, (size_t)rsz, xr == 0 ? "OK" : "FAIL xml-remove");
    return xr;
}

/* ------------------------------------------------------------------ */
/* control socket server                                               */
/* ------------------------------------------------------------------ */
static int parse_bd_arg(const char *s, uint8_t *bd)
{
    char tmp[13];
    int i, j = 0;
    size_t len = strlen(s);
    if (len != 12 && len != 17) return -1;
    for (i = 0; s[i]; i++) {
        if (len == 17 && i % 3 == 2) {
            if ((s[i] != ':' && s[i] != '-') || s[i] != s[2]) return -1;
        } else {
            if (!isxdigit((unsigned char)s[i]) || j == 12) return -1;
            tmp[j++] = s[i];
        }
    }
    tmp[j] = 0;
    if (j != 12) return -1;
    for (i = 0; i < 6; i++) {
        int hi = toupper((unsigned char)tmp[2 * i]);
        int lo = toupper((unsigned char)tmp[2 * i + 1]);
        int h = (hi >= '0' && hi <= '9') ? hi - '0' : (hi >= 'A' && hi <= 'F') ? hi - 'A' + 10 : -1;
        int l = (lo >= '0' && lo <= '9') ? lo - '0' : (lo >= 'A' && lo <= 'F') ? lo - 'A' + 10 : -1;
        if (h < 0 || l < 0) return -1;
        bd[i] = (uint8_t)((h << 4) | l);
    }
    return 0;
}

/* Match the existing wlmicd configuration: bd=/bd0= are TX1; bd1= is TX2. */
static int config_address_slot(const char *line, unsigned *key_length)
{
    if (!strncasecmp(line, "bd=", 3)) { *key_length = 3; return 0; }
    if (!strncasecmp(line, "bd0=", 4)) { *key_length = 4; return 0; }
    if (!strncasecmp(line, "bd1=", 4)) { *key_length = 4; return 1; }
    return -1;
}

/* Persist a slot identity (NULL frees the slot). Bonds are managed separately.
 * Called with control_mtx held; preserve the other slot and unrelated options. */
static int select_paired_device(struct mic_tx *tx, const uint8_t *bd)
{
    unsigned slot = (unsigned)(tx - TX);
    int length = 0, result = -1;
    char *old = NULL, *replacement = NULL;
    const char *key = slot ? "bd1=" : "bd=";
    /* Mapping and an owner's target must remain stable across persistence.
     * No disconnect of the other slot and no removal of either bond. */
    pthread_mutex_lock(&E.feed_mtx);
    pthread_mutex_lock(&E.rec_mtx);
    pthread_mutex_lock(&E.st_mtx);
    int same = bd && tx->cfg_bd_valid && !memcmp(tx->cfg_bd, bd, 6);
    int duplicate = bd && TX[slot ^ 1].cfg_bd_valid && !memcmp(TX[slot ^ 1].cfg_bd, bd, 6);
    int busy = !same && (tx->connected || tx->state == ST_CONNECTING ||
        (tx->cfg_bd_valid && dg_target_state(tx->cfg_bd)) ||
        (rec_active_locked() && rec_uses_slot_locked(slot)));
    pthread_mutex_unlock(&E.st_mtx);
    if (duplicate || busy) goto done;
    old = xml_load(CFG_PATH, &length);
    if (!old && access(CFG_PATH, F_OK) == 0) goto done;
    replacement = calloc(1, (size_t)length + 80);
    if (!replacement) goto done;
    int used = 0;
    for (int pos = 0; pos < length;) {
        int end = pos;
        while (end < length && old[end] != '\n') end++;
        if (end < length) end++;
        unsigned key_len;
        if (config_address_slot(old + pos, &key_len) != (int)slot) {
            memcpy(replacement + used, old + pos, (size_t)(end - pos));
            used += end - pos;
        }
        pos = end;
    }
    if (used && replacement[used - 1] != '\n') replacement[used++] = '\n';
    if (bd) used += snprintf(replacement + used, 80, "%s%02x:%02x:%02x:%02x:%02x:%02x\n",
                            key, bd[0], bd[1], bd[2], bd[3], bd[4], bd[5]);
    if (xml_save(CFG_PATH, replacement, used)) goto done;
    pthread_mutex_lock(&E.st_mtx);
    if (!same) {
        memset(tx, 0, sizeof *tx);
        tx->battery = tx->battery_level = tx->charging = tx->mute = tx->recording = tx->capacity = -1;
        tx->mic_gain = -128; tx->prev_state = -1;
        TS[slot].span_count = 0;
    }
    if (bd) { memcpy(tx->cfg_bd, bd, 6); tx->cfg_bd_valid = 1; }
    tx->dg_event_after = mic_dg_event_cursor();
    tx->device_offline = tx->user_hold = 0;
    tx->connect_fails = tx->bond_fails = 0;
    tx_admission_update(tx);
    pthread_mutex_unlock(&E.st_mtx);
    /* Replacing a transmitter must not reuse its predecessor's PLC history.
     * feed_mtx excludes the decoder while it is reinitialized. */
    if (!same && decoders[slot].context && engine_decoder_init(slot))
        mlog("config: TX%u decoder reset failed; audio disabled", slot + 1);
    update_paired(tx);
    result = 0;
done:
    free(replacement); free(old);
    pthread_mutex_unlock(&E.rec_mtx);
    pthread_mutex_unlock(&E.feed_mtx);
    return result;
}

/* control_mtx serializes config writers; rec_mtx serializes with start. */
static int set_channel_mode(unsigned mode)
{
    int length = 0, result = -1, used = 0;
    char *old = NULL, *replacement = NULL;
    pthread_mutex_lock(&E.feed_mtx);
    pthread_mutex_lock(&E.rec_mtx);
    if (rec_active_locked()) { result = -2; goto done; }
    old = xml_load(CFG_PATH, &length);
    if (!old && access(CFG_PATH, F_OK) == 0) goto done;
    replacement = calloc(1, (size_t)length + 32);
    if (!replacement) goto done;
    for (int pos = 0; pos < length;) {
        int end = pos;
        while (end < length && old[end] != '\n') end++;
        if (end < length) end++;
        if (strncasecmp(old + pos, "channel_mode=", 13)) {
            memcpy(replacement + used, old + pos, (size_t)(end - pos));
            used += end - pos;
        }
        pos = end;
    }
    if (used && replacement[used - 1] != '\n') replacement[used++] = '\n';
    used += snprintf(replacement + used, 32, "channel_mode=%u\n", mode);
    if (xml_save(CFG_PATH, replacement, used)) goto done;
    E.channel_mode = mode;
    result = 0;
done:
    free(old); free(replacement);
    pthread_mutex_unlock(&E.rec_mtx);
    pthread_mutex_unlock(&E.feed_mtx);
    return result;
}

#include "mic_sessions.h"

static void ctl_handle_line_impl(char *line, char *resp, int rsz)
{
    char *av[8];
    int an = 0;
    char *tok, *save = NULL;

    for (tok = strtok_r(line, " \t\r\n", &save); tok && an < 8;
         tok = strtok_r(NULL, " \t\r\n", &save))
        av[an++] = tok;
    if (an == 0) { snprintf(resp, (size_t)rsz, "ERR empty"); return; }

    struct mic_tx *tx = NULL;
    if (!strcmp(av[0], "connect") || !strcmp(av[0], "disconnect") || !strcmp(av[0], "kickstart")) {
        uint8_t bd[6];
        if (an > 2 || (an == 2 && parse_bd_arg(av[1], bd))) {
            snprintf(resp, (size_t)rsz, "FAIL bad-bd"); return;
        }
        pthread_mutex_lock(&E.st_mtx);
        tx = tx_find_bd(an == 2 ? bd : NULL);
        pthread_mutex_unlock(&E.st_mtx);
        if (!tx) { snprintf(resp, (size_t)rsz, "FAIL target-required"); return; }
    }

    if (!strcmp(av[0], "status")) {
        int result;
        uint8_t bd[6];
        if (an > 2 || (an == 2 && parse_bd_arg(av[1], bd))) {
            snprintf(resp, (size_t)rsz, "FAIL bad-bd"); return;
        }
        result = an == 1 ? miccore_status_json(resp, rsz) : status_json_for_address(bd, resp, rsz);
        if (result < 0) snprintf(resp, (size_t)rsz,
            result == -2 ? "FAIL unknown-target" : "FAIL status-too-large");
    } else if (!strcmp(av[0], "status_all")) {
        if (an != 1) snprintf(resp, (size_t)rsz, "FAIL unexpected-argument");
        else if (status_all_json(resp, rsz) < 0) snprintf(resp, (size_t)rsz, "FAIL status-too-large");
    } else if (!strcmp(av[0], "recording_input")) {
        if (an != 1) snprintf(resp, (size_t)rsz, "FAIL unexpected-argument");
        else if (recording_input_json(resp, (size_t)rsz) < 0)
            snprintf(resp, (size_t)rsz, "FAIL observation-too-large");
    } else if (!strcmp(av[0], "recording_timeline")) {
        uint64_t values[2] = {0};
        int valid = an == 3;
        for (int i = 0; valid && i < 2; i++) {
            char *end = NULL;
            errno = 0;
            values[i] = strtoull(av[i + 1], &end, 10);
            valid = isdigit((unsigned char)av[i + 1][0]) && !errno && end && !*end;
        }
        if (!valid || recording_timeline_json(resp, (size_t)rsz, values[0], values[1]) < 0)
            snprintf(resp, (size_t)rsz, "FAIL invalid-recording-timeline");
    } else if (!strcmp(av[0], "channel_mode")) {
        if (an != 2 || strlen(av[1]) != 1 || av[1][0] < '1' || av[1][0] > '3') {
            snprintf(resp, (size_t)rsz, "FAIL bad-channel-mode"); return;
        }
        int result = set_channel_mode((unsigned)(av[1][0] - '0'));
        snprintf(resp, (size_t)rsz, result == -2 ? "FAIL recording-active" :
            result ? "FAIL config-persist" : "OK");
    } else if (!strcmp(av[0], "timing") && an == 2) {
        char *end = NULL;
        errno = 0;
        unsigned long long after = strtoull(av[1], &end, 10);
        if (!isdigit((unsigned char)av[1][0]) || errno || !end || *end ||
            timing_page(resp, (size_t)rsz, after) < 0)
            snprintf(resp, (size_t)rsz, "FAIL invalid-timing-cursor");
    } else if (!strcmp(av[0], "use") || !strcmp(av[0], "release") ||
               !strcmp(av[0], "rec_owned") || !strcmp(av[0], "rec_aac_owned") ||
               !strcmp(av[0], "rec_stereo_owned") || !strcmp(av[0], "rec_aac_stereo_owned") || !strcmp(av[0], "stop_owned") ||
               !strcmp(av[0], "owner_status")) {
        session_command(an, av, resp, rsz);
    } else if (!strcmp(av[0], "connect")) {
        int rc;
        if (tx->dg_close_pending && (!bb_base || dg_target_state(tx->cfg_bd))) {
            snprintf(resp, (size_t)rsz, "FAIL disconnect-pending");
            return; /* retain user_hold until the old connection has retired */
        }
        /* Serialize hold -> allow with the whole native OPEN callback. If an
         * OPEN ran under hold, its native audio admission was rejected: retire
         * that transport before resuming instead of warm-reusing an incomplete
         * native audio slot. A later OPEN sees ALLOW from its first check. */
#ifdef MICCORE_DISCOVERED_ABI
        int dg_guard = bb_base && mic_dg.initialized && !mic_dg.failed;
        if (dg_guard) pthread_mutex_lock(&mic_dg.mutex);
#endif
        int held_transport = tx->user_hold && bb_base && dg_target_state(tx->cfg_bd);
        pthread_mutex_lock(&E.st_mtx);
        tx->user_hold = 0;                /* explicit user intent: resume auto */
        tx->device_offline = 0;           /* user clicked connect: clear offline */
        tx->connect_fails = tx->bond_fails = 0;
        tx_admission_update(tx);
        pthread_mutex_unlock(&E.st_mtx);
#ifdef MICCORE_DISCOVERED_ABI
        if (dg_guard) pthread_mutex_unlock(&mic_dg.mutex);
#endif
        if (held_transport) {
            tx->dg_close_pending = 1;
            if (dg_close_and_wait(tx->cfg_bd)) {
                snprintf(resp, (size_t)rsz, "FAIL disconnect-pending");
                return;
            }
            tx->dg_close_pending = 0;
        }
        rc = tx_connect(tx);
        if (rc == 0) snprintf(resp, (size_t)rsz, "OK");
        else if (rc == 3) snprintf(resp, (size_t)rsz, "OK queued");
        else if (rc == 2) snprintf(resp, (size_t)rsz, "OK already-connecting");
        else if (rc == 1) snprintf(resp, (size_t)rsz, "BUSY");
        else if (rc == -3) snprintf(resp, (size_t)rsz, "FAIL pairing-required");
        else if (rc == -4) snprintf(resp, (size_t)rsz, "FAIL disconnect-pending");
        else if (rc == -6) snprintf(resp, (size_t)rsz, "FAIL pairing-database-unavailable");
        else snprintf(resp, (size_t)rsz, "FAIL %s", p_dg_open ? "open-failed" : "no-inproc-api");
    } else if (!strcmp(av[0], "disconnect")) {
        /* hold BEFORE teardown: the hk idle auto-retry used to re-enter
         * CONNECTING ~250 ms after a user disconnect (mic.log 03:28:52) */
        pthread_mutex_lock(&E.st_mtx);
        tx->user_hold = 1;
        tx->device_offline = 1;   /* user says go away: stop auto-retry */
        tx->connect_fails = tx->bond_fails = 0;
        tx_admission_update(tx);
        pthread_mutex_unlock(&E.st_mtx);
        mlog("disconnect: user_hold=1 device_offline=1 (auto-connect off)");
        int rc = tx_disconnect(tx);
        status_write();          /* GUI sees offline immediately */
        snprintf(resp, (size_t)rsz, rc == 0 ? "OK" :
                 tx->dg_close_pending ? "FAIL disconnect-pending" : "FAIL disconnect-failed");
    } else if (!strcmp(av[0], "scan")) {
        ctl_scan(an > 1 ? atoi(av[1]) : 10, resp, rsz);
    } else if (!strcmp(av[0], "pair")) {
        uint8_t bd[6];
        if ((an != 2 && an != 3) || parse_bd_arg(av[1], bd) < 0) {
            snprintf(resp, (size_t)rsz, "FAIL bad-bd"); return;
        }
        if (an == 3 && strcmp(av[2], "1") && strcmp(av[2], "2")) {
            snprintf(resp, (size_t)rsz, "FAIL bad-slot"); return;
        }
        struct mic_tx *existing = tx_find_bd(bd);
        tx = an == 3 ? &TX[av[2][0] - '1'] : existing;
        if (existing && tx != existing) {
            snprintf(resp, (size_t)rsz, "FAIL target-in-other-slot"); return;
        }
        if (!tx) for (unsigned slot = 0; slot < 2; slot++)
            if (!TX[slot].cfg_bd_valid) { tx = &TX[slot]; break; }
        if (!tx) { snprintf(resp, (size_t)rsz, "FAIL slots-full"); return; }
        if (tx_dial_blocked(tx, now_ms())) {
            snprintf(resp, (size_t)rsz, "FAIL other-tx-connecting"); return;
        }
        if (tx->dg_close_pending && (!bb_base || dg_target_state(tx->cfg_bd))) {
            snprintf(resp, (size_t)rsz, "FAIL disconnect-pending"); return;
        }
        int ours = tx->cfg_bd_valid && !memcmp(tx->cfg_bd, bd, 6);
        pthread_mutex_lock(&E.rec_mtx);
        int recording_active = rec_active_locked() && rec_uses_slot_locked((unsigned)(tx - TX));
        pthread_mutex_unlock(&E.rec_mtx);
        if (!ours && (tx->connected || tx->state == ST_CONNECTING || recording_active ||
            (tx->cfg_bd_valid && dg_target_state(tx->cfg_bd)))) {
            snprintf(resp, (size_t)rsz, "FAIL disconnect-current-device-first"); return;
        }
        int held = tx->user_hold;
        /* Install the explicit slot choice before SecBond: Mic 2 can send its
         * opening handshake immediately after AUTH_CMPL, before ctl_pair's
         * waiter returns. Without this mapping the feed drops the new peer.
         * A failed bond keeps the selected slot held for explicit retry;
         * neither this nor replacing a slot removes an existing link key. */
        if (select_paired_device(tx, bd) != 0) {
            snprintf(resp, (size_t)rsz, "FAIL config-persist");
            return;
        }
        if (ctl_pair(bd, resp, rsz) == 0) {
            schedule_after_pair(tx);
        } else {
            pthread_mutex_lock(&E.st_mtx);
            tx->user_hold = ours ? held : 1;
            tx->kick_stage = tx->autocmd_pending = 0;
            tx_admission_update(tx);
            pthread_mutex_unlock(&E.st_mtx);
            if (tx->user_hold && (tx->connected || dg_target_state(bd)))
                tx_disconnect(tx);
        }
    } else if (!strcmp(av[0], "unpair")) {
        uint8_t bd[6];
        if (an < 2 || parse_bd_arg(av[1], bd) < 0)
            snprintf(resp, (size_t)rsz, "FAIL bad-bd");
        else
            ctl_unpair(bd, resp, rsz);
    } else if (!strcmp(av[0], "rec")) {
        pthread_mutex_lock(&usage_mtx);
        if (atomic_load(&engine_stopping)) {
            pthread_mutex_unlock(&usage_mtx);
            snprintf(resp, rsz, "FAIL engine-stopping");
            return;
        }
        int rc = miccore_rec_start(an > 1 ? atoi(av[1]) : 0, an > 2 ? av[2] : NULL);
        if (rc == 0) {
            pthread_mutex_lock(&E.rec_mtx);
            snprintf(resp, (size_t)rsz, "OK %s", E.rec_path);
            pthread_mutex_unlock(&E.rec_mtx);
        } else {
            snprintf(resp, (size_t)rsz, rc == -4 ? "FAIL formatting-active" : "FAIL busy-or-open");
        }
        pthread_mutex_unlock(&usage_mtx);
    } else if (!strcmp(av[0], "stop")) {
        char path[256];
        uint64_t bytes = 0;
        pthread_mutex_lock(&usage_mtx);
        pthread_mutex_lock(&E.rec_mtx);
        int rc = E.rec_owner[0] ? -3 : rec_stop_locked(path, sizeof path, &bytes);
        pthread_mutex_unlock(&E.rec_mtx);
        pthread_mutex_unlock(&usage_mtx);
        if (rc == 0)
            snprintf(resp, (size_t)rsz, "OK %s %llu", path, (unsigned long long)bytes);
        else if (rc == 1)
            snprintf(resp, (size_t)rsz, "OK stopping");
        else if (rc == -3)
            snprintf(resp, (size_t)rsz, "FAIL recording-owned");
        else if (rc == -2)
            snprintf(resp, (size_t)rsz, "FAIL recording-io-error");
        else
            snprintf(resp, (size_t)rsz, "FAIL not-recording");
    } else if (!strcmp(av[0], "tx")) {
        tx_control(an == 3 ? av[1] : "", an == 3 ? av[2] : "", resp, rsz);
    } else if (!strcmp(av[0], "tx_at")) {
        uint8_t bd[6];
        if (an != 4 || strlen(av[1]) != 17 || parse_bd_arg(av[1], bd) < 0)
            snprintf(resp, (size_t)rsz, "FAIL invalid-target");
        else tx_control_target(bd, av[2], av[3], resp, rsz);
    } else if (!strcmp(av[0], "format")) {
        uint8_t bd[6];
        if (an != 3 || strcmp(av[2], "CONFIRM") || strlen(av[1]) != 17
                || parse_bd_arg(av[1], bd) < 0)
            snprintf(resp, (size_t)rsz, "FAIL format-confirmation-required");
        else tx_format(bd, resp, rsz);
    } else if (!strcmp(av[0], "kickstart")) {   /* extension (not in v1 spec) */
        int rc = tx_kickstart(tx);
        snprintf(resp, (size_t)rsz, rc ? "FAIL send-failed" : "OK");
    } else if (!strcmp(av[0], "help")) {        /* extension */
        snprintf(resp, (size_t)rsz,
                 "status [bd]|status_all|connect [bd]|disconnect [bd]|scan <sec>|pair <bd> [1|2]|unpair <bd>|"
                 "rec <sec> [path]|stop|kickstart|quit");
    } else {
        snprintf(resp, (size_t)rsz, "ERR unknown-cmd");
    }
}

static int ctl_lock_command(const char *line)
{
    int rc = pthread_mutex_trylock(&control_mtx);
    if (rc != EBUSY) return rc;
    const char *verb = line + strspn(line, " \t");
    size_t n = strcspn(verb, " \t");
    int link = (n == 7 && !strncmp(verb, "connect", n)) ||
               (n == 10 && !strncmp(verb, "disconnect", n));
    int targeted_setting = n == 5 && !strncmp(verb, "tx_at", n);
    if ((!link && !targeted_setting) || atomic_load(&discovery_operation) || atomic_load(&engine_stopping))
        return EBUSY;
    /* A short native CLOSE/RSSI wait is normal housekeeping, not a rejected
     * user intent. Wait for mutex ownership, without polling or accepting a
     * deferred command. Addressed TX settings recheck target/readiness after
     * acquiring the lock; legacy untargeted settings, inquiry/pairing and other
     * mutations remain fail-fast rather than selecting a new default TX later.
     * Three seconds covers the 2.5 s close callback wait; if maintenance stays
     * busy longer, no state is changed and the caller gets its existing error. */
    struct timespec until;
#ifdef __ANDROID__
    /* Available on our API 28 target; wall-clock corrections must not extend
     * a camera command's wait. Host tests use the portable POSIX equivalent. */
    if (clock_gettime(CLOCK_MONOTONIC, &until)) return EBUSY;
    until.tv_sec += 3;
    rc = pthread_mutex_timedlock_monotonic_np(&control_mtx, &until);
#else
    if (clock_gettime(CLOCK_REALTIME, &until)) return EBUSY;
    until.tv_sec += 3;
    rc = pthread_mutex_timedlock(&control_mtx, &until);
#endif
    if (!rc && atomic_load(&engine_stopping)) {
        pthread_mutex_unlock(&control_mtx);
        return ECANCELED; /* Never execute a command whose wait crossed shutdown. */
    }
    return rc;
}

static void ctl_handle_line(char *line, char *resp, int rsz)
{
    line[strcspn(line, "\r\n")] = 0;
    if (!strcmp(line, "cancel") || !strcmp(line, "cancel\n")) {
        pthread_mutex_lock(&discovery_mtx);
        if (atomic_load(&discovery_operation)) {
            atomic_store(&discovery_cancel, 1);
            snprintf(resp, (size_t)rsz, "OK cancel-requested");
        } else snprintf(resp, (size_t)rsz, "OK idle");
        pthread_mutex_unlock(&discovery_mtx);
        return;
    }
    /* Timing uses MT.mutex only. Do not make a read-only journal compete with
     * Bluetooth maintenance/inquiry (control_mtx), or it returns FAIL busy. */
    if (!strcmp(line, "status") || !strncmp(line, "status ", 7) || !strcmp(line, "status_all")
            || !strncmp(line, "timing ", 7) || !strcmp(line, "recording_input")
            || !strncmp(line, "recording_timeline ", 19)
            || !strcmp(line, "stop") || !strcmp(line, "stop\n")
            || !strncmp(line, "rec_aac_owned ", 14)
            || !strncmp(line, "rec_stereo_owned ", 17) || !strncmp(line, "rec_aac_stereo_owned ", 21)
            || !strncmp(line, "stop_owned ", 11) || !strncmp(line, "owner_status ", 13)
            || !strncmp(line, "use ", 4) || !strncmp(line, "release ", 8)) {
        ctl_handle_line_impl(line, resp, rsz);
        return;
    }
    if (ctl_lock_command(line)) {
        snprintf(resp, (size_t)rsz, "FAIL busy");
        return;
    }
    int discover = !strncmp(line, "scan ", 5) || !strncmp(line, "pair ", 5);
    pthread_mutex_lock(&discovery_mtx);
    atomic_store(&discovery_cancel, 0);
    atomic_store(&discovery_operation, discover);
    pthread_mutex_unlock(&discovery_mtx);
    ctl_handle_line_impl(line, resp, rsz);
    /* A late cancel must not leave a latch that aborts background pairing.
     * Serialize completion with cancel's check-and-set, including failures. */
    pthread_mutex_lock(&discovery_mtx);
    atomic_store(&discovery_operation, 0);
    atomic_store(&discovery_cancel, 0);
    pthread_mutex_unlock(&discovery_mtx);
    pthread_mutex_unlock(&control_mtx);
}

struct ctl_connection {
    int fd, finished;
    pthread_t thread;
    struct ctl_connection *next;
};
static pthread_mutex_t ctl_clients_mtx = PTHREAD_MUTEX_INITIALIZER;
static struct ctl_connection *ctl_clients;
static int ctl_listener = -1, ctl_running;

/* Accept loop is the sole normal reaper; shutdown reaps after joining it. */
static void ctl_reap(int all)
{
    for (;;) {
        pthread_mutex_lock(&ctl_clients_mtx);
        struct ctl_connection **link = &ctl_clients;
        while (*link && !all && !(*link)->finished) link = &(*link)->next;
        struct ctl_connection *client = *link;
        if (client) *link = client->next;
        pthread_mutex_unlock(&ctl_clients_mtx);
        if (!client) break;
        pthread_join(client->thread, NULL);
        free(client);
    }
}

static void *ctl_conn_thread(void *arg)
{
    struct ctl_connection *client = arg;
    int fd = client->fd;
    char line[1024], resp[32768]; /* status_all contains two complete peer snapshots */
    FILE *f = fdopen(fd, "r+");

    if (f) setvbuf(f, NULL, _IONBF, 0);
    while (f && !atomic_load(&engine_stopping) && fgets(line, sizeof line, f)) {
        if (atomic_load(&engine_stopping)) break;
        if (!strncasecmp(line, "quit", 4)) break;
        ctl_handle_line(line, resp, sizeof resp);
        fprintf(f, "%s\n", resp);
        fflush(f);
    }
    pthread_mutex_lock(&ctl_clients_mtx);
    client->fd = -1;
    if (f) fclose(f); else close(fd);
    client->finished = 1;
    pthread_mutex_unlock(&ctl_clients_mtx);
    return NULL;
}

static int ctl_launch_connection(int fd)
{
    struct ctl_connection *client = calloc(1, sizeof *client);
    if (!client) { close(fd); return -1; }
    client->fd = fd;
    pthread_mutex_lock(&ctl_clients_mtx);
    int rc = atomic_load(&engine_stopping) ? ECANCELED
        : pthread_create(&client->thread, NULL, ctl_conn_thread, client);
    if (!rc) { client->next = ctl_clients; ctl_clients = client; }
    pthread_mutex_unlock(&ctl_clients_mtx);
    if (rc) { close(fd); free(client); }
    return rc ? -1 : 0;
}

static void ctl_stop(void)
{
    if (ctl_running) {
        shutdown(ctl_listener, SHUT_RDWR);
        pthread_join(E.ctl_thread, NULL);
        close(ctl_listener);
        ctl_listener = -1;
        ctl_running = 0;
    }
    pthread_mutex_lock(&ctl_clients_mtx);
    for (struct ctl_connection *c = ctl_clients; c; c = c->next)
        if (c->fd >= 0) shutdown(c->fd, SHUT_RDWR);
    pthread_mutex_unlock(&ctl_clients_mtx);
    ctl_reap(1);
}

static void *ctl_accept_thread(void *arg)
{
    int lfd = (int)(intptr_t)arg;

    (void)arg;
    while (!atomic_load(&engine_stopping)) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            mlog("ctl accept: %s", strerror(errno));
            break;
        }
        ctl_reap(0);
        ctl_launch_connection(cfd);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* housekeeper                                                         */
/* ------------------------------------------------------------------ */
static void bt_audio_cfg_sync(void);

/* Called under st_mtx. "offline" means no usable audio, not proof that
 * the TX was powered off: range loss and transport failure look the same. */
static void stream_health_tick(struct mic_tx *tx, uint64_t now)
{
    if (tx->last_audio_ms && now > tx->last_audio_ms && now - tx->last_audio_ms > 5000) {
        tx->connected = 0;
        if (tx->state == ST_STREAMING) {
            tx->stream_lost = 1;
            tx->level = 0;
            tx->state = ST_RECONNECTING;
            tx->retries = 0;
            tx->kick_stage = tx->kick_tries = 0;
            tx->ts_valid = tx->ts_58_01_valid = 0;
            tx->retry_gate_ms = now; /* five seconds silent already paid the grace */
            mlog("stream unavailable for 5s -> offline; bounded recovery scheduled");
        }
    }
    if (tx->state == ST_RECONNECTING && tx->retries >= 3 && now >= tx->retry_gate_ms) {
        tx->state = ST_IDLE;
        tx->device_offline = 1;
        tx->kick_stage = tx->kick_tries = 0;
        mlog("device_offline: stream recovery exhausted; stop retries, bond retained");
    }
}

static int stream_reconnect_due(struct mic_tx *tx, uint64_t now)
{
    if (tx->state != ST_RECONNECTING || tx->user_hold || tx->device_offline ||
        tx->retries >= 3 || now < tx->retry_gate_ms) return 0;
    tx->retries++;
    tx->retry_gate_ms = now + 20000; /* each attempt gets its full callback window */
    return 1;
}

/* control_mtx held. Each return ends only this peer's tick. */
static void tx_housekeep(struct mic_tx *tx, uint64_t now)
{
    dg_table_edge(tx, now);
    /* Honor the one-time boot request and accepted queued dials even if
     * another TX starts streaming first. Later background recovery still
     * uses passive listening to avoid repeatedly paging an absent peer. */
    if (tx->autocmd_pending &&
        (tx->autocmd_at == 0 || now >= tx->autocmd_at)) {
        tx->autocmd_pending = 0;
        if (!tx->user_hold && tx->cfg_bd_valid && xml_has_key(tx->cfg_bd))
            tx_connect(tx);
    }


    /* v3: idle + auto_connect -> gentle retry. The product keeps
     * the camera in dg_listen forever so a TX can re-connect any time
     * (feature_bt_audio_timer role-switch logic); the engine's
     * equivalent is re-issuing app_dg_open_ex2 until the TX wakes.
     * After >=2 consecutive bond failures the cadence drops to 5 min
     * -- hammering a cold TX with pairing attempts burns its trust. */
    {
        /* anchor the retry interval at IDLE entry: a stale anchor from
         * before a long streaming session fired the retry on the very
         * first idle tick */
        if (tx->state == ST_IDLE && tx->prev_state != ST_IDLE) tx->idle_since_ms = now;
        tx->prev_state = tx->state;
        if (!tx->autocmd_pending && !tx->device_offline && tx->state == ST_IDLE && E.auto_connect &&
            !tx->user_hold && tx->cfg_bd_valid && p_dg_open && xml_has_key(tx->cfg_bd)) {
            uint64_t iv = tx->bond_fails >= 2 ? 300000 : 30000;
            if (tx->idle_try_ms < tx->idle_since_ms) tx->idle_try_ms = tx->idle_since_ms;
            if (now - tx->idle_try_ms >= iv) {
                tx->idle_try_ms = now;
                if (tx_background_listen(tx, now_ms())) return;
                mlog("idle auto-retry connect (TX away? bond_fails=%d)",
                     tx->bond_fails);
                tx_connect(tx);
                return;  /* re-snapshot `now`: connect_ms just moved */
            }
        }
    }


    /* v3: re-snapshot `now` before the time-based state checks. The
     * loop-head `now` can be OLDER than tx->last_audio_ms / connect_ms
     * (the feed thread and mid-loop connect() advance them during the
     * update_paired() file-IO window); unsigned `now - x > N` checks
     * would underflow toward 2^64 and fire spuriously. The same-
     * millisecond "stream stall -> auto-reconnect attempt" pairs in
     * the v9 log carry this bug's signature. */
    now = now_ms();
    int dial_blocked = tx_dial_blocked(tx, now);

    pthread_mutex_lock(&E.st_mtx);
    stream_health_tick(tx, now);

    /* connect that never completes (page timeout / TX asleep).
     * Guard `now > tx->connect_ms`: hk may call tx_connect(tx) mid-
     * loop (autocmd / idle-retry), leaving the loop's stale `now`
     * snapshot BELOW the fresh connect_ms -> unsigned underflow would
     * fire the 30 s timeout in the same millisecond.
     * PASSIVE mode: the TX dance can take minutes -- wait 10 min
     * (waiting is free; there is no half-open of ours to clean up). */
    {
        uint64_t cto = tx->connect_fails < 3 ? 30000 : 120000;
        if (tx->state == ST_CONNECTING && tx->connect_ms &&
            now >= tx->connect_ms && now - tx->connect_ms >= cto) {
            tx->state = ST_IDLE;
            tx->dial_until_ms = 0;
            tx->dial_pending = 0;
            tx->kick_stage = 0;
            tx->kick_tries = 0;
            tx->connect_fails++;
            mlog("connect incomplete after %llus -> idle (fail %d/3)",
                 (unsigned long long)(cto / 1000), tx->connect_fails);
            if (tx->connect_fails >= 3 || tx->bond_fails >= 3) {
                tx->device_offline = 1;
                mlog("device_offline: %d connect fails, %d bond fails "
                     "-> stop auto-retry (click connect to retry)",
                     tx->connect_fails, tx->bond_fails);
                /* Written below, after st_mtx is released. */
            }
        }
    }

    if (tx->state == ST_RECONNECTING) {
        if (!dial_blocked && now >= tx->retry_gate_ms) {
            pthread_mutex_unlock(&E.st_mtx);
            if (tx_background_listen(tx, now)) return;
            pthread_mutex_lock(&E.st_mtx);
        }
        if (!dial_blocked && stream_reconnect_due(tx, now)) {
            tx->dial_until_ms = tx->retry_gate_ms;
            tx->dial_pending = 1;
            mlog("auto-reconnect attempt %d", tx->retries);
            pthread_mutex_unlock(&E.st_mtx);
            if (E.passive) {
                tx->kick_stage = 0;
                if (ensure_dg_listener(tx) != 0) tx->dial_until_ms = 0;

                return;
            }
            pthread_mutex_lock(&E.st_mtx);
            tx_arm_bootstrap_locked(tx);
            pthread_mutex_unlock(&E.st_mtx);
            int rc = p_dg_open && tx->cfg_bd_valid ? dg_open_once(tx->cfg_bd) : -1;
            if (rc != 0) {
                pthread_mutex_lock(&E.st_mtx);
                tx->dial_until_ms = 0;
                tx->kick_stage = 0;
                pthread_mutex_unlock(&E.st_mtx);
            }

            return;
        }
    }

    /* kickstart ladder after connect */
    if (dial_blocked && (tx->kick_stage == 3 || tx->kick_stage == 4)) {
        pthread_mutex_unlock(&E.st_mtx);
        return; /* Native dial deferred; another peer owns the callback window. */
    }
    if (tx->kick_stage == 1 && now >= tx->kick_ms) {
        pthread_mutex_unlock(&E.st_mtx);
        if (bb_base && !(dg_target_state(tx->cfg_bd) & 2)) {
            pthread_mutex_lock(&E.st_mtx);
            if (tx->kick_stage == 1) tx->kick_ms = now_ms() + 1000;
            pthread_mutex_unlock(&E.st_mtx);
            return; /* native OPEN_EVT must precede handshake sends */
        }
        if (p_ccode && tx->cfg_bd_valid) {
            int rc = p_ccode(tx->cfg_bd, 0);
            mlog("kick ladder: ccode(0) = %d", rc);
        }
        pthread_mutex_lock(&E.st_mtx);
        if (tx->kick_stage == 1) {
            tx->kick_stage = 2;
            tx->kick_ms = now_ms() + 3000;
        }
        pthread_mutex_unlock(&E.st_mtx);
        return;
    }
    if (tx->kick_stage == 2 && now >= tx->kick_ms) {
        pthread_mutex_unlock(&E.st_mtx);
        if (bb_base && !(dg_target_state(tx->cfg_bd) & 2)) {
            pthread_mutex_lock(&E.st_mtx);
            if (tx->kick_stage == 2) {
                tx->kick_stage = 1;
                tx->kick_ms = now_ms() + 1000;
            }
            pthread_mutex_unlock(&E.st_mtx);
            return;
        }
        /* A provisional first batch can set STREAMING while this bootstrap
         * remains armed. Sustained receptions retire it in the feed path. */
        {
            int krc = tx_kickstart(tx);
            pthread_mutex_lock(&E.st_mtx);
            if (tx->kick_stage != 2) {
                pthread_mutex_unlock(&E.st_mtx);
                return; /* Audio retired the ladder during the native sends. */
            }
            if (krc != 0 && tx->kick_tries < 8) {
                /* DG entry not opened yet (cold-wake TX: open evt
                 * pending) -> all sends returned -1. Retry until the
                 * ladder actually leaves or 8 tries (~16 s). */
                tx->kick_tries++;
                tx->kick_stage = 2;
                tx->kick_ms = now_ms() + 2000;
                mlog("kick ladder: sends failed (DG not open), retry %d",
                     tx->kick_tries);
                pthread_mutex_unlock(&E.st_mtx);
                return;
            }
            if (krc == 0 && !tx->first_audio_ms)
                tx->first_audio_ms = now_ms();
                /* ladder out: a TX sitting in its registration state
                 * waits for our capability sync before it streams */
            /* The native per-BD version/clock timers own retries and close.
             * Sending frames is not a successful clock response. */
        }
        tx->kick_stage = 0;
        tx->kick_tries = 0;
        pthread_mutex_unlock(&E.st_mtx);
        return;
    }
    if (tx->kick_stage == 4 && now >= tx->kick_ms) {
        tx->dial_until_ms = tx->connect_ms + (tx->connect_fails < 3 ? 30000 : 120000);
        tx->dial_pending = 1;
        pthread_mutex_unlock(&E.st_mtx);
        if (tx->state != ST_STREAMING && !tx->user_hold && p_dg_open && tx->cfg_bd_valid) {
            /* Action5 WMS 0x13 requests DgOpen without an RSSI/ACL
             * prerequisite. Stage 3 could spend the entire post-bond
             * timeout retrying HFP without ever requesting SPP. */
            /* The native branch may already have opened the transport
             * without audio yet. Reuse it instead of opening it twice. */
            tx_request_spp(tx, "post-bond window expired");
        } else {
            tx->kick_stage = 0;
        }
        return;
    }
    if (tx->kick_stage == 3 && now >= tx->kick_ms) {
        tx->dial_until_ms = tx->connect_ms + (tx->connect_fails < 3 ? 30000 : 120000);
        tx->dial_pending = 1;
        pthread_mutex_unlock(&E.st_mtx);
        /* HFP acceptance is asynchronous, not proof of an ACL or SPP.
         * As with the post-bond / A5 0x13 path, let BSA page this peer.
         * dg_open_once preserves a late native pending/open connection. */
        if (tx->state != ST_STREAMING && !tx->user_hold && tx->cfg_bd_valid) {
            tx_request_spp(tx, "HFP window expired");
            return;
        }
        tx->kick_stage = 0;
        return;
    }
    pthread_mutex_unlock(&E.st_mtx);

    /* WL2: capability sync ~2s after the first audio batch */
    wl2_capsync_tick(tx, now);

    /* live telemetry while connected. The 1 Hz 0x582 RSSI poll is the
     * engine's equivalent of the product's BSA_SEC_RSSI_EVT stream:
     * each successful poll replays the 0x55 keepalive leg the product
     * sends per RSSI event (REPORT_ctrl_frames 3). The old 10 s active
     * 0x57 poll is GONE -- 0x57 is answered only when the TX asks. */
    if (!E.offline && tx->connected && tx->cfg_bd_valid) {
        unsigned char r[MAXPL];
        int rn = bsa_sync(0x582, tx->cfg_bd, 6, r, sizeof r, 1500);
        if (rn >= 12 && rd16b(r) == 0) {
            tx->rssi = (int)(int8_t)r[10];
            ka_055_tick(tx);
        }
    }

    /* Safety net: if device is offline, force state to IDLE (some
     * path may have set it back to CONNECTING after the timeout) */
    if (tx->device_offline && tx->state == ST_CONNECTING) {
        tx->state = ST_IDLE;
        tx->kick_stage = 0;
        tx->kick_tries = 0;
    }

}

static void *hk_thread(void *arg)
{
    (void)arg;
    while (!atomic_load(&engine_stopping)) {
        uint64_t cursor = mic_dg_event_cursor();
        uint64_t now = now_ms();
        rec_check_deadline(now);
        /* Pair/unpair/config switches must not race the reconnect ladder.
         * Recording deadlines and read-only status remain live during pairing. */
        if (pthread_mutex_trylock(&control_mtx)) {
            status_write();
            engine_wait_ms(100);
            continue;
        }

        /* v2: track paired state (XML key entry presence) */
        if (!E.offline) {
            for (unsigned slot = 0; slot < 2; slot++) update_paired(&TX[slot]);
            bt_audio_cfg_sync();       /* v4 standalone: stock whitelist */
        }

        /* bootstrap bt_bsa_app's BT stack (stock: WMS cmd 0x00 -> app_init)
         * when the process was started outside the boot chain */
        if (E.need_app_init && !E.app_init_done) {
            E.app_init_done = 1;
            if (p_app_init && !bb_stack_inited()) {
                mlog("bootstrap: stack not inited -> calling app_init()");
                int rc = p_app_init();
                *(uint8_t *)((char *)bb_base + OFF_INITED) = 1;  /* like cmd 0x00 handler */
                mlog("bootstrap: app_init() = %d, inited=%d",
                     rc, bb_stack_inited());
                /* give the stack a moment, then proceed to auto-connect */
                schedule_pending_connects(now_ms() + 3000);
            } else {
                schedule_pending_connects(now_ms());
            }
            E.need_app_init = 0;
        }

        pthread_mutex_lock(&E.feed_mtx);
        if (E.pcm_fd < 0) E.pcm_fd = open(FIFO_PATH, O_WRONLY | O_NONBLOCK);
        pthread_mutex_unlock(&E.feed_mtx);
        for (unsigned slot = 0; slot < 2; slot++) {
            dg_events_drain();
            struct mic_tx *tx = &TX[slot];
            if (tx->cfg_bd_valid || tx->connected || tx->state != ST_IDLE)
                tx_housekeep(tx, now_ms());
        }
        dg_events_drain();
        maintain_dg_listeners();
        status_write();
        pthread_mutex_unlock(&control_mtx);
        if (mic_dg_events_enabled()) mic_dg_event_wait(cursor, 1000);
        else engine_wait_ms(1000);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* config                                                              */
/* ------------------------------------------------------------------ */
static void read_cfg(void)
{
    FILE *f = fopen(CFG_PATH, "r");
    char line[256];

    E.auto_connect = 1;
    E.channel_mode = 2;
    E.ka_055 = 1;                   /* v3: RSSI-event 0x55 chain on by default */
    E.passive = 1;                  /* v3.1: PASSIVE connect mode by default */
    E.manage_bt_audio_cfg = 0;      /* v4: never list the TX (see
                                     * bt_audio_cfg_sync: Connectable:0) */
    TX[0].cfg_bd_valid = TX[1].cfg_bd_valid = 0;
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        unsigned key_len;
        int slot = config_address_slot(line, &key_len);
        if (slot >= 0) {
            char *value = line + key_len;
            value[strcspn(value, "\r\n")] = 0;
            uint8_t bd[6];
            if (!TX[slot].cfg_bd_valid && parse_bd_arg(value, bd) == 0) {
                memcpy(TX[slot].cfg_bd, bd, 6);
                TX[slot].cfg_bd_valid = 1;
            }
        } else if (!strncasecmp(line, "auto_connect=", 13)) {
            E.auto_connect = atoi(line + 13) != 0;
        } else if (!strncasecmp(line, "keepalive_055=", 14)) {
            E.ka_055 = atoi(line + 14) != 0;
        } else if (!strncasecmp(line, "manage_bt_audio_cfg=", 20)) {
            E.manage_bt_audio_cfg = atoi(line + 20) != 0;
        } else if (!strncasecmp(line, "passive=", 8)) {
            E.passive = atoi(line + 8) != 0;
        } else if (!strncasecmp(line, "channel_mode=", 13)) {
            line[strcspn(line, "\r\n")] = 0;
            if (strlen(line + 13) == 1 && line[13] >= '1' && line[13] <= '3')
                E.channel_mode = (unsigned)(line[13] - '0');
        }
    }
    if (TX[0].cfg_bd_valid && TX[1].cfg_bd_valid && !memcmp(TX[0].cfg_bd, TX[1].cfg_bd, 6)) {
        TX[1].cfg_bd_valid = 0;
        mlog("config: duplicate TX address in bd1 ignored");
    }
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */
/* v4: /data/misc/bluetooth/bt_audio.cfg -- the stock mic whitelist that
 * dji_network::_feature_bt_audio_update_config writes after a pair on
 * Action 5 (feature disabled on X2D II, so nobody writes it). bt_bsa_app
 * re-reads it on every use: app_audio_connect_device (0x23988) refuses a
 * TX-initiated (server) SPP whose bd is not listed. One "%02x:..:%02x\n"
 * line per mic (max 2).
 *
 * !! DO NOT ENABLE ON X2D II (manage_bt_audio_cfg=0 is the default) !!
 * wms_bt_cmd_handle cmd 0x00 (0x28acc): master_count()>=1 ->
 * app_dm_set_visibility(0,0) = "Discoverable:0 Connectable:0". Action 5
 * survives that because dji_network then PAGES the mic itself (WMS cmd
 * 0x13); on X2D II nobody pages and the TX cannot page a non-connectable
 * camera -> dead lock (observed 2026-10-03 04:04-04:20, after the wlmicd
 * prototype wrote the file). The working path -- TX HFP connect -> DJI
 * branch -> app_dg_open_ex2 (client, server=0) -- never consults the file.
 * With the option off we only REMOVE a file that lists exactly our bd. */
#define BT_AUDIO_CFG "/data/misc/bluetooth/bt_audio.cfg"
static void bt_audio_cfg_sync(void)
{
    char want[32] = "", have[64] = "";
    FILE *f;
    if (!E.standalone || !TX[0].cfg_bd_valid) return;
    if (TX[0].paired && E.manage_bt_audio_cfg)
        snprintf(want, sizeof want, "%02x:%02x:%02x:%02x:%02x:%02x\n",
                 TX[0].cfg_bd[0], TX[0].cfg_bd[1], TX[0].cfg_bd[2],
                 TX[0].cfg_bd[3], TX[0].cfg_bd[4], TX[0].cfg_bd[5]);
    f = fopen(BT_AUDIO_CFG, "r");
    if (f) {
        size_t n = fread(have, 1, sizeof have - 1, f);
        have[n] = 0;
        fclose(f);
    }
    if (!strcmp(want, have)) return;
    if (!want[0]) {
        /* not wanted: only remove a file naming exactly our bd */
        char ours[32];
        snprintf(ours, sizeof ours, "%02x:%02x:%02x:%02x:%02x:%02x\n",
                 TX[0].cfg_bd[0], TX[0].cfg_bd[1], TX[0].cfg_bd[2],
                 TX[0].cfg_bd[3], TX[0].cfg_bd[4], TX[0].cfg_bd[5]);
        if (!strcmp(have, ours)) {
            unlink(BT_AUDIO_CFG);
            mlog("bt_audio.cfg: removed our bd (a listed mic makes app_init "
                 "set Connectable:0); effective at next BT init");
        }
        return;
    }
    f = fopen(BT_AUDIO_CFG ".tmp", "w");
    if (!f) {
        mlog("bt_audio.cfg: write: %s", strerror(errno));
        return;
    }
    fputs(want, f);
    fclose(f);
    chmod(BT_AUDIO_CFG ".tmp", 0666);
    rename(BT_AUDIO_CFG ".tmp", BT_AUDIO_CFG);
    mlog("bt_audio.cfg: wrote %.17s (stock mic whitelist)", want);
}

/* ------------------------------------------------------------------ */
/* uplink drain: @/dsock/57012 (injection AND standalone)               */
/* + dji_network DUSS relay: @/dsock/57022 -> @/dsock/57011             */
/* ------------------------------------------------------------------ */
/* bt_bsa_app::_bt_audio_send_audio_data (X2D II 0x21150) forwards every
 * accepted SPP audio payload -- after ITS OWN clock(0x58)+version(0x65/04)
 * sync with the TX (flags 0x81ef9 / slot+0x32) -- as one datagram:
 *   {01, seq8, len16be(=n+10), bd[6], data[n]}
 * data = count x 125B units (AA ver0) or the id=1 subpacket body (ver1).
 * On Action 5 libwirelessmicservice binds this port (wl_mic_sw_bluetooth
 * _init(57012, 57011)); the X2D II firmware ships without it, so the
 * port is free and nothing in bt_bsa_app changes when we bind it. */
#ifndef DSOCK_UP
#define DSOCK_UP   "/dsock/57012"
#endif

/* abstract-namespace sockaddr for "\0<name>"; returns the socklen */
static socklen_t dsock_addr(struct sockaddr_un *a, const char *name)
{
    size_t nl = strlen(name);
    memset(a, 0, sizeof *a);
    a->sun_family = AF_UNIX;
    a->sun_path[0] = 0;                      /* abstract namespace */
    memcpy(a->sun_path + 1, name, nl);
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + nl);
}

static int dsock_bind(const char *name)
{
    struct sockaddr_un a;
    socklen_t al;
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    al = dsock_addr(&a, name);
    if (bind(fd, (struct sockaddr *)&a, al) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

static void *dsock_up_thread(void *arg)
{
    static uint8_t buf[4096];
    int fd = -1;
    uint64_t last_err_ms = 0, last_bd_ms = 0;
    (void)arg;
    while (!atomic_load(&engine_stopping)) {
        ssize_t n;
        unsigned L;
        if (fd < 0) {
            fd = dsock_bind(DSOCK_UP);
            if (fd < 0) {
                uint64_t t = now_ms();
                if (t - last_err_ms > 60000) {
                    last_err_ms = t;
                    mlog("dsock: bind @%s: %s (another consumer?) -- retrying",
                         DSOCK_UP, strerror(errno));
                }
                engine_wait_ms(5000);
                continue;
            }
            mlog("dsock: listening on @%s (stock bt_bsa_app audio uplink)", DSOCK_UP);
        }
        struct pollfd input = { .fd = fd, .events = POLLIN };
        int ready = poll(&input, 1, 100);
        if (atomic_load(&engine_stopping)) break;
        if (ready <= 0) continue;
        n = recv(fd, buf, sizeof buf, MSG_DONTWAIT);
        uint64_t receive_ns = timing_now_ns();
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            mlog("dsock: recv: %s", strerror(errno));
            close(fd);
            fd = -1;
            engine_wait_ms(1000);
            continue;
        }
        if (n < 10) continue;
        L = ((unsigned)buf[2] << 8) | buf[3];
        if (L != (unsigned)n || L <= 10) continue;
        pthread_mutex_lock(&E.feed_mtx);
        uint64_t acquired_ns = timing_now_ns();
        pthread_mutex_lock(&E.st_mtx);
        struct mic_tx *tx = tx_find_bd(buf + 4);
        pthread_mutex_unlock(&E.st_mtx);
        if (!tx) {
            pthread_mutex_unlock(&E.feed_mtx);
            uint64_t t = now_ms();
            if (t - last_bd_ms > 30000) {
                last_bd_ms = t;
                mlog("dsock: data from other bd %02x:%02x:%02x:%02x:%02x:%02x ignored",
                     buf[4], buf[5], buf[6], buf[7], buf[8], buf[9]);
            }
            continue;
        }
        if (E.dsock_pkts++ == 0)
            mlog("dsock: first uplink datagram (%d B, body %u B)", (int)n, L - 10);
        unsigned slot = (unsigned)(tx - TX);
        timing_ingress_begin(slot, 1, (size_t)n, receive_ns, acquired_ns);
        memset(&TS[slot].context, 0, sizeof TS[slot].context);
        TS[slot].context.receive_ns = TS[slot].context.receive_end_ns = receive_ns;
        TS[slot].context.source = 2;
        memcpy(TS[slot].context.bd, buf + 4, 6);
        uplink_body_dispatch(tx, buf + 10, L - 10, buf + 4);
        timing_ingress_end(slot, 1, acquired_ns, timing_now_ns());
        pthread_mutex_unlock(&E.feed_mtx);
    }
    if (fd >= 0) close(fd);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* dji_network control plane: @/dsock/57022 -> @/dsock/57011            */
/* ------------------------------------------------------------------ */
/* On Action 5 the wl service (wl_mic_sw_bluetooth_init(57022, 57021))
 * binds 57022 as the sink for dji_network feature_bt_audio's link-up
 * ladder: bare DUSS frames (sender 07, receiver 0x18, set 0x12, cmd
 * 0x54/0x65/...) the TX waits for before it streams.  The X2D II ships
 * without the service, so those datagrams hit an unbound abstract port
 * and the TX never sees link-up.  Relay instead of answering: rewrap
 * each DUSS verbatim into the stock downlink datagram wire format
 * (libwirelessmicservice wl_comm_data_send_entry @0x3bb40)
 *   {01, seq8, BE16 0x0087, TX bd[6], DUSS(<=0x7d), zero pad} = 135 B
 * and send it to bt_bsa_app's @/dsock/57011, which forwards it to the
 * TX over SPP. Preserve the body verbatim after validating the frame
 * and resolving its target; never assign all native messages to TX1. */
#ifndef DSOCK_RELAY_IN
#define DSOCK_RELAY_IN "/dsock/57022"
#endif
#ifndef DSOCK_DOWN
#define DSOCK_DOWN    "/dsock/57011"
#endif
#define RELAY_DGRAM   135        /* 0x87 on the wire: 10 B header + 125 B body */
#define RELAY_BODY    0x7D       /* largest bare DUSS frame dji_network sends */

/* DUSS in -> 135 B wl-style downlink datagram; bumps the local seq8 */
static int dsock_relay_build(uint8_t *out, const uint8_t *duss, int dlen,
                             const uint8_t *bd)
{
    memset(out, 0, RELAY_DGRAM);
    out[0] = 0x01;
    out[1] = (uint8_t)E.relay_seq++;
    out[2] = 0x00;                    /* BE16 total length 0x0087 */
    out[3] = 0x87;
    memcpy(out + 4, bd, 6);
    memcpy(out + 10, duss, (size_t)dlen);   /* verbatim, zero-padded */
    return RELAY_DGRAM;
}

/* Native 0x54={id,action,bd}, 0x55={quality,bd}, 0x56={bd,clock...}.
 * conn_id is a pairing token, NOT our slot number. Address lists in 0x65
 * are also not transport destinations. Unaddressed verbs can only use a
 * unique configured peer, including TX2 alone. A malformed/unknown explicit
 * address must never fall back to that peer. st_mtx protects the snapshot;
 * the copied destination survives a later slot replacement. */
static int dsock_relay_prepare(uint8_t *out, const uint8_t *duss, int dlen)
{
    if (dlen > RELAY_BODY || !duss_valid(duss, dlen) || duss[4] != 7 ||
        duss[5] != 0x18 || duss[9] != 0x12 ||
        duss_crc16(duss, dlen - 2, 0x3692) !=
            (uint16_t)(duss[dlen - 2] | (duss[dlen - 1] << 8))) return 0;
    const uint8_t *bd = NULL;
    int plen = dlen - 13;
    switch (duss[10]) {
    case 0x54: if (plen != 8) return 0; bd = duss + 13; break;
    case 0x55: if (plen != 7) return 0; bd = duss + 12; break;
    case 0x56: if (plen != 20) return 0; bd = duss + 11; break;
    }
    uint8_t target[6];
    pthread_mutex_lock(&E.st_mtx);
    struct mic_tx *tx = tx_find_bd(bd);
    int found = tx && tx->cfg_bd_valid && !tx->user_hold;
    if (found) memcpy(target, tx->cfg_bd, sizeof target);
    pthread_mutex_unlock(&E.st_mtx);
    return found ? dsock_relay_build(out, duss, dlen, target) : 0;
}

/* send one built datagram to bt_bsa_app's downlink port (unconnected:
 * the peer may not exist yet, a failed send is simply dropped) */
static int dsock_send_down(int fd, const uint8_t *dg, int len)
{
    struct sockaddr_un a;
    socklen_t al = dsock_addr(&a, DSOCK_DOWN);
    return sendto(fd, dg, (size_t)len, MSG_DONTWAIT,
                  (struct sockaddr *)&a, al) == len ? 0 : -1;
}

static void *dsock_relay_thread(void *arg)
{
    static uint8_t buf[256];
    uint8_t dg[RELAY_DGRAM];
    int fd = -1, out = -1;
    uint64_t last_err_ms = 0, last_tx_ms = 0, last_drop_ms = 0;
    unsigned dropped = 0;
    (void)arg;
    while (!atomic_load(&engine_stopping)) {
        ssize_t n;
        if (fd < 0) {
            fd = dsock_bind(DSOCK_RELAY_IN);
            if (fd < 0) {
                uint64_t t = now_ms();
                if (t - last_err_ms > 60000) {
                    last_err_ms = t;
                    mlog("dsock-relay: bind @%s: %s (another consumer?) -- retrying",
                         DSOCK_RELAY_IN, strerror(errno));
                }
                engine_wait_ms(5000);
                continue;
            }
            mlog("dsock-relay: listening on @%s (dji_network DUSS ladder -> @%s)",
                 DSOCK_RELAY_IN, DSOCK_DOWN);
        }
        if (out < 0)
            out = socket(AF_UNIX, SOCK_DGRAM, 0);
        struct pollfd input = { .fd = fd, .events = POLLIN };
        int ready = poll(&input, 1, 100);
        if (atomic_load(&engine_stopping)) break;
        if (ready <= 0) continue;
        n = recv(fd, buf, sizeof buf, MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            mlog("dsock-relay: recv: %s", strerror(errno));
            close(fd);
            fd = -1;
            engine_wait_ms(1000);
            continue;
        }
        if (!dsock_relay_prepare(dg, buf, (int)n)) {
            uint64_t t = now_ms();
            if (++dropped == 1 || t - last_drop_ms >= 60000) {
                last_drop_ms = t;
                mlog("dsock-relay: dropped invalid/unresolved/held target (total=%u)", dropped);
            }
            continue;
        }
        if (out >= 0 && dsock_send_down(out, dg, sizeof dg) == 0)
            E.relay_pkts++;
        else {
            uint64_t t = now_ms();
            if (t - last_tx_ms > 60000) {
                last_tx_ms = t;
                mlog("dsock-relay: send @%s: %s", DSOCK_DOWN, strerror(errno));
            }
        }
        mlog("relay 57022->57011 cmd=0x%02x len=%d", buf[10], (int)n);
    }
    if (fd >= 0) close(fd);
    if (out >= 0) close(out);
    return NULL;
}

static int miccore_shutdown_locked(void);

static int miccore_start_locked(void)
{
    pthread_mutex_lock(&E.st_mtx);
    if (E.started) {
        pthread_mutex_unlock(&E.st_mtx);
        return 0;
    }
    if (!getenv("MICCORE_OFFLINE")) {
        mkdir(MIC_DIR, 0755);
        engine_lock_fd = open(MIC_DIR "/engine.lock", O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (engine_lock_fd < 0 || flock(engine_lock_fd, LOCK_EX | LOCK_NB)) {
            if (engine_lock_fd >= 0) close(engine_lock_fd);
            engine_lock_fd = -1;
            pthread_mutex_unlock(&E.st_mtx);
            return -1;
        }
        /* An older engine has no lock: do not unlink its live control socket. */
        int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
        struct sockaddr_un address = { .sun_family = AF_UNIX };
        snprintf(address.sun_path, sizeof address.sun_path, "%s", CTL_PATH);
        int rc = probe < 0 ? -1 : connect(probe, (struct sockaddr *)&address, sizeof address);
        int err = errno;
        if (probe >= 0) close(probe);
        if (rc == 0 || (err != ENOENT && err != ECONNREFUSED)) {
            close(engine_lock_fd); engine_lock_fd = -1;
            pthread_mutex_unlock(&E.st_mtx);
            return -1;
        }
    }
    E.started = 1;
    atomic_store(&engine_stopping, 0);
    pthread_mutex_unlock(&E.st_mtx);

    signal(SIGPIPE, SIG_IGN);

    E.offline = getenv("MICCORE_OFFLINE") != NULL;
    mic_admission_enable(0);
    timing_init();
    memset(TX, 0, sizeof TX);
    memset(recent_pcm,0,sizeof recent_pcm);
    memset(record_preroll,0,sizeof record_preroll);
    for (unsigned slot = 0; slot < 2; slot++) {
        TX[slot].battery = TX[slot].battery_level = TX[slot].charging = -1;
        TX[slot].mute = TX[slot].recording = TX[slot].capacity = -1;
        TX[slot].mic_gain = -128;
        TX[slot].prev_state = -1;
    }
    E.pcm_fd = -1;
    memset(&PCM2, 0, sizeof PCM2); PCM2.fd = -1;
    E.standalone = 0;
    atomic_store(&discovery_cancel, 0);
    B.fd = -1;                      /* 0 is a valid fd number (stdin) */

    mkdir(MIC_DIR, 0755);
    read_cfg();
    mlog("miccore_start: offline=%d bd_valid=%d auto_connect=%d",
         E.offline, TX[0].cfg_bd_valid, E.auto_connect);

    for (unsigned slot = 0; slot < 2; slot++) {
        if (TX[slot].cfg_bd_valid)
            lookup_name(TX[slot].cfg_bd, TX[slot].name, sizeof TX[slot].name);
        update_paired(&TX[slot]);
        if (engine_decoder_init(slot) != 0)
            mlog("miccore_start: TX%u decoder unavailable", slot + 1);
    }

    if (bb_init()) goto failed;
#ifdef MICCORE_DISCOVERED_ABI
    mic_dg_events_enable(!E.offline && bb_base && mic_dg.initialized && !mic_dg.failed);
#endif
    if (!E.offline && !bb_base) {
        /* v4: running as the standalone daemon (micd), not injected */
        E.standalone = 1;
        E.ka_055 = 0;              /* no in-process SPP send path */
        mlog("miccore_start: STANDALONE mode (no injection; uplink via @%s)",
             DSOCK_UP);
    }

    mkfifo(FIFO_PATH, 0666);
    chmod(FIFO_PATH, 0666);
    if (mkfifo(STEREO_FIFO_PATH, 0666) && errno != EEXIST) goto failed;
    struct stat stereo_node;
    if (lstat(STEREO_FIFO_PATH, &stereo_node) || !S_ISFIFO(stereo_node.st_mode)) goto failed;
    chmod(STEREO_FIFO_PATH, 0666);
    E.pcm_running = pthread_create(&E.pcm_thread, NULL, pcm_stereo_thread, NULL) == 0;
    if (!E.pcm_running) goto failed;

    if (!E.offline) {
        unlink(CTL_PATH);
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un a;
        if (fd < 0) {
            mlog("ctl socket: %s", strerror(errno));
            goto failed;
        }
        memset(&a, 0, sizeof a);
        a.sun_family = AF_UNIX;
        strncpy(a.sun_path, CTL_PATH, sizeof a.sun_path - 1);
        if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 ||
            listen(fd, 4) < 0) {
            mlog("ctl bind/listen: %s", strerror(errno));
            close(fd);
            goto failed;
        }
        chmod(CTL_PATH, 0666);
        ctl_running = pthread_create(&E.ctl_thread, NULL, ctl_accept_thread, (void *)(intptr_t)fd) == 0;
        if (ctl_running) ctl_listener = fd;
        else { close(fd); mlog("miccore_start: control listener unavailable"); goto failed; }
    }

    E.hk_running = pthread_create(&E.hk_thread, NULL, hk_thread, NULL) == 0;
    if (!E.hk_running) { mlog("miccore_start: housekeeper unavailable"); goto failed; }
    /* v5: the dsock workers run in EVERY non-offline mode.  They used to
     * be standalone-only: injected, nobody bound @/dsock/57012 while
     * bt_bsa_app kept firing audio frames at it (5 ms x3 retries each,
     * backpressure took bsa_server down), and dji_network's link-up
     * ladder on @/dsock/57022 went nowhere -> the TX never linked up. */
    if (!E.offline) {
        E.dsock_running = pthread_create(&E.dsock_thread, NULL, dsock_up_thread, NULL) == 0;
        if (!E.dsock_running) { mlog("miccore_start: uplink worker unavailable"); goto failed; }
        E.relay_running = pthread_create(&E.relay_thread, NULL, dsock_relay_thread, NULL) == 0;
        if (!E.relay_running) { mlog("miccore_start: relay worker unavailable"); goto failed; }
    }

    /* v3: reply-queue worker (sends DUSS answers off the feed path);
     * started in offline mode too so self-tests see constructed bytes */
    if (rq_start()) { mlog("miccore_start: reply worker unavailable"); goto failed; }

    status_write();

    /* auto-connect is deferred to the housekeeper thread: calling into
     * bt_bsa_app from the dlopen constructor context (loader lock held)
     * could deadlock if app_dg_open_ex2 ever touches dlopen itself.
     * If the BSA client is not open (bt_bsa_app restarted outside the
     * boot chain -- stock bring-up is WMS dsock cmd 0x00 -> app_init),
     * bootstrap it in-process first. */
    unsigned pending = 0;
    for (unsigned slot = 0; slot < 2; slot++) if (!E.offline && TX[slot].cfg_bd_valid && (p_dg_open || E.standalone)) {
        if (!bb_stack_inited() && p_app_init) E.need_app_init = 1;
        if (E.auto_connect) {
            TX[slot].autocmd_pending = 1;
            TX[slot].autocmd_at = now_ms() + pending++ * 2000;
        }
    }

    atomic_store(&feed_enabled, 1);
    return 0;
failed:
    miccore_shutdown_locked();
    return -1;
}

int miccore_start(void)
{
    pthread_mutex_lock(&lifecycle_mtx);
    int result = miccore_start_locked();
    pthread_mutex_unlock(&lifecycle_mtx);
    return result;
}

static int miccore_shutdown_locked(void)
{
    char path[256];
    uint64_t bytes = 0;
    if (!E.started) return 0;
    atomic_store(&feed_enabled, 0);
    atomic_store(&engine_stopping, 1);
    mic_admission_enable(0);
    mic_dg_events_enable(0); /* wake housekeeping and close waiters before joins */
    atomic_store(&discovery_cancel, 1);
    ctl_stop();
    if (E.hk_running) { pthread_join(E.hk_thread, NULL); E.hk_running = 0; }
    if (E.pcm_running) { pthread_join(E.pcm_thread, NULL); E.pcm_running = 0; }
    if (E.dsock_running) { pthread_join(E.dsock_thread, NULL); E.dsock_running = 0; }
    if (E.relay_running) { pthread_join(E.relay_thread, NULL); E.relay_running = 0; }
    /* v3: if a link is still up, send the 0x54/0x20 sleep notice before
     * the process goes away (product stop path) */
    if (!E.offline) for (unsigned slot = 0; slot < 2; slot++)
        if (link_up_gate(&TX[slot])) announce_sleep_0x54_20(&TX[slot]);
    pthread_mutex_lock(&E.feed_mtx);
    pthread_mutex_lock(&E.rec_mtx);
    while (E.rec_finalizing || record_mono.draining || record_tail.draining) pthread_cond_wait(&rec_finalized, &E.rec_mtx);
    rec_stop_locked(path, sizeof path, &bytes);
    pthread_mutex_unlock(&E.rec_mtx);
    mic_aac_free(E.rec_aac); E.rec_aac = NULL;
    rq_stop();
    engine_decoders_close();
    if (E.pcm_fd >= 0) close(E.pcm_fd);
    E.pcm_fd = -1;
    pcm_stereo_close();
    pthread_mutex_unlock(&E.feed_mtx);
    if (B.fd >= 0) { close(B.fd); B.fd = -1; }
    unlink(CTL_PATH);
    memset(usage_tokens, 0, sizeof usage_tokens);
    if (engine_lock_fd >= 0) { close(engine_lock_fd); engine_lock_fd = -1; }
    E.started = 0;
    return 0;
}

int miccore_shutdown(void)
{
    pthread_mutex_lock(&lifecycle_mtx);
    int result = miccore_shutdown_locked();
    pthread_mutex_unlock(&lifecycle_mtx);
    return result;
}

/* auto-start when dlopen()ed (constructor runs inside bt_bsa_app) */
#ifndef MICCORE_NATIVE_HOOK
__attribute__((constructor)) static void miccore_ctor(void)
{
    /* constructor context: safe to start threads (dlopen completes after
     * return; threads only use already-initialized statics).
     * v4: auto-start ONLY when injected into bt_bsa_app. Executables that
     * link the engine (micd, miccore_test) call miccore_start() from main()
     * -- otherwise the ctor would start a live engine before main() can
     * set MICCORE_OFFLINE, and steal /data/mic/mic_ctl from a running one. */
    if (!bb_resolve()) return;
    miccore_start();
}
#endif
