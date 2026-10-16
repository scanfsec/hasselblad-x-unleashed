/*
 * wlmicd.c -- DJI Mic 2 wireless-mic service daemon (Action-5 wl-service
 * equivalent for the Hasselblad HB722, firmware 316).
 *
 * =====================================================================
 *  ARCHITECTURE -- the complete wl service stack, ZERO INJECTION.
 * =====================================================================
 *
 *   TX <==SPP==> bt_bsa_app (100% native behaviour, no hooks, no
 *                 LD_PRELOAD, no ptrace, no memory writes)
 *     |-- /dsock/57012 uplink dgrams  {01, seq8, len16be, bd[6], payload}
 *     |        payload = N x 125B audio units (6B {btclk,intra} hdr +
 *        [wlmicd]   119B Wl block)  -- Wl 0x10 audio, 0x19 status TLV
 *     |-- /dsock/57011 downlink dgrams {xx, xx, len16be(=10+plen),
 *     |        bd[6], bare-DUSS<=125B} -> bt_bsa_app wraps AA-ver1 and
 *        [wlmicd]   sends to SPP natively
 *     +-- bsa daemon socket /data/misc/bluetooth/bt-daemon-socket
 *        [wlmicd]   client: scan / pair / unpair / RSSI (0x582) /
 *                   TmDisconnect (0x583)
 *
 *  wlmicd is the stand-alone replacement for the stage-1 frida-injected
 *  miccore engine: same three user faces (mic_ctl / mic_status.json /
 *  mic.pcm, INTERFACE.md unchanged), same DUSS builders (CRC tables
 *  verified byte-exact against live frames), same LC3plus decode --
 *  but it lives in its own process and talks to bt_bsa_app ONLY through
 *  the two native dsock channels bt_bsa_app already exposes in main()
 *  (app_audio_play_init: handle_create(57011, 57012, 1000, -1)).
 *
 *  Native-behaviour facts baked in (disassembled + live-verified,
 *  artifacts/bt_bsa_app.device, fw 316):
 *   - uplink:  _bt_audio_send_audio_data@0x21150: audio units -> 57012.
 *     DUSS control tails never reach 57012: _bt_audio_analysis_v1@0x21a48
 *     handles TX->camera set-0x12 cmd 0x58/0x65 locally and DROPS the
 *     rest; foreign-addressed DUSS goes to WMS cmd 0x05 (/dsock/80000).
 *     => wlmicd's control legs are PROACTIVE (0x55 keepalive 1 Hz,
 *        0x57 status poll 10 s, conn-OK refresh); the reactive responder
 *     stays compiled in and fires only if a DUSS frame ever rides a
 *     57012 payload (parser supports it).
 *   - native keepalive legs on this firmware: bt_bsa_app itself answers
 *     TX's 0x58/01 handshake; TX fw316 sustains on 1 Hz 0x55 + the
 *     periodic 0x58 dance (live evidence mic.log 03:14-03:26, 132k+
 *     frames, no TX 0x15/0x17 at all).
 *   - downlink gate: _bt_audio_recv_thread@0x241e0 requires the dgram
 *     bd to match the in-memory bt_audio.cfg slot (struct @0x800e90,
 *     mic0/mic1, stride 0x834) AND slot+0x32 (ver_done) != 0.
 *     ver_done is set by _bt_audio_handle_version_sync@0x226c8 when
 *     TX answers the camera's 0x65/04 announce -- which bt_bsa_app only
 *     sends when the cfg slot exists (is_master_addr gate).  Therefore
 *     wlmicd (cutover mode) maintains /data/misc/bluetooth/bt_audio.cfg
 *     exactly like dji_network's update_config: after the file exists,
 *     the NEXT SPP bring-up completes version sync and the gate opens.
 *   - WMS cmd channel /dsock/90000 is source-verified to /dsock/80000
 *     (owned by dji_network) => connect stays TX-initiated (the native
 *     HFP->SPP DJI special case).  ctl "connect" arms + pairs.
 *
 *  Compatibility period: while the stage-1 hook engine still runs inside
 *  bt_bsa_app it owns /data/mic/mic_ctl + mic_status.json + mic.pcm and
 *  provides the responder.  wlmicd auto-detects it (mic_ctl alive) and
 *  defaults to compat mode: alternate face paths, no fifo write, no
 *  keepalive/responder duplication, 0x57 poll on.  Both processes read
 *  the SAME /data/mic/mic.cfg (only wlmicd ever writes it).
 *
 * Offline / self-test:  wlmicd --replay <dgram-capture.bin> [--wav out]
 *   [--tlv-synth] [--expect-frames N] [--probe57] [--probe55]
 * feeds captures (e.g. D:/dji/decode/spp_audio_capture.bin -- already
 * in native dgram format) through the identical input path, plus a
 * synthetic Wl 0x19 TLV block and byte-template checks of the DUSS
 * constructors (REPORT_ctrl_frames section 4).
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
#include <time.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <dlfcn.h>

/* ------------------------------------------------------------------ */
/* paths & limits                                                      */
/* ------------------------------------------------------------------ */
#define MIC_DIR     "/data/mic"
#define CFG_PATH    MIC_DIR "/mic.cfg"
#define DEV_XML     "/data/misc/bluetooth/bt_devices.xml"
#define BT_AUD_CFG  "/data/misc/bluetooth/bt_audio.cfg"
#define BT_SOCK     "/data/misc/bluetooth/bt-daemon-socket"
#define LC3_SO      MIC_DIR "/libLC3plus_bionic.so"
#define LC3_SO_ALT  MIC_DIR "/liblc3plus_b.so"

#define DSOCK_UP    "/dsock/57012"   /* bt_bsa_app -> wlmicd (we bind)   */
#define DSOCK_DOWN  "/dsock/57011"   /* wlmicd -> bt_bsa_app (we sendto) */

#define MAXPL       2100
#define SRATE       48000
#define FRAME_MS    5
#define NSAMP       240
#define DG_MAX      2048            /* native dgram cap is 0x416+10     */

/* ------------------------------------------------------------------ */
/* engine state                                                        */
/* ------------------------------------------------------------------ */
enum { ST_IDLE = 0, ST_CONNECTING, ST_STREAMING, ST_RECONNECTING };
static const char *ST_NAME[] = { "idle", "connecting", "streaming", "reconnecting" };

static struct {
    int started;
    int offline;                 /* --replay / WLMICD_OFFLINE: no dsock/bsa/ctl */
    int compat;                  /* hook engine present: share nothing it owns */
    /* dual-mic config (bd0 = primary; "bd=" is the legacy spelling) */
    uint8_t bd[2][6];
    int bd_valid[2];
    /* switches (mic.cfg; hot-reloaded) */
    int auto_connect;
    int keepalive_055;
    int responder;               /* reactive DUSS answers (cutover only) */
    int poll_057;
    int poll_057_sec;
    int linkup_refresh_sec;      /* periodic conn-OK replay, 0 = off */
    int manage_bt_audio_cfg;
    int rssi_poll;
    int adopt_bd;                /* pair success -> rewrite mic.cfg bd */
    /* status */
    pthread_mutex_t st_mtx;
    int state;
    int connected;
    int paired;
    char name[64];
    int battery, charging, mute, recording;    /* -1 unknown */
    int rssi;
    uint64_t last_audio_ms, last_pad_ms, connect_ms;
    /* time-sync cache for 0x56/0x57 answers: prefer the 0x58 exchange,
     * fall back to the freshest audio-unit BT-clock header (57012
     * carries {btclk u32le, intra u16le} per 125B unit) */
    uint32_t ts_btclk;
    uint16_t ts_intra;
    uint64_t ts_ms;
    int ts_valid;
    volatile uint64_t frames, drops;
    volatile double level;
    /* pcm sinks */
    int pcm_fd;
    pthread_mutex_t rec_mtx;
    FILE *rec_f;
    char rec_path[256];
    uint64_t rec_bytes, rec_deadline_ms;
    /* stream parser */
    pthread_mutex_t feed_mtx;
    uint8_t sbuf[1 << 20];
    int slen, have_seq;
    uint8_t last_seq;
    /* per-source accounting */
    uint64_t dgrams, dgrams_audio, dgrams_other_bd;
    /* threads */
    pthread_t hk_thread, ctl_thread, rx_thread, rq_thread;
    int rx_running, ctl_running, rq_running;
    /* resolved face paths (compat-aware) */
    char p_status[128], p_ctl[128], p_fifo[128], p_log[128];
    int fifo_enabled;
    /* cfg hot reload */
    uint64_t cfg_mtime_ns;
    int cfg_dirty_hint;
} E;

/* ------------------------------------------------------------------ */
/* logging                                                             */
/* ------------------------------------------------------------------ */
static pthread_mutex_t log_mtx = PTHREAD_MUTEX_INITIALIZER;
static int log_fd = -1;
static int log_stderr = 0;

static void mlog(const char *fmt, ...)
{
    va_list ap;
    char b[600];
    struct timespec now;
    struct tm tmv;
    int n;
    time_t t;

    if (log_stderr) {
        clock_gettime(CLOCK_REALTIME, &now);
        t = now.tv_sec;
        localtime_r(&t, &tmv);
        n = snprintf(b, sizeof b, "[%02d:%02d:%02d.%03ld] ",
                     tmv.tm_hour, tmv.tm_min, tmv.tm_sec, now.tv_nsec / 1000000);
        va_start(ap, fmt);
        n += vsnprintf(b + n, sizeof b - n - 1, fmt, ap);
        va_end(ap);
        if (n > (int)sizeof b - 2) n = (int)sizeof b - 2;
        b[n++] = '\n';
        fwrite(b, 1, (size_t)n, stderr);
        fflush(stderr);
    }
    if (E.p_log[0] == 0) return;
    if (log_fd < 0) {
        log_fd = open(E.p_log, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (log_fd < 0) return;
    }
    if (!log_stderr) {
        clock_gettime(CLOCK_REALTIME, &now);
        t = now.tv_sec;
        localtime_r(&t, &tmv);
        n = snprintf(b, sizeof b, "[%02d:%02d:%02d.%03ld] ",
                     tmv.tm_hour, tmv.tm_min, tmv.tm_sec, now.tv_nsec / 1000000);
        va_start(ap, fmt);
        n += vsnprintf(b + n, sizeof b - n - 1, fmt, ap);
        va_end(ap);
        if (n > (int)sizeof b - 2) n = (int)sizeof b - 2;
        b[n++] = '\n';
    }
    pthread_mutex_lock(&log_mtx);
    if (write(log_fd, b, (size_t)n) < 0) { /* ignore */ }
    pthread_mutex_unlock(&log_mtx);
}

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
/* LC3plus decoder, dual backend (ETSI primary / surgical fallback)    */
/*   API traps (src/decoder/README.md): PLC enum 1, dms 5MS == 4,      */
/*   dec16 wants int16_t ** and a WRITABLE input copy.                 */
/* ------------------------------------------------------------------ */
static void *dec;
static int dec_etsi;
static int dec_nsamp = NSAMP;
static int16_t pcm_frame[NSAMP];
static uint8_t framebuf[128];

typedef int (*fe_get_size)(int, int);
typedef int (*fe_init)(void *, int, int, int, int);
typedef int (*fe_dms)(void *, int);
typedef int (*fe_ns)(void *);
typedef int (*fe_dec16)(void *, unsigned char *, int, int16_t **, void *, int);
static fe_get_size e_get_size;
static fe_init e_init;
static fe_dms e_dms;
static fe_ns e_ns;
static fe_dec16 e_dec16;

typedef int (*f_get_size)(int, int);
typedef int (*f_init)(void *, int, int, int);
typedef int (*f_hr)(void *, int);
typedef int (*f_ms)(void *, float);
typedef int (*f_ns)(void *);
typedef int (*f_dec16)(void *, const void *, int, void **);
static f_get_size lc3_dec_get_size;
static f_init lc3_dec_init;
static f_hr lc3_dec_set_hrmode;
static f_ms lc3_dec_set_frame_ms;
static f_ns lc3_dec_get_output_samples;
static f_dec16 lc3_dec16;
static void *dec_iov[2];

/* glibc-name stubs for the surgical .so (bionic has no __assert_fail).
 * In a stand-alone daemon an assert may abort without taking any
 * system process with us -- but logging is still nicer. */
void __assert_fail(const char *expr, const char *file, unsigned line, const char *fn)
{
    mlog("liblc3plus ASSERT: %s:%u %s: %s", file, line, fn ? fn : "", expr);
    if (getenv("WLMICD_ABORT_ON_ASSERT")) abort();
}
void __gmon_start__(void) {}
void _ITM_deregisterTMCloneTable(void) {}
void _ITM_registerTMCloneTable(void) {}

static int decoder_init_etsi(void)
{
    void *h = dlopen(LC3_SO, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        mlog("decoder_init: ETSI dlopen %s: %s", LC3_SO, dlerror());
        return -1;
    }
#define RS(var, name) do { *(void **)(&(var)) = dlsym(h, name); \
        if (!(var)) { mlog("decoder_init: dlsym %s failed", name); return -1; } } while (0)
    RS(e_get_size, "lc3plus_dec_get_size");
    RS(e_init, "lc3plus_dec_init");
    RS(e_dms, "lc3plus_dec_set_frame_dms");
    RS(e_ns, "lc3plus_dec_get_output_samples");
    RS(e_dec16, "lc3plus_dec16");
#undef RS
    int need = e_get_size(SRATE, 1);
    dec = calloc(1, (size_t)need);
    if (!dec) return -1;
    if (e_init(dec, SRATE, 1, 1 /* PLC_ADVANCED */, 0)) { dec = NULL; return -1; }
    if (e_dms(dec, 4 /* 5MS */)) { dec = NULL; return -1; }
    dec_nsamp = e_ns(dec);
    if (dec_nsamp <= 0 || dec_nsamp > NSAMP) dec_nsamp = NSAMP;
    dec_etsi = 1;
    mlog("decoder_init: ETSI ok (%d samples/frame, PLC)", dec_nsamp);
    return 0;
}

static int decoder_init(void)
{
    if (decoder_init_etsi() == 0) return 0;
    void *h = dlopen(LC3_SO_ALT, RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        mlog("decoder_init: fallback dlopen %s: %s", LC3_SO_ALT, dlerror());
        return -1;
    }
#define RS(var, name) do { *(void **)(&(var)) = dlsym(h, name); \
        if (!(var)) { mlog("decoder_init: dlsym %s failed", name); return -1; } } while (0)
    RS(lc3_dec_get_size, "lc3_dec_get_size");
    RS(lc3_dec_init, "lc3_dec_init");
    RS(lc3_dec_set_hrmode, "lc3_dec_set_hrmode");
    RS(lc3_dec_set_frame_ms, "lc3_dec_set_frame_ms");
    RS(lc3_dec_get_output_samples, "lc3_dec_get_output_samples");
    RS(lc3_dec16, "lc3_dec16");
#undef RS
    int need = lc3_dec_get_size(SRATE, 1);
    dec = calloc(1, (size_t)need);
    if (!dec) return -1;
    lc3_dec_init(dec, SRATE, 1, 0);
    lc3_dec_set_hrmode(dec, 0);
    if (lc3_dec_set_frame_ms(dec, (float)FRAME_MS)) { dec = NULL; return -1; }
    dec_nsamp = lc3_dec_get_output_samples(dec);
    if (dec_nsamp <= 0 || dec_nsamp > NSAMP) dec_nsamp = NSAMP;
    dec_iov[0] = pcm_frame;
    dec_iov[1] = (char *)pcm_frame + dec_nsamp * 2;
    dec_etsi = 0;
    mlog("decoder_init: surgical fallback ok (%d samples/frame, no PLC)", dec_nsamp);
    return 0;
}

static int decode_frame(const uint8_t *frame, int plen)
{
    if (dec_etsi) {
        int16_t *outp = pcm_frame;
        if (plen > (int)sizeof framebuf) return -1;
        memcpy(framebuf, frame, (size_t)plen);
        return e_dec16(dec, framebuf, plen, &outp, NULL, 0);
    }
    return lc3_dec16(dec, frame, plen, dec_iov);
}

static int decode_plc_frame(void)
{
    if (dec_etsi) {
        int16_t *outp = pcm_frame;
        return e_dec16(dec, NULL, 0, &outp, NULL, 0);
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* DUSS v1 frame builder (CRC tables verified vs live frame            */
/* 55 0E 04 66 07 3A 01 00 40 12 58 02 03 EE A5 and REPORT_ctrl_frames) */
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
        /* standard split-table update.  (The stage-1 miccore.c had a
         * transcription bug here -- OR instead of XOR and the old high
         * byte folded into the new low byte -- which degenerates to
         * input-insensitive values like 0xf1ff.  TX fw316 tolerated the
         * corrupt CRCs, but every report template verifies only with
         * this form: 58/02 real capture -> 03EE, 0x54 -> 1464,
         * 0x55 -> CB49, 0x57 -> 2A99, ccode -> E08D, ver-sync -> B9C8.) */
        c = (uint16_t)((c >> 8) ^ ((duss_t16hi[idx] << 8) | duss_t16lo[idx]));
    }
    return c;
}

/* single shared seq counter for ALL camera->TX DUSS frames (product:
 * dji_network 0x508d68) -- exposed for the byte-template self-test */
uint16_t wlmicd_duss_seq = 0x0100;

static int duss_build_r(uint8_t *out, uint8_t cmd_id, const uint8_t *payload,
                        int plen, uint8_t receiver)
{
    int len = 11 + plen + 2;
    uint16_t c16;
    out[0] = 0x55;
    out[1] = (uint8_t)(len & 0xff);
    out[2] = (uint8_t)(0x04 | ((len >> 8) & 0x03));
    out[3] = duss_crc8(out, 3, 0x77);
    out[4] = 0x07;                 /* sender: camera (0x700) */
    out[5] = receiver;             /* 0x18 product relay / 0x3a TX local */
    out[6] = (uint8_t)(wlmicd_duss_seq & 0xff);
    out[7] = (uint8_t)(wlmicd_duss_seq >> 8);
    wlmicd_duss_seq++;
    out[8] = 0x40;
    out[9] = 0x12;                 /* cmd_set: Mic */
    out[10] = cmd_id;
    if (plen > 0) memcpy(out + 11, payload, plen);
    c16 = duss_crc16(out, len - 2, 0x3692);
    out[len - 2] = (uint8_t)(c16 & 0xff);
    out[len - 1] = (uint8_t)(c16 >> 8);
    return len;
}

/* ------------------------------------------------------------------ */
/* downlink transport: /dsock/57011 dgram -> native AA wrap -> SPP      */
/*   dgram = {[0]=0x01, [1]=seq8, [2..3]=total BE16 = 10+plen,          */
/*            [4..9]=bd, [10..]=BARE DUSS frame, plen <= 125}           */
/*   (bt_bsa_app _bt_audio_recv_thread@0x241e0 validates total<0x88,   */
/*    bd vs bt_audio.cfg slots, slot ver_done; then wraps ver1-id1)    */
/* ------------------------------------------------------------------ */
static int dn_fd = -1;                       /* unbound abstract sender */
static struct sockaddr_un dn_addr;
static socklen_t dn_alen;
static uint8_t dn_seq = 0;
static pthread_mutex_t dn_mtx = PTHREAD_MUTEX_INITIALIZER;
static volatile uint64_t dn_sent, dn_fail;

static int dn_open(void)
{
    if (dn_fd >= 0) return 0;
    dn_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (dn_fd < 0) {
        mlog("dsock57011: socket: %s", strerror(errno));
        return -1;
    }
    memset(&dn_addr, 0, sizeof dn_addr);
    dn_addr.sun_family = AF_UNIX;
    dn_addr.sun_path[0] = 0;
    snprintf(dn_addr.sun_path + 1, sizeof dn_addr.sun_path - 1, DSOCK_DOWN);
    dn_alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(DSOCK_DOWN));
    return 0;
}

/* send one bare DUSS frame to the TX via the native downlink */
static int dsock_send_duss(const uint8_t *duss, int dlen, const uint8_t *bd)
{
    uint8_t pkt[4 + 6 + 125];
    int total;
    if (dlen <= 0 || dlen > 125) return -1;
    total = 10 + dlen;
    pkt[0] = 0x01;
    pkt[1] = dn_seq++;
    pkt[2] = (uint8_t)((total >> 8) & 0xff);
    pkt[3] = (uint8_t)(total & 0xff);
    memcpy(pkt + 4, bd, 6);
    memcpy(pkt + 10, duss, (size_t)dlen);
    pthread_mutex_lock(&dn_mtx);
    if (dn_open() == 0 && sendto(dn_fd, pkt, (size_t)total, 0,
                                 (struct sockaddr *)&dn_addr, dn_alen) == total)
        dn_sent++;
    else {
        dn_fail++;
        if ((dn_fail & (dn_fail - 1)) == 0 || dn_fail < 8)   /* log 1,2,4,8... */
            mlog("dsock57011: sendto failed (%llu attempts): %s",
                 (unsigned long long)dn_fail, strerror(errno));
    }
    pthread_mutex_unlock(&dn_mtx);
    return 0;
}

/* ------------------------------------------------------------------ */
/* reply queue: serialized sender thread (never on the RX path)        */
/* ------------------------------------------------------------------ */
#define RQ_CAP 16
static struct {
    pthread_mutex_t mtx;
    pthread_cond_t cv;
    int running;
    struct { uint8_t b[128]; int n; uint8_t bd[6]; } q[RQ_CAP];
    int h, cnt;
    volatile uint64_t sent, lost;
} RQ = { .mtx = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

static void hexlog(const char *tag, const uint8_t *b, int n)
{
    char hx[3 * 128 + 1];
    int i, o = 0;
    if (n > 128) n = 128;
    for (i = 0; i < n; i++) o += snprintf(hx + o, sizeof hx - o, "%02x", b[i]);
    mlog("%s [%s] (%dB)", tag, hx, n);
}

static void rq_push_duss(uint8_t cmd_id, const uint8_t *payload, int plen,
                         uint8_t receiver, const uint8_t *bd)
{
    uint8_t duss[96];
    int dl;
    if (plen < 0 || plen > 64) return;
    dl = duss_build_r(duss, cmd_id, payload, plen, receiver);
    pthread_mutex_lock(&RQ.mtx);
    if (RQ.cnt >= RQ_CAP) { RQ.h = (RQ.h + 1) % RQ_CAP; RQ.cnt--; RQ.lost++; }
    {
        int t = (RQ.h + RQ.cnt) % RQ_CAP;
        memcpy(RQ.q[t].b, duss, (size_t)dl);
        RQ.q[t].n = dl;
        memcpy(RQ.q[t].bd, bd, 6);
        RQ.cnt++;
    }
    pthread_cond_signal(&RQ.cv);
    pthread_mutex_unlock(&RQ.mtx);
}

static void *rq_thread(void *arg)
{
    (void)arg;
    for (;;) {
        uint8_t b[128];
        int n;
        uint8_t bd[6];
        pthread_mutex_lock(&RQ.mtx);
        while (RQ.cnt == 0)
            pthread_cond_wait(&RQ.cv, &RQ.mtx);
        n = RQ.q[RQ.h].n;
        memcpy(b, RQ.q[RQ.h].b, (size_t)n);
        memcpy(bd, RQ.q[RQ.h].bd, 6);
        RQ.h = (RQ.h + 1) % RQ_CAP;
        RQ.cnt--;
        pthread_mutex_unlock(&RQ.mtx);
        RQ.sent++;
        if (E.offline) {
            hexlog("reply_tx (offline)", b, n);   /* self-test evidence */
        } else {
            hexlog("reply_tx", b, n);
            dsock_send_duss(b, n, bd);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* BSA daemon client: scan / pair / unpair / RSSI / TmDisconnect       */
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
    uint8_t bond_bd[6];
    uint8_t bond_key[16];
    uint8_t bond_key_type;
    uint8_t bond_ok;
    char bond_name[249];
    struct { uint8_t bd[6]; char name[48]; unsigned cod; int rssi; } seen[64];
    int nseen, scan_mode;
    int disc_done;
} B;                                /* B.fd MUST start at -1 */

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
    if (write(B.fd, hdr, 4) != 4) {
        mlog("bsa_send(0x%03x): hdr: %s", id, strerror(errno));
        return -1;
    }
    if (plen > 0 && write(B.fd, payload, plen) != plen) {
        mlog("bsa_send(0x%03x): payload: %s", id, strerror(errno));
        return -1;
    }
    return 0;
}

static unsigned rd16b(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }

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
    if (!(id & 0x4000)) return;
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
        if (n > 0x108) rssi = (int)(int32_t)(pl[0x104] | ((uint32_t)pl[0x105] << 8) |
                                              ((uint32_t)pl[0x106] << 16) | ((uint32_t)pl[0x107] << 24));
        if (n > 9) {
            int k;
            for (k = 0; k < 40 && 9 + k < n && pl[9 + k]; k++) nm[k] = pl[9 + k];
            nm[k] = 0;
            if (k > 0) nm_len = k;
        }
        if (n > 0x119 && pl[0x118]) {
            int lim = 0x119 + (pl[0x118] > 0xf0 ? 0xf0 : pl[0x118]);
            if (lim > n) lim = n;
            for (i = 0x119; i + 1 < lim; ) {
                int tl = pl[i], tt = pl[i + 1];
                if (tl == 0 || i + 1 + tl > lim) break;
                if ((tt == 0x09 || tt == 0x08) && tl > 1) {
                    int L = tl - 1; if (L > 63) L = 63;
                    memcpy(nm, pl + i + 2, (size_t)L); nm[L] = 0; nm_len = L;
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
    case 0xCC:
        B.disc_done = 1;
        break;
    case 0x13E:                   /* SP_CFM_REQ: auto-accept (SSP) */
        if (n >= 8) {
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
        if (n > 0x112) {
            int k;
            memcpy(B.bond_bd, pl, 6);
            B.bond_ok = pl[0x111] ? 1 : 2;
            B.bond_key_type = pl[0x110];
            memcpy(B.bond_key, pl + 0xFF, 16);
            for (k = 0; k < 248 && 6 + k < n && pl[6 + k]; k++) B.bond_name[k] = pl[6 + k];
            B.bond_name[k] = 0;
            mlog("bsa: AUTH_CMPL %s name='%s'", B.bond_ok == 1 ? "SUCCESS" : "FAIL", B.bond_name);
        }
        break;
    default:
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

static int bsa_pump2(int timeout_ms, int for_disc)
{
    uint64_t deadline = now_ms() + (uint64_t)timeout_ms;
    for (;;) {
        uint64_t left;
        fd_set rf;
        struct timeval tv;
        unsigned char chunk[4096];
        int r;
        if ((B.wait_id && B.got_resp) || (for_disc && B.disc_done)) return 0;
        left = deadline - now_ms();
        if (left == 0 || (int64_t)left < 0) return -1;
        FD_ZERO(&rf); FD_SET(B.fd, &rf);
        tv.tv_sec = (time_t)(left / 1000);
        tv.tv_usec = (suseconds_t)((left % 1000) * 1000);
        r = select(B.fd + 1, &rf, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return B.got_resp ? 0 : -1;
        r = (int)read(B.fd, chunk, sizeof chunk);
        if (r <= 0) {
            mlog("bsa_pump: read=%d (server closed?)", r);
            close(B.fd);
            B.fd = -1;
            return -1;
        }
        fr_feed(&B.fr, chunk, r);
    }
}

static int bsa_sync(unsigned id, const void *pl, int plen, unsigned char *out,
                    int outsz, int timeout_ms)
{
    int rc;
    pthread_mutex_lock(&B.mtx);
    if (bsa_conn() < 0) { pthread_mutex_unlock(&B.mtx); return -1; }
    B.wait_id = (int)(id & 0xFFF);
    B.got_resp = 0;
    B.resp_len = -1;
    if (bsa_send(id, pl, plen) < 0) {
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
    memcpy(out, p, (size_t)vl);
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
    long sz;
    char *buf;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 512 * 1024) { fclose(f); return NULL; }
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return NULL; }
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
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    if (write(fd, buf, len) != len) { close(fd); unlink(tmp); return -1; }
    close(fd);
    if (stat(path, &st) == 0) {
        chmod(tmp, st.st_mode & 07777);
        chown(tmp, st.st_uid, st.st_gid);
    }
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

static int xml_count_devices(char *buf, int len)
{
    char *cur = NULL, *be;
    int n = 0;
    while (xml_each_device(buf, len, &cur, &be)) n++;
    return n;
}

static int xml_replace_tag(char *buf, int *len, char *blk, int blklen,
                           const char *tag, const char *newval)
{
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
    memmove(o + nlen, c, (size_t)tail);
    memcpy(o, newval, (size_t)nlen);
    *len += nlen - olen;
    return 0;
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

static int persist_key(const uint8_t *bd, const uint8_t *key, int ktype, const char *name)
{
    int len, blen, i;
    char *buf, *blk, *be, *bak = NULL;
    char norm[13], keyhex[64], bds[24];
    unsigned kwords[16];

    snprintf(norm, sizeof norm, "%02x%02x%02x%02x%02x%02x",
             bd[0], bd[1], bd[2], bd[3], bd[4], bd[5]);
    snprintf(bds, sizeof bds, "%02X:%02X:%02X:%02X:%02X:%02X",
             bd[0], bd[1], bd[2], bd[3], bd[4], bd[5]);
    for (i = 0; i < 16; i++) kwords[i] = key[i];
    snprintf(keyhex, sizeof keyhex,
             "%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
             kwords[0], kwords[1], kwords[2], kwords[3], kwords[4], kwords[5],
             kwords[6], kwords[7], kwords[8], kwords[9], kwords[10], kwords[11],
             kwords[12], kwords[13], kwords[14], kwords[15]);

    buf = xml_load(DEV_XML, &len);
    if (!buf) { mlog("persist_key: cannot read %s", DEV_XML); return -1; }
    bak = xml_load(DEV_XML ".micbak", &blen);
    blk = xml_find_device(buf, len, norm, &be);
    if (blk) {
        char kt[16];
        snprintf(kt, sizeof kt, "%d", ktype);
        if (xml_replace_tag(buf, &len, blk, (int)(be - blk), "link_key", keyhex) != 0 ||
            xml_replace_tag(buf, &len, blk, (int)(be - blk), "Link_key_present", "1") != 0 ||
            xml_replace_tag(buf, &len, blk, (int)(be - blk), "key_type", kt) != 0) {
            mlog("persist_key: entry lacks key fields");
            free(buf); free(bak);
            return -1;
        }
        if (name && name[0])
            xml_replace_tag(buf, &len, blk, (int)(be - blk), "device_name", name);
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
        if (!nm[0] && name && name[0]) snprintf(nm, sizeof nm, "%s", name);
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
            nm, cod, keyhex, ktype, tr, vid, pidv, ver, feat);
        if (bl2 < 0 || bl2 >= (int)sizeof blkbuf) { free(buf); free(bak); return -1; }
        ins = xmemstr(buf, len, "</bt_devices>");
        if (!ins) { free(buf); free(bak); return -1; }
        memmove(ins + bl2, ins, (size_t)(len - (int)(ins - buf)));
        memcpy(ins, blkbuf, (size_t)bl2);
        len += bl2;
        if (xml_save(DEV_XML, buf, len) != 0) { free(buf); free(bak); return -1; }
        mlog("persist_key: appended entry idx=%d for %s name='%s'", idx, bds, nm);
    }
    free(buf);
    free(bak);
    return 0;
}

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
    if (!blk) { free(buf); return 0; }
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

static int xml_has_key(const uint8_t *bd)
{
    int len;
    char *buf, *blk, *be, norm[13], lk[32];
    int rc = 0;
    snprintf(norm, sizeof norm, "%02x%02x%02x%02x%02x%02x",
             bd[0], bd[1], bd[2], bd[3], bd[4], bd[5]);
    buf = xml_load(DEV_XML, &len);
    if (!buf) return 0;
    blk = xml_find_device(buf, len, norm, &be);
    if (blk) {
        lk[0] = 0;
        if (xml_field(blk, (int)(be - blk), "Link_key_present", lk, sizeof lk) >= 0 &&
            atoi(lk) != 0)
            rc = 1;
    }
    free(buf);
    return rc;
}

static int xml_restore_device_from_backup(const uint8_t *bd)
{
    int len, blen;
    char *buf, *bak, *be, *bblk, *bbe, norm[13];
    int rc = -1;

    snprintf(norm, sizeof norm, "%02x%02x%02x%02x%02x%02x",
             bd[0], bd[1], bd[2], bd[3], bd[4], bd[5]);
    buf = xml_load(DEV_XML, &len);
    bak = xml_load(DEV_XML ".micbak", &blen);
    if (!buf || !bak) { free(buf); free(bak); return -1; }
    if (xml_find_device(buf, len, norm, &be)) { free(buf); free(bak); return 0; }
    bblk = xml_find_device(bak, blen, norm, &bbe);
    if (bblk) {
        int inslen = (int)(bbe - bblk);
        char *ins = xmemstr(buf, len, "</bt_devices>");
        if (ins) {
            memmove(ins + inslen, ins, (size_t)(len - (int)(ins - buf)));
            memcpy(ins, bblk, (size_t)inslen);
            len += inslen;
            if (xml_save(DEV_XML, buf, len) == 0) {
                mlog("xml_restore: re-inserted device block from .micbak");
                rc = 0;
            }
        }
    }
    free(buf);
    free(bak);
    return rc;
}

static void update_paired(void)
{
    int p = (E.bd_valid[0] && !E.offline) ? xml_has_key(E.bd[0]) : 0;
    pthread_mutex_lock(&E.st_mtx);
    if (p != E.paired) {
        E.paired = p;
        if (p)
            lookup_name(E.bd[0], E.name, sizeof E.name);
        else
            E.name[0] = 0;
        mlog("paired=%d (xml key entry %s)", p, p ? "present" : "absent");
    }
    pthread_mutex_unlock(&E.st_mtx);
}

/* ------------------------------------------------------------------ */
/* mic.cfg management (dual-mic forward compatible, hot reload,        */
/* pair-success auto-adoption; shared file with the stage-1 engine)    */
/*                                                                     */
/*  Keys read by BOTH wlmicd and the stage-1 miccore engine (the       */
/*  migration-period ownership contract -- only wlmicd ever writes):   */
/*    bd=            mic0 MAC (legacy spelling; kept written for the   */
/*                   old engine, which knows nothing else)             */
/*    auto_connect=  idle retry (engine: in-proc DgOpen; wlmicd:       */
/*                   arm + optional auto-pair)                         */
/*    keepalive_055= 1 Hz RSSI-event 0x55 chain                        */
/*    passive=       engine-only (wlmicd is passive by architecture)   */
/*  wlmicd-only keys (ignored by the old engine's parser):             */
/*    bd0= / bd1=    dual-mic slots (bd0 == bd)                        */
/*    compat=        1 = hook engine present (share nothing it owns)   */
/*    poll_057= / poll_057_sec=      TX status exchange cadence        */
/*    responder=     reactive DUSS answers (cutover only)              */
/*    linkup_refresh_sec=  periodic conn-OK replay, 0 off              */
/*    manage_bt_audio_cfg=  write /data/misc/bluetooth/bt_audio.cfg    */
/*    rssi_poll=     1 Hz 0x582 telemetry                             */
/*    adopt_bd=      pair success rewrites bd0=                        */
/* ------------------------------------------------------------------ */
static void cfg_set_defaults_for_mode(void)
{
    /* cutover defaults; compat (below) tightens them */
    E.auto_connect = 1;
    E.keepalive_055 = 1;
    E.responder = 1;
    E.poll_057 = 1;
    E.poll_057_sec = 10;
    E.linkup_refresh_sec = 60;
    E.manage_bt_audio_cfg = 1;
    E.rssi_poll = 1;
    E.adopt_bd = 1;
    if (E.compat && !E.offline) {
        /* hook engine already answers/keeps-alive and owns the faces */
        E.auto_connect = 0;
        E.keepalive_055 = 0;
        E.responder = 0;
        E.linkup_refresh_sec = 0;
        E.manage_bt_audio_cfg = 0;
    }
}

static int parse_bd_str(const char *s, uint8_t *bd)
{
    char tmp[13];
    int i, j = 0;
    for (i = 0; s[i] && j < 12; i++)
        if (s[i] != ':' && s[i] != '-' && s[i] != '\n' && s[i] != '\r')
            tmp[j++] = s[i];
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

static int read_cfg(int apply)
{
    FILE *f;
    char line[256];
    uint8_t bd0[6], bd1[6];
    int have0 = 0, have1 = 0;
    int compat = E.compat;

    f = fopen(CFG_PATH, "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            if (!strncasecmp(line, "bd0=", 4) || !strncasecmp(line, "bd=", 3)) {
                const char *v = line + (line[2] == '0' ? 4 : 3);
                uint8_t b[6];
                if (parse_bd_str(v, b) == 0 && !have0) { memcpy(bd0, b, 6); have0 = 1; }
            } else if (!strncasecmp(line, "bd1=", 4)) {
                uint8_t b[6];
                if (parse_bd_str(line + 4, b) == 0) { memcpy(bd1, b, 6); have1 = 1; }
            } else if (!strncasecmp(line, "auto_connect=", 13)) {
                E.auto_connect = atoi(line + 13) != 0;
            } else if (!strncasecmp(line, "keepalive_055=", 14)) {
                E.keepalive_055 = atoi(line + 14) != 0;
            } else if (!strncasecmp(line, "passive=", 8)) {
                /* engine-only key; accepted, nothing to do */
            } else if (!strncasecmp(line, "compat=", 7)) {
                compat = atoi(line + 7) != 0;
            } else if (!strncasecmp(line, "poll_057=", 9)) {
                E.poll_057 = atoi(line + 9) != 0;
            } else if (!strncasecmp(line, "poll_057_sec=", 13)) {
                E.poll_057_sec = atoi(line + 13);
                if (E.poll_057_sec < 1) E.poll_057_sec = 1;
                if (E.poll_057_sec > 3600) E.poll_057_sec = 3600;
            } else if (!strncasecmp(line, "responder=", 10)) {
                E.responder = atoi(line + 10) != 0;
            } else if (!strncasecmp(line, "linkup_refresh_sec=", 19)) {
                E.linkup_refresh_sec = atoi(line + 19);
                if (E.linkup_refresh_sec < 0) E.linkup_refresh_sec = 0;
            } else if (!strncasecmp(line, "manage_bt_audio_cfg=", 20)) {
                E.manage_bt_audio_cfg = atoi(line + 20) != 0;
            } else if (!strncasecmp(line, "rssi_poll=", 10)) {
                E.rssi_poll = atoi(line + 10) != 0;
            } else if (!strncasecmp(line, "adopt_bd=", 9)) {
                E.adopt_bd = atoi(line + 9) != 0;
            }
        }
        fclose(f);
    }

    if (apply) {
        E.compat = compat;
        if (have0) { memcpy(E.bd[0], bd0, 6); E.bd_valid[0] = 1; }
        if (have1) { memcpy(E.bd[1], bd1, 6); E.bd_valid[1] = 1; }
        else E.bd_valid[1] = 0;
        if (E.compat && !E.offline) {
            /* HARD CLAMP while the hook engine is alive: the shared
             * mic.cfg's keepalive_055/auto_connect lines address the
             * ENGINE's legs -- honouring them here too would duplicate
             * the 1 Hz 0x55 chain and the pairing retries.  poll_057
             * stays off too: the native 57011 gate drops our dgrams
             * until bt_audio.cfg exists + version sync completed, i.e.
             * until cutover arms it (manage_bt_audio_cfg).  All of
             * these turn on in cutover mode (compat=0). */
            E.keepalive_055 = 0;
            E.responder = 0;
            E.auto_connect = 0;
            E.linkup_refresh_sec = 0;
            E.manage_bt_audio_cfg = 0;
            E.poll_057 = 0;
        }
    }
    return 0;
}

/* rewrite mic.cfg updating bd=/bd0=/bd1= while preserving every other
 * line verbatim (atomic .tmp + rename; only wlmicd writes this file) */
static int cfg_write_bds(const uint8_t *bd0, int have0, const uint8_t *bd1, int have1)
{
    FILE *in = fopen(CFG_PATH, "r"), *out;
    char line[256], tmp[300];
    int wrote0 = 0, wrote1 = 0;
    long ts = (long)now_epoch_ms();

    snprintf(tmp, sizeof tmp, "%s.tmp", CFG_PATH);
    out = fopen(tmp, "w");
    if (!out) { if (in) fclose(in); return -1; }
    if (in) {
        while (fgets(line, sizeof line, in)) {
            if (!strncasecmp(line, "bd=", 3) || !strncasecmp(line, "bd0=", 4)) {
                if (have0 && !wrote0) {
                    fprintf(out, "bd=%02x:%02x:%02x:%02x:%02x:%02x\n",
                            bd0[0], bd0[1], bd0[2], bd0[3], bd0[4], bd0[5]);
                    fprintf(out, "bd0=%02x:%02x:%02x:%02x:%02x:%02x\n",
                            bd0[0], bd0[1], bd0[2], bd0[3], bd0[4], bd0[5]);
                    wrote0 = 1;
                }
                continue;               /* drop old copies */
            }
            if (!strncasecmp(line, "bd1=", 4)) {
                if (have1 && !wrote1) {
                    fprintf(out, "bd1=%02x:%02x:%02x:%02x:%02x:%02x\n",
                            bd1[0], bd1[1], bd1[2], bd1[3], bd1[4], bd1[5]);
                    wrote1 = 1;
                }
                continue;
            }
            fputs(line, out);
        }
        fclose(in);
    }
    if (have0 && !wrote0) {
        fprintf(out, "bd=%02x:%02x:%02x:%02x:%02x:%02x\n",
                bd0[0], bd0[1], bd0[2], bd0[3], bd0[4], bd0[5]);
        fprintf(out, "bd0=%02x:%02x:%02x:%02x:%02x:%02x\n",
                bd0[0], bd0[1], bd0[2], bd0[3], bd0[4], bd0[5]);
    }
    if (have1 && !wrote1) {
        fprintf(out, "bd1=%02x:%02x:%02x:%02x:%02x:%02x\n",
                bd1[0], bd1[1], bd1[2], bd1[3], bd1[4], bd1[5]);
    }
    fprintf(out, "# updated by wlmicd %ld\n", ts);
    fclose(out);
    if (rename(tmp, CFG_PATH) != 0) { unlink(tmp); return -1; }
    chmod(CFG_PATH, 0666);
    return 0;
}

static int bt_audio_cfg_sync(void);

/* pair success (or an explicitly adopted bd) -> mic0 slot becomes the
 * new device; the previous mic0 is NOT demoted to mic1 (the product
 * treats mic0/mic1 as user-assigned teaming slots; mixing them behind
 * the user's back would surprise the dual-mic workflow) */
static void cfg_adopt_bd(const uint8_t *bd)
{
    if (!E.adopt_bd) return;
    if (E.bd_valid[0] && memcmp(E.bd[0], bd, 6) == 0) return;   /* unchanged */
    if (cfg_write_bds(bd, 1, E.bd[1], E.bd_valid[1] &&
                      memcmp(E.bd[1], bd, 6) != 0) != 0) {
        mlog("cfg_adopt: mic.cfg rewrite failed: %s", strerror(errno));
        return;
    }
    memcpy(E.bd[0], bd, 6);
    E.bd_valid[0] = 1;
    if (E.bd_valid[1] && memcmp(E.bd[1], bd, 6) == 0) E.bd_valid[1] = 0;
    mlog("cfg_adopt: mic.cfg bd0 -> %02x:%02x:%02x:%02x:%02x:%02x (hot)",
         bd[0], bd[1], bd[2], bd[3], bd[4], bd[5]);
    update_paired();
    bt_audio_cfg_sync();
}

/* ------------------------------------------------------------------ */
/* bt_audio.cfg manager (cutover): product-mirror of dji_network's     */
/* update_config -- one "%02x:.." line per configured mic.  bt_bsa_app */
/* re-reads it on connect/time-sync events; with the slot present the  */
/* native version sync completes and the 57011 downlink gate opens.    */
/* ------------------------------------------------------------------ */
static int bt_audio_cfg_sync(void)
{
    char want[80];
    int wl = 0;
    char *cur;
    int curlen;

    if (E.offline || !E.manage_bt_audio_cfg) return 0;
    if (E.bd_valid[0]) wl += snprintf(want + wl, sizeof want - (size_t)wl,
                                      "%02x:%02x:%02x:%02x:%02x:%02x\n",
                                      E.bd[0][0], E.bd[0][1], E.bd[0][2],
                                      E.bd[0][3], E.bd[0][4], E.bd[0][5]);
    if (E.bd_valid[1]) wl += snprintf(want + wl, sizeof want - (size_t)wl,
                                      "%02x:%02x:%02x:%02x:%02x:%02x\n",
                                      E.bd[1][0], E.bd[1][1], E.bd[1][2],
                                      E.bd[1][3], E.bd[1][4], E.bd[1][5]);
    cur = xml_load(BT_AUD_CFG, &curlen);
    if (cur) {
        int same = (curlen == wl) && (wl == 0 || memcmp(cur, want, (size_t)wl) == 0);
        free(cur);
        if (same) return 0;
    }
    if (xml_save(BT_AUD_CFG, want, wl) != 0) {
        mlog("bt_audio_cfg: write failed: %s", strerror(errno));
        return -1;
    }
    mlog("bt_audio_cfg: wrote %d slot line(s) (native gate arms on next "
         "connect/version-sync)", E.bd_valid[0] + E.bd_valid[1]);
    return 0;
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
}

static void rec_write(const int16_t *s, int n)
{
    pthread_mutex_lock(&E.rec_mtx);
    if (E.rec_f) {
        if (fwrite(s, 2, (size_t)n, E.rec_f) == (size_t)n)
            E.rec_bytes += (uint64_t)n * 2;
    }
    pthread_mutex_unlock(&E.rec_mtx);
}

static void wav_header(FILE *f, uint32_t bytes)
{
    uint8_t h[44];
    memset(h, 0, 44);
    memcpy(h, "RIFF", 4);
    *(uint32_t *)(h + 4) = 36 + bytes;
    memcpy(h + 8, "WAVEfmt ", 8);
    *(uint32_t *)(h + 16) = 16;
    *(uint16_t *)(h + 20) = 1;
    *(uint16_t *)(h + 22) = 1;
    *(uint32_t *)(h + 24) = SRATE;
    *(uint32_t *)(h + 28) = SRATE * 2;
    *(uint16_t *)(h + 32) = 2;
    *(uint16_t *)(h + 34) = 16;
    memcpy(h + 36, "data", 4);
    *(uint32_t *)(h + 40) = bytes;
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
}

static int rec_start(int sec, const char *path)
{
    static const uint8_t zero44[44] = {0};
    char pbuf[256];
    pthread_mutex_lock(&E.rec_mtx);
    if (E.rec_f) { pthread_mutex_unlock(&E.rec_mtx); return -1; }
    if (!path || !path[0]) {
        if (E.offline) {
            snprintf(pbuf, sizeof pbuf, "wlmicd_replay_%llu.wav",
                     (unsigned long long)(now_epoch_ms() / 1000));
            path = pbuf;
        } else {
            const char *rdir = "/mnt/media_rw/ssd/mic";
            mkdir(rdir, 0777);
            snprintf(pbuf, sizeof pbuf, "%s/rec_%llu.wav", rdir,
                     (unsigned long long)(now_epoch_ms() / 1000));
            path = pbuf;
        }
    }
    E.rec_f = fopen(path, "wb");
    if (!E.rec_f) {
        pthread_mutex_unlock(&E.rec_mtx);
        mlog("rec_start: fopen %s: %s", path, strerror(errno));
        return -1;
    }
    fwrite(zero44, 1, 44, E.rec_f);
    snprintf(E.rec_path, sizeof E.rec_path, "%s", path);
    E.rec_bytes = 0;
    E.rec_deadline_ms = sec > 0 ? now_ms() + (uint64_t)sec * 1000 : 0;
    pthread_mutex_unlock(&E.rec_mtx);
    mlog("rec_start: %s sec=%d", path, sec);
    return 0;
}

static int rec_stop(char *path_out, int path_sz, uint64_t *bytes_out)
{
    FILE *f;
    pthread_mutex_lock(&E.rec_mtx);
    if (!E.rec_f) { pthread_mutex_unlock(&E.rec_mtx); return -1; }
    f = E.rec_f;
    E.rec_f = NULL;
    fflush(f);
    wav_header(f, (uint32_t)E.rec_bytes);
    fclose(f);
    if (path_out) snprintf(path_out, path_sz, "%s", E.rec_path);
    if (bytes_out) *bytes_out = E.rec_bytes;
    mlog("rec_stop: %s %llu bytes", E.rec_path, (unsigned long long)E.rec_bytes);
    E.rec_deadline_ms = 0;
    pthread_mutex_unlock(&E.rec_mtx);
    return 0;
}

static void pcm_dispatch(const int16_t *s, int n)
{
    double sum = 0;
    int i;
    for (i = 0; i < n; i++) sum += (double)s[i] * s[i];
    if (n > 0) E.level = sqrt(sum / n) / 32768.0;
    if (E.fifo_enabled) fifo_write(s, n);
    rec_write(s, n);
}

static void pcm_fill(int frames)
{
    static const int16_t z[NSAMP] = {0};
    int i;
    for (i = 0; i < frames; i++) {
        E.drops++;
        if (decode_plc_frame() == 0)
            pcm_dispatch(pcm_frame, dec_nsamp);
        else {
            if (E.fifo_enabled) fifo_write(z, dec_nsamp);
            rec_write(z, dec_nsamp);
        }
    }
    E.level = 0.0;
}

/* ------------------------------------------------------------------ */
/* status TLVs (Wl cmd 0x19 payload, libwirelessmicservice 0x39a20)    */
/* ------------------------------------------------------------------ */
static void status_write(void);

static void handle_status_tlvs(const uint8_t *p, int n)
{
    int i = 0;
    int changed = 0;
    while (i < n) {
        uint8_t t;
        if (p[i] == 0xff) { i++; continue; }
        if (i + 2 > n) break;
        t = p[i];
        {
            int ln = p[i + 1];
            if (i + 2 + ln > n) break;
            if (ln >= 1) {
                int v = p[i + 2] ? 1 : 0;
                int known = 1;
                pthread_mutex_lock(&E.st_mtx);
                if (t == 0x10) {
                    if (E.battery != p[i + 2]) mlog("battery=%d%%", p[i + 2]);
                    E.battery = p[i + 2];
                    changed = 1;
                } else if (t == 0x05) {
                    if (E.charging != v) mlog("charging=%d", v);
                    E.charging = v;
                    changed = 1;
                } else if (t == 0x01) {
                    if (E.mute != v) mlog("mute=%d", v);
                    E.mute = v;
                    changed = 1;
                } else if (t == 0x06) {
                    if (E.recording != v) mlog("recording=%d", v);
                    E.recording = v;
                    changed = 1;
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
    if (changed) status_write();   /* INTERFACE.md: refresh on events too */
}

/* ================================================================== */
/* UNIFIED STREAM PARSER + PASSIVE RESPONDER SET (miccore v3 lineage)  */
/* ================================================================== */
static int link_up_gate(void)
{
    int ok;
    pthread_mutex_lock(&E.st_mtx);
    ok = E.connected || E.state == ST_STREAMING;
    pthread_mutex_unlock(&E.st_mtx);
    return ok;
}

/* 0x15 -> 0x56 verbatim echo */
static void resp_0x56_echo(const uint8_t *body, int blen)
{
    if (!E.bd_valid[0]) return;
    if (blen >= 6 + 4 + 2 + 8) {
        pthread_mutex_lock(&E.st_mtx);
        E.ts_btclk = (uint32_t)(body[6] | (body[7] << 8) |
                                (body[8] << 16) | ((uint32_t)body[9] << 24));
        E.ts_intra = (uint16_t)(body[10] | (body[11] << 8));
        E.ts_valid = 1;
        E.ts_ms = now_epoch_ms();
        pthread_mutex_unlock(&E.st_mtx);
    }
    rq_push_duss(0x56, body, blen, 0x18, E.bd[0]);
    mlog("resp: 0x15 -> 0x56 echo (%dB body)", blen);
}

/* 0x17 -> 0x55 {quality=clamp(dBm+100,0,127), bd} */
static void resp_0x55(void)
{
    uint8_t pl[8];
    int dbm, q;
    if (!E.bd_valid[0]) return;
    dbm = E.rssi ? E.rssi : -60;
    q = dbm + 100;
    if (q < 0) q = 0;
    if (q > 127) q = 127;
    pl[0] = (uint8_t)q;
    memcpy(pl + 1, E.bd[0], 6);
    rq_push_duss(0x55, pl, 7, 0x18, E.bd[0]);
}

/* proactive 0x55 (RSSI-event leg / keepalive) */
static void ka_055_tick(void)
{
    if (!E.keepalive_055 || !E.bd_valid[0]) return;
    resp_0x55();
}

/* 0x57 get_status ANSWER (16+6n, REPORT_ctrl_frames 2.1) */
static void build_0x57_payload(uint8_t *pl, const uint8_t *bd,
                               uint32_t btclk, uint16_t intra, uint64_t systime)
{
    int i;
    pl[0] = 0x00;
    pl[1] = (uint8_t)(btclk & 0xff);
    pl[2] = (uint8_t)((btclk >> 8) & 0xff);
    pl[3] = (uint8_t)((btclk >> 16) & 0xff);
    pl[4] = (uint8_t)((btclk >> 24) & 0xff);
    pl[5] = (uint8_t)(intra & 0xff);
    pl[6] = (uint8_t)(intra >> 8);
    for (i = 0; i < 8; i++) pl[7 + i] = (uint8_t)((systime >> (8 * i)) & 0xff);
    pl[15] = 1;
    memcpy(pl + 16, bd, 6);
}

static void resp_0x57(void)
{
    uint8_t pl[22];
    uint32_t btclk;
    uint16_t intra;
    if (!E.bd_valid[0]) return;
    pthread_mutex_lock(&E.st_mtx);
    btclk = E.ts_valid ? E.ts_btclk : 0;
    intra = E.ts_valid ? E.ts_intra : 0;
    pthread_mutex_unlock(&E.st_mtx);
    build_0x57_payload(pl, E.bd[0], btclk, intra, now_epoch_ms());
    rq_push_duss(0x57, pl, 22, 0x18, E.bd[0]);
    mlog("resp: 0x57 -> answer btclk=0x%08x intra=%u count=1", btclk, intra);
}

/* PROACTIVE 0x57 status poll (task phase-1 spec): asks the TX for its
 * status with the product 16+6n shape.  btclk/intra from the freshest
 * cached clock (0x58 exchange, else the newest audio-unit header),
 * systime = now.  Sent regardless of the TX answer's visibility -- on
 * this firmware bt_bsa_app drops TX->camera control frames before
 * 57012, so the measurable effect is the TX entering its status
 * reporting state (Wl 0x19 TLV blocks in the audio region). */
static void poll_0x57_tick(void)
{
    uint8_t pl[22];
    uint32_t btclk;
    uint16_t intra;
    uint64_t sys;
    if (!E.poll_057 || !E.bd_valid[0] || E.offline) return;
    pthread_mutex_lock(&E.st_mtx);
    btclk = E.ts_valid ? E.ts_btclk : 0;
    intra = E.ts_valid ? E.ts_intra : 0;
    sys = E.ts_valid ? E.ts_ms : now_epoch_ms();
    pthread_mutex_unlock(&E.st_mtx);
    build_0x57_payload(pl, E.bd[0], btclk, intra, sys);
    rq_push_duss(0x57, pl, 22, 0x18, E.bd[0]);
    mlog("poll: 0x57 status exchange sent (btclk=0x%08x intra=%u)", btclk, intra);
}

/* conn-OK pair (dji_network five-step subset): sync-addr + link-up.
 * Receiver 0x3a + these exact shapes are live-proven (mic.log 03:14,
 * stream sustained).  Used as periodic refresh and by ctl kickstart. */
static void send_conn_ok_pair(void)
{
    uint8_t pl[16];
    if (!E.bd_valid[0]) return;
    pl[0] = 0x03; pl[1] = 0x02;
    memcpy(pl + 2, E.bd[0], 6);
    memcpy(pl + 8, E.bd[0], 6);
    rq_push_duss(0x65, pl, 14, 0x3a, E.bd[0]);
    pl[0] = 0x01; pl[1] = 0x10;
    memcpy(pl + 2, E.bd[0], 6);
    rq_push_duss(0x54, pl, 8, 0x3a, E.bd[0]);
    mlog("conn-OK pair: sync-addr 0x65/03 + link-up 0x54/10 queued");
}

/* kickstart ladder via the native downlink (ctl command; cutover).
 * Version-sync announce + time-sync req + conn-OK pair; ccode is NOT
 * sent: fw316 TX streams without it (live-proven, PASSIVE session had
 * no ccode) and the exact WMS-cmd-19 wrapper is not reachable
 * externally.  Byte shapes from REPORT_audio_start 3.2. */
static void kickstart_ladder(void)
{
    uint8_t pl[4];
    if (!E.bd_valid[0]) return;
    pl[0] = 0x04; pl[1] = 0x01;                    /* version sync {04, cur=1} */
    rq_push_duss(0x65, pl, 2, 0x3a, E.bd[0]);
    pl[0] = 0x01;                                  /* time-sync request */
    rq_push_duss(0x58, pl, 1, 0x3a, E.bd[0]);
    send_conn_ok_pair();
    mlog("kickstart: version-sync + time-sync-req + conn-OK queued via 57011");
}

/* sleep notice before teardown: 0x54 {00,0x20,bd} + 200ms grace
 * (feature_bt_audio_stop@0x489c40) */
static void announce_sleep_0x54_20(void)
{
    uint8_t pl[8];
    if (!E.bd_valid[0] || E.offline) return;
    pl[0] = 0x00;
    pl[1] = 0x20;
    memcpy(pl + 2, E.bd[0], 6);
    rq_push_duss(0x54, pl, 8, 0x18, E.bd[0]);
    mlog("sleep-announce: 0x54{00,20,bd} queued (+200ms grace)");
    usleep(200 * 1000);
}

/* 0x58 subtype handling (request latch + answer echo) */
static void resp_0x56_from_ts(const uint8_t *body, int blen)
{
    uint8_t pl[20];
    uint32_t btclk = 0;
    uint16_t intra = 0;
    int off = -1;
    if (!E.bd_valid[0]) return;
    if (blen >= 1 && body[0] == 0x02) off = 0;
    else if (blen >= 2 && body[1] == 0x02) off = 1;
    if (off >= 0 && blen >= off + 7) {
        int cb = (off == 1) ? off + 1 : off + 2;
        btclk = (uint32_t)(body[cb] | (body[cb + 1] << 8) |
                           (body[cb + 2] << 16) | ((uint32_t)body[cb + 3] << 24));
        intra = (uint16_t)(body[cb + 4] | (body[cb + 5] << 8));
        pthread_mutex_lock(&E.st_mtx);
        E.ts_btclk = btclk;
        E.ts_intra = intra;
        E.ts_valid = 1;
        E.ts_ms = now_epoch_ms();
        pthread_mutex_unlock(&E.st_mtx);
    }
    memcpy(pl, E.bd[0], 6);
    pl[6] = (uint8_t)(btclk & 0xff);
    pl[7] = (uint8_t)((btclk >> 8) & 0xff);
    pl[8] = (uint8_t)((btclk >> 16) & 0xff);
    pl[9] = (uint8_t)((btclk >> 24) & 0xff);
    pl[10] = (uint8_t)(intra & 0xff);
    pl[11] = (uint8_t)(intra >> 8);
    {
        pthread_mutex_lock(&E.st_mtx);
        uint64_t st = E.ts_ms ? E.ts_ms : now_epoch_ms();
        pthread_mutex_unlock(&E.st_mtx);
        int i;
        for (i = 0; i < 8; i++) pl[12 + i] = (uint8_t)((st >> (8 * i)) & 0xff);
    }
    rq_push_duss(0x56, pl, 20, 0x18, E.bd[0]);
    mlog("resp: 0x58/02 -> 0x56 btclk=0x%08x intra=%u", btclk, intra);
}

static void resp_0x58_01(void)
{
    uint8_t pl[1];
    if (!E.bd_valid[0]) return;
    pthread_mutex_lock(&E.st_mtx);
    E.ts_ms = now_epoch_ms();       /* latch request time for the 0x56 systime */
    pthread_mutex_unlock(&E.st_mtx);
    pl[0] = 0x02;
    rq_push_duss(0x58, pl, 1, 0x3a, E.bd[0]);
    mlog("resp: 0x58/01 -> 0x58/02 {02}");
}

static void duss_dispatch(const uint8_t *d, int n)
{
    int dl = ((d[2] & 3) << 8) | d[1];
    uint8_t set = d[9], cmd = d[10];
    uint16_t seq = (uint16_t)(d[6] | (d[7] << 8));
    const uint8_t *pl = d + 11;
    int plen = dl - 13;

    if (set != 0x12) {
        /* TX announcements in unknown sets (0x00/0x02/0x04 observed live,
         * 0.8-1.5 Hz constant payloads): rate-limited logging only */
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

    mlog("duss_rx set=12 cmd=0x%02x seq=%u plen=%d "
         "snd=%02x rcv=%02x fl=%02x %02x%02x%02x%02x%02x%02x%02x%02x",
         cmd, seq, plen, d[4], d[5], d[8],
         pl[0], plen > 1 ? pl[1] : 0, plen > 2 ? pl[2] : 0, plen > 3 ? pl[3] : 0,
         plen > 4 ? pl[4] : 0, plen > 5 ? pl[5] : 0, plen > 6 ? pl[6] : 0,
         plen > 7 ? pl[7] : 0, plen > 8 ? pl[8] : 0);

    /* Bring-up exchanges first (TX stalls without the replies).  On this
     * firmware these frames are natively consumed before 57012, so the
     * reactive legs only fire when DUSS rides a dgram payload (offline
     * tests, future firmware) -- hence the E.responder gate. */
    if (!E.responder && !E.offline) {
        mlog("duss_rx: responder disabled (compat mode) -- log only");
        return;
    }
    switch (cmd) {
    case 0x58:
        if ((plen >= 1 && pl[0] == 0x01) ||
            (plen >= 2 && pl[0] == 0x00 && pl[1] == 0x01)) {
            resp_0x58_01();
        } else if (((plen >= 1 && pl[0] == 0x02) ||
                    (plen >= 2 && pl[1] == 0x02)) && plen >= 8) {
            resp_0x56_from_ts(pl, plen);
        }
        return;
    case 0x65:
        if (plen >= 1 && pl[0] == 0x00) {
            send_conn_ok_pair();       /* TX ACK -> idempotent replay */
            mlog("resp: 0x65/00 -> conn-OK replay");
        }
        return;
    default:
        break;
    }

    if (!link_up_gate()) {
        mlog("duss_rx: mic cmd 0x%02x before link-up gate -- no reply", cmd);
        return;
    }
    switch (cmd) {
    case 0x15: resp_0x56_echo(pl, plen); break;
    case 0x17: resp_0x55(); break;
    case 0x57: resp_0x57(); break;
    default: break;
    }
}

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

/* ---------- Wl block handling (bare or AA-stripped) --------------- */
static void wl_block_dispatch(uint8_t *p)
{
    uint8_t cmd = p[4];
    pthread_mutex_lock(&E.st_mtx);
    if (E.state == ST_CONNECTING || E.state == ST_RECONNECTING) E.state = ST_STREAMING;
    E.connected = 1;
    pthread_mutex_unlock(&E.st_mtx);

    if (cmd == 0x10) {
        uint8_t seq = p[6];
        int plen = 119 - 8;
        E.last_audio_ms = now_ms();
        E.last_pad_ms = E.last_audio_ms;
        if (E.have_seq) {
            int dd = (seq - E.last_seq) & 0xff;
            if (dd == 0 || dd >= 200) return;
            if (dd > 1) {
                int want = (dd - 1 > 40) ? 40 : dd - 1;
                pcm_fill(want);
            }
        }
        E.have_seq = 1;
        E.last_seq = seq;
        if (!dec) return;
        memset(pcm_frame, 0, sizeof pcm_frame);
        if (decode_frame(p + 7, plen) == 0) {
            E.frames++;
            pcm_dispatch(pcm_frame, dec_nsamp);
        }
    } else if (cmd == 0x19) {
        handle_status_tlvs(p + 7, 119 - 8);
    }
}

static void wl_scan(uint8_t *p, int n)
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
        if ((s & 0x3f) != (b[3] & 0x3f)) { pos = b - p + 1; continue; }
        wl_block_dispatch(b);
        pos = b - p + 119;
    }
}

static void dispatch_aa(uint8_t *payload, int plen, unsigned ver,
                        unsigned cnt, unsigned tlen)
{
    if (ver == 1) {
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
                clen = l16 & 0x3fff;
                if ((unsigned long)off + 3 + clen > (unsigned long)plen) { ok = 0; break; }
            }
            if (id == 2 && clen >= 13 && duss_valid(payload + off + 3, (int)clen))
                duss_dispatch(payload + off + 3, (int)clen);
            else
                wl_scan(payload + off + 3, (int)clen);
            any = 1;
            off += 3 + (int)clen;
        }
        if (!ok || !any)
            wl_scan(payload, plen);
        return;
    }

    if (cnt > 0)
        wl_scan(payload, (int)(cnt * 125));
    if (tlen > 0) {
        uint8_t *tail = payload + cnt * 125;
        if (duss_valid(tail, (int)tlen))
            duss_dispatch(tail, (int)tlen);
        else
            wl_scan(tail, (int)tlen);
    }
}

static void parse_stream(void)
{
    int pos = 0;
    while (pos < E.slen) {
        uint8_t b = E.sbuf[pos];
        unsigned l16, ver, cnt, tlen, plen, end;

        if (b == 0xAA && E.slen - pos >= 3) {
            l16 = ((unsigned)E.sbuf[pos + 1] << 8) | E.sbuf[pos + 2];
            ver = l16 >> 14;
            cnt = (l16 >> 10) & 0xF;
            tlen = l16 & 0x3FF;
            plen = cnt * 125 + tlen;
            if (plen >= 4 && plen <= (unsigned)15 * 125 + 1023) {
                end = (unsigned)pos + 3 + plen;
                if (end >= (unsigned)E.slen) break;
                if (E.sbuf[end] == 0xA5) {
                    dispatch_aa(E.sbuf + pos + 3, (int)plen, ver, cnt, tlen);
                    pos = (int)end + 1;
                    continue;
                }
            }
            pos++;
            continue;
        }
        if (b == 0x55 && E.slen - pos >= 3 &&
            E.sbuf[pos + 1] == 0x77 && E.sbuf[pos + 2] == 0x05) {
            if (E.slen - pos < 119) break;
            {
                unsigned s = 0;
                int i;
                for (i = 4; i <= 117; i++) s += E.sbuf[pos + i];
                if ((s & 0x3f) == (E.sbuf[pos + 3] & 0x3f)) {
                    wl_block_dispatch(E.sbuf + pos);
                    pos += 119;
                    continue;
                }
            }
            pos++;
            continue;
        }
        pos++;
    }
    if (pos > 0) {
        memmove(E.sbuf, E.sbuf + pos, (size_t)(E.slen - pos));
        E.slen -= pos;
    }
}

static void stream_feed(const uint8_t *buf, int len)
{
    if (!E.started || len <= 0) return;
    pthread_mutex_lock(&E.feed_mtx);
    if (E.slen + len > (int)sizeof E.sbuf) E.slen = 0;
    memcpy(E.sbuf + E.slen, buf, (size_t)len);
    E.slen += len;
    parse_stream();
    pthread_mutex_unlock(&E.feed_mtx);
}

/* ------------------------------------------------------------------ */
/* 57012 dgram input (the ONLY live feed; also used verbatim by the    */
/* offline replay):                                                   */
/*   [0]=0x01 [1]=seq8 [2..3]=total BE16 (=10+payload_len)            */
/*   [4..9]=source bd  [10..]=payload                                 */
/* payload = N x 125B audio units ({btclk u32le, intra u16le} + 119B  */
/* Wl) in the common case; AA ver0/ver1 frames and bare DUSS are      */
/* handled by the generic parser when present.                        */
/* ------------------------------------------------------------------ */
static void dgram_input(const uint8_t *dgram, int n)
{
    unsigned total;
    const uint8_t *bd, *payload;
    int plen, ours;

    if (n < 11) return;
    total = ((unsigned)dgram[2] << 8) | dgram[3];
    if ((int)total > n) total = (unsigned)n;      /* tolerate short read */
    plen = (int)total - 10;
    if (plen <= 0) return;
    bd = dgram + 4;
    payload = dgram + 10;
    E.dgrams++;

    ours = (E.bd_valid[0] && memcmp(bd, E.bd[0], 6) == 0) ||
           (E.bd_valid[1] && memcmp(bd, E.bd[1], 6) == 0);
    if (!ours) {
        static uint8_t logged_bd[6];
        static int logged;
        E.dgrams_other_bd++;
        if (!logged || memcmp(logged_bd, bd, 6) != 0) {
            memcpy(logged_bd, bd, 6);
            logged = 1;
            mlog("dgram from unconfigured bd %02x:%02x:%02x:%02x:%02x:%02x "
                 "(decode skipped; micctl pair <bd> to adopt)",
                 bd[0], bd[1], bd[2], bd[3], bd[4], bd[5]);
        }
        return;
    }

    /* common case first: whole payload = 125B audio units.  Harvest the
     * BT-clock headers (the freshest native clock we ever see on this
     * channel) and feed only the Wl blocks. */
    if (plen % 125 == 0 && plen >= 125 && payload[6] == 0x55 &&
        payload[7] == 0x77 && payload[8] == 0x05) {
        int u;
        pthread_mutex_lock(&E.st_mtx);
        for (u = 0; u < plen / 125; u++) {
            const uint8_t *unit = payload + u * 125;
            if (unit[6] == 0x55 && unit[7] == 0x77 && unit[8] == 0x05) {
                E.ts_btclk = (uint32_t)(unit[0] | (unit[1] << 8) |
                                        (unit[2] << 16) | ((uint32_t)unit[3] << 24));
                E.ts_intra = (uint16_t)(unit[4] | (unit[5] << 8));
                E.ts_ms = now_epoch_ms();
                E.ts_valid = 1;
            }
        }
        pthread_mutex_unlock(&E.st_mtx);
        for (u = 0; u < plen / 125; u++)
            stream_feed(payload + u * 125 + 6, 119);
        E.dgrams_audio++;
        return;
    }
    /* generic: AA frames / ver1 subpackets / bare stream */
    stream_feed(payload, plen);
}

/* RX thread: bind /dsock/57012 and pump dgrams */
static int up_fd = -1;

static int up_bind(void)
{
    struct sockaddr_un a;
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    int rcv = 512 * 1024;
    if (fd < 0) {
        mlog("dsock57012: socket: %s", strerror(errno));
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof rcv);
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    a.sun_path[0] = 0;
    snprintf(a.sun_path + 1, sizeof a.sun_path - 1, DSOCK_UP);
    if (bind(fd, (struct sockaddr *)&a,
             (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(DSOCK_UP))) < 0) {
        mlog("dsock57012: bind %s: %s (another consumer running?)",
             DSOCK_UP, strerror(errno));
        close(fd);
        return -1;
    }
    up_fd = fd;
    mlog("dsock57012: bound (bt_bsa_app uplink consumer)");
    return 0;
}

static void *rx_thread(void *arg)
{
    uint8_t buf[DG_MAX];
    (void)arg;
    while (E.rx_running) {
        ssize_t r = recv(up_fd, buf, sizeof buf, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            mlog("dsock57012: recv: %s", strerror(errno));
            sleep(1);
            continue;
        }
        dgram_input(buf, (int)r);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* status JSON (INTERFACE.md v3 shape, unchanged for the GUI)          */
/* ------------------------------------------------------------------ */
static int status_json(char *out, int outsz)
{
    int n;
    char bds[24] = "";
    char bdq[32] = "null";
    char name[64] = "";
    char nmq[80] = "null";
    char batt[16] = "null";
    char rssis[16] = "null";
    char chgs[16] = "null", mutes[16] = "null", recs[16] = "null";
    int paired, charging, mute, recording;

    pthread_mutex_lock(&E.st_mtx);
    paired = E.paired;
    charging = E.charging;
    mute = E.mute;
    recording = E.recording;
    if (paired && E.bd_valid[0]) {
        snprintf(bds, sizeof bds, "%02x:%02x:%02x:%02x:%02x:%02x",
                 E.bd[0][0], E.bd[0][1], E.bd[0][2],
                 E.bd[0][3], E.bd[0][4], E.bd[0][5]);
        snprintf(bdq, sizeof bdq, "\"%s\"", bds);
        snprintf(name, sizeof name, "%s", E.name);
        snprintf(nmq, sizeof nmq, "\"%s\"", name);
        snprintf(batt, sizeof batt, E.battery >= 0 ? "%d" : "null", E.battery);
        snprintf(rssis, sizeof rssis, E.rssi ? "%d" : "null", E.rssi);
        snprintf(chgs, sizeof chgs, charging >= 0 ? (charging ? "true" : "false") : "null");
        snprintf(mutes, sizeof mutes, mute >= 0 ? (mute ? "true" : "false") : "null");
        snprintf(recs, sizeof recs, recording >= 0 ? (recording ? "true" : "false") : "null");
    }
    n = snprintf(out, (size_t)outsz,
        "{\"paired\":%s,\"connected\":%s,\"bd\":%s,\"name\":%s,\"battery\":%s,"
        "\"charging\":%s,\"mute\":%s,\"recording\":%s,"
        "\"rssi\":%s,\"frames\":%llu,\"drops\":%llu,\"level\":%.3f,"
        "\"state\":\"%s\",\"ts\":%llu}",
        paired ? "true" : "false",
        (paired && E.connected) ? "true" : "false",
        bdq, nmq, batt, chgs, mutes, recs, rssis,
        (unsigned long long)E.frames,
        (unsigned long long)E.drops,
        E.level,
        ST_NAME[E.state],
        (unsigned long long)now_epoch_ms());
    pthread_mutex_unlock(&E.st_mtx);
    return n;
}

static void status_write(void)
{
    char buf[1024];
    int n = status_json(buf, sizeof buf);
    int fd;
    char tmp[160];
    if (n <= 0 || E.p_status[0] == 0) return;
    snprintf(tmp, sizeof tmp, "%s.tmp", E.p_status);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    if (write(fd, buf, (size_t)n) == n) {
        buf[0] = '\n';
        write(fd, buf, 1);
    }
    close(fd);
    rename(tmp, E.p_status);
}

/* ------------------------------------------------------------------ */
/* control commands (scan / pair / unpair) over the bsa daemon socket  */
/* ------------------------------------------------------------------ */
static int ctl_scan(int sec, char *resp, int rsz)
{
    unsigned char p[40], r[MAXPL];
    int n, i, off;
    int units = sec < 1 ? 1 : (sec > 48 ? 48 : sec);

    if (E.offline) { snprintf(resp, (size_t)rsz, "FAIL offline"); return -1; }
    if (bsa_conn() < 0) { snprintf(resp, (size_t)rsz, "FAIL no-bsa-socket"); return -1; }
    memset(p, 0, sizeof p);
    *(uint32_t *)p = (uint32_t)units;
    p[0x20] = 0x01;
    B.nseen = 0;
    B.scan_mode = 1;
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

    off = snprintf(resp, (size_t)rsz, "[");
    for (i = 0; i < B.nseen; i++) {
        int is_dji = (B.seen[i].name[0] == 'D' && B.seen[i].name[1] == 'J' &&
                      B.seen[i].name[2] == 'I');
        if (!is_dji) continue;
        off += snprintf(resp + off, (size_t)(rsz - off),
                        "%s{\"bd\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"name\":\"%s\","
                        "\"rssi\":%d,\"cod\":%u}",
                        off > 1 ? "," : "",
                        B.seen[i].bd[0], B.seen[i].bd[1], B.seen[i].bd[2],
                        B.seen[i].bd[3], B.seen[i].bd[4], B.seen[i].bd[5],
                        B.seen[i].name, B.seen[i].rssi, B.seen[i].cod);
        if (off >= rsz - 64) break;
    }
    snprintf(resp + off, (size_t)(rsz - off), "]");
    return 0;
}

static int ctl_pair(const uint8_t *bd, char *resp, int rsz)
{
    unsigned char z2[2] = {0, 0};
    uint64_t deadline;
    int i, nz;

    if (E.offline) { snprintf(resp, (size_t)rsz, "FAIL offline"); return -1; }
    if (bsa_conn() < 0) { snprintf(resp, (size_t)rsz, "FAIL no-bsa-socket"); return -1; }
    pthread_mutex_lock(&B.mtx);
    bsa_send(0x12C, z2, 2);
    bsa_pump2(500, 0);
    B.bond_ok = 0;
    memset(B.bond_key, 0, 16);
    bsa_send(0x12D, bd, 6);
    deadline = now_ms() + 30000;
    while (!B.bond_ok && now_ms() < deadline)
        if (bsa_pump2(1000, 0) < 0 && B.fd < 0) break;
    if (B.bond_ok != 1) {
        pthread_mutex_unlock(&B.mtx);
        snprintf(resp, (size_t)rsz, "FAIL bond-timeout-or-rejected");
        return -1;
    }
    nz = 0;
    for (i = 0; i < 16; i++) if (B.bond_key[i]) nz = 1;
    if (!nz) {
        pthread_mutex_unlock(&B.mtx);
        if (xml_restore_device_from_backup(bd) == 0) {
            snprintf(resp, (size_t)rsz, "OK restored-from-backup");
            if (E.bd_valid[0] && !memcmp(E.bd[0], bd, 6)) update_paired();
            return 0;
        }
        snprintf(resp, (size_t)rsz, "FAIL no-link-key");
        return -1;
    }
    if (memcmp(B.bond_bd, bd, 6) != 0) {
        pthread_mutex_unlock(&B.mtx);
        snprintf(resp, (size_t)rsz, "FAIL key-for-other-bd");
        return -1;
    }
    pthread_mutex_unlock(&B.mtx);

    if (persist_key(bd, B.bond_key, B.bond_key_type ? B.bond_key_type : 5,
                    B.bond_name[0] ? B.bond_name : NULL) != 0) {
        snprintf(resp, (size_t)rsz, "FAIL xml-persist");
        return -1;
    }
    snprintf(resp, (size_t)rsz,
             "OK %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
             B.bond_key[0], B.bond_key[1], B.bond_key[2], B.bond_key[3],
             B.bond_key[4], B.bond_key[5], B.bond_key[6], B.bond_key[7],
             B.bond_key[8], B.bond_key[9], B.bond_key[10], B.bond_key[11],
             B.bond_key[12], B.bond_key[13], B.bond_key[14], B.bond_key[15]);
    /* v2 semantics + wlmicd config adoption: a newly paired mic becomes
     * mic0 (mic.cfg bd=/bd0= rewritten hot, other keys preserved) */
    cfg_adopt_bd(bd);
    return 0;
}

/* cutover disconnect: sleep notice -> DgClose(handle from the live
 * bt-dg-socket<N> inode) -> TmDisconnect -> ACL probe */
static int dg_handle_by_bd(void)
{
    /* The per-connection DG socket name carries the handle; with one
     * TX there is exactly one bt-dg-socket<N>. */
    int i;
    for (i = 0; i < 20; i++) {
        char p[96];
        struct stat st;
        snprintf(p, sizeof p, "/data/misc/bluetooth/bt-dg-socket%d", i);
        if (stat(p, &st) == 0) return i;
    }
    return -1;
}

static int ctl_disconnect(char *resp, int rsz)
{
    unsigned char r[MAXPL];
    int n, handle;

    if (E.offline) { snprintf(resp, (size_t)rsz, "FAIL offline"); return -1; }
    if (E.compat) {
        snprintf(resp, (size_t)rsz, "FAIL compat-mode (hook engine owns the link)");
        return -1;
    }
    if (!E.bd_valid[0]) { snprintf(resp, (size_t)rsz, "OK nothing-configured"); return 0; }

    if (link_up_gate())
        announce_sleep_0x54_20();

    pthread_mutex_lock(&E.st_mtx);
    E.state = ST_IDLE;
    E.connected = 0;
    E.battery = -1;
    E.rssi = 0;
    E.charging = -1;
    E.mute = -1;
    E.recording = -1;
    E.ts_valid = 0;
    pthread_mutex_unlock(&E.st_mtx);

    handle = dg_handle_by_bd();
    if (handle >= 0) {
        unsigned char h2[2] = { (unsigned char)(handle & 0xff), (unsigned char)(handle >> 8) };
        n = bsa_sync(0x4B4, h2, 2, r, sizeof r, 4000);   /* BSA_DgClose */
        mlog("disconnect: DgClose(%d) resp %d", handle, n);
    }
    n = bsa_sync(0x583, E.bd[0], 6, r, sizeof r, 4000);  /* TmDisconnect */
    mlog("disconnect: TmDisconnect resp %d status=%u", n, n >= 2 ? rd16b(r) : 0xFFFF);
    n = bsa_sync(0x582, E.bd[0], 6, r, sizeof r, 1500);
    {
        int alive = (n >= 12 && rd16b(r) == 0);
        mlog("disconnect: ACL probe alive=%d", alive);
        if (alive) {
            unsigned char r2[MAXPL];
            bsa_sync(0x583, E.bd[0], 6, r2, sizeof r2, 4000);
        }
    }
    snprintf(resp, (size_t)rsz, "OK");
    return 0;
}

static int ctl_unpair(const uint8_t *bd, char *resp, int rsz)
{
    unsigned char p[7], r[MAXPL];
    int n, ours;
    memcpy(p, bd, 6);
    p[6] = 1;
    ours = E.bd_valid[0] && !memcmp(E.bd[0], bd, 6);
    if (ours && !E.offline && !E.compat)
        ctl_disconnect(resp, rsz);
    n = bsa_sync(0x134, p, 7, r, sizeof r, 4000);
    if (n < 2 || rd16b(r) != 0) {
        snprintf(resp, (size_t)rsz, "FAIL sec-remove-status=%u", n >= 2 ? rd16b(r) : 0xFFFF);
        return -1;
    }
    {
        int xr = xml_remove_device(bd);
        if (ours) {
            pthread_mutex_lock(&E.st_mtx);
            E.battery = -1;
            E.rssi = 0;
            E.name[0] = 0;
            E.charging = -1;
            E.mute = -1;
            E.recording = -1;
            pthread_mutex_unlock(&E.st_mtx);
            /* pairing gone: drop it from the mic.cfg slots too */
            if (E.bd_valid[1] && memcmp(E.bd[1], bd, 6) == 0)
                cfg_write_bds(E.bd[0], E.bd_valid[0] && memcmp(E.bd[0], bd, 6) != 0,
                              NULL, 0);
            else if (E.bd_valid[0] && memcmp(E.bd[0], bd, 6) == 0)
                cfg_write_bds(NULL, 0, E.bd[1], E.bd_valid[1]);
            read_cfg(1);
            update_paired();
            bt_audio_cfg_sync();
        }
        snprintf(resp, (size_t)rsz, xr == 0 ? "OK" : "OK already-absent");
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* control socket server                                               */
/* ------------------------------------------------------------------ */
static void ctl_handle_line(char *line, char *resp, int rsz)
{
    char *av[8];
    int an = 0;
    char *tok, *save = NULL;

    for (tok = strtok_r(line, " \t\r\n", &save); tok && an < 8;
         tok = strtok_r(NULL, " \t\r\n", &save))
        av[an++] = tok;
    if (an == 0) { snprintf(resp, (size_t)rsz, "ERR empty"); return; }

    if (!strcmp(av[0], "status")) {
        status_json(resp, rsz);
    } else if (!strcmp(av[0], "connect")) {
        /* no-injection constraint: SPP open lives inside bt_bsa_app and
         * the WMS cmd channel (90000) is source-verified to dji_network.
         * connect = ensure pairing, then ARM and wait for the TX's
         * native HFP->SPP dance (the product's own passive path). */
        if (E.offline) { snprintf(resp, (size_t)rsz, "FAIL offline"); return; }
        if (!E.bd_valid[0]) { snprintf(resp, (size_t)rsz, "FAIL no-bd"); return; }
        pthread_mutex_lock(&E.st_mtx);
        if (E.state == ST_STREAMING || E.state == ST_CONNECTING) {
            int st = E.state;
            pthread_mutex_unlock(&E.st_mtx);
            snprintf(resp, (size_t)rsz, st == ST_CONNECTING ? "OK already-arming" : "BUSY");
            return;
        }
        E.state = ST_CONNECTING;
        E.connect_ms = now_ms();
        pthread_mutex_unlock(&E.st_mtx);
        E.have_seq = 0;
        if (!xml_has_key(E.bd[0])) {
            char presp[160] = "";
            mlog("connect: no key -> auto-pair (TX pairing mode)");
            ctl_pair(E.bd[0], presp, sizeof presp);
            mlog("connect: auto-pair %s", presp);
            if (strncmp(presp, "OK", 2) != 0) {
                pthread_mutex_lock(&E.st_mtx);
                E.state = ST_IDLE;
                pthread_mutex_unlock(&E.st_mtx);
                snprintf(resp, (size_t)rsz, "FAIL pair %s", presp);
                return;
            }
        }
        if (E.linkup_refresh_sec > 0) send_conn_ok_pair();
        mlog("connect: ARMED (passive; TX dance opens SPP natively)");
        snprintf(resp, (size_t)rsz, "OK armed-passive");
    } else if (!strcmp(av[0], "disconnect")) {
        ctl_disconnect(resp, rsz);
    } else if (!strcmp(av[0], "scan")) {
        ctl_scan(an > 1 ? atoi(av[1]) : 10, resp, rsz);
    } else if (!strcmp(av[0], "pair")) {
        uint8_t bd[6];
        if (an < 2 || parse_bd_str(av[1], bd) < 0)
            snprintf(resp, (size_t)rsz, "FAIL bad-bd");
        else
            ctl_pair(bd, resp, rsz);
    } else if (!strcmp(av[0], "unpair")) {
        uint8_t bd[6];
        if (an < 2 || parse_bd_str(av[1], bd) < 0)
            snprintf(resp, (size_t)rsz, "FAIL bad-bd");
        else
            ctl_unpair(bd, resp, rsz);
    } else if (!strcmp(av[0], "rec")) {
        int rc = rec_start(an > 1 ? atoi(av[1]) : 0, an > 2 ? av[2] : NULL);
        if (rc == 0) {
            pthread_mutex_lock(&E.rec_mtx);
            snprintf(resp, (size_t)rsz, "OK %s", E.rec_path);
            pthread_mutex_unlock(&E.rec_mtx);
        } else {
            snprintf(resp, (size_t)rsz, "FAIL busy-or-open");
        }
    } else if (!strcmp(av[0], "stop")) {
        char path[256];
        uint64_t bytes = 0;
        if (rec_stop(path, sizeof path, &bytes) == 0)
            snprintf(resp, (size_t)rsz, "OK %s %llu", path, (unsigned long long)bytes);
        else
            snprintf(resp, (size_t)rsz, "FAIL not-recording");
    } else if (!strcmp(av[0], "kickstart")) {
        if (E.offline) { snprintf(resp, (size_t)rsz, "FAIL offline"); return; }
        if (E.compat) { snprintf(resp, (size_t)rsz, "FAIL compat-mode"); return; }
        kickstart_ladder();
        snprintf(resp, (size_t)rsz, "OK");
    } else if (!strcmp(av[0], "poll57")) {          /* extension */
        poll_0x57_tick();
        snprintf(resp, (size_t)rsz, "OK");
    } else if (!strcmp(av[0], "reload")) {          /* extension */
        read_cfg(1);
        update_paired();
        bt_audio_cfg_sync();
        snprintf(resp, (size_t)rsz, "OK compat=%d poll57=%d ka055=%d",
                 E.compat, E.poll_057, E.keepalive_055);
    } else if (!strcmp(av[0], "mode")) {            /* extension */
        snprintf(resp, (size_t)rsz, "OK %s offline=%d bd0=%d bd1=%d",
                 E.compat ? "compat" : "cutover", E.offline,
                 E.bd_valid[0], E.bd_valid[1]);
    } else if (!strcmp(av[0], "help")) {
        snprintf(resp, (size_t)rsz,
                 "status|connect|disconnect|scan <sec>|pair <bd>|unpair <bd>|"
                 "rec <sec> [path]|stop|kickstart|poll57|reload|mode|quit");
    } else {
        snprintf(resp, (size_t)rsz, "ERR unknown-cmd");
    }
}

static void *ctl_conn_thread(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char line[1024], resp[4096];
    FILE *f = fdopen(fd, "r+");

    if (!f) { close(fd); return NULL; }
    setvbuf(f, NULL, _IONBF, 0);
    while (fgets(line, sizeof line, f)) {
        if (!strncasecmp(line, "quit", 4)) break;
        ctl_handle_line(line, resp, sizeof resp);
        fprintf(f, "%s\n", resp);
        fflush(f);
    }
    fclose(f);
    return NULL;
}

static void *ctl_accept_thread(void *arg)
{
    int lfd = (int)(intptr_t)arg;
    (void)arg;
    for (;;) {
        int cfd = accept(lfd, NULL, NULL);
        pthread_t t;
        pthread_attr_t a;
        if (cfd < 0) {
            if (errno == EINTR) continue;
            mlog("ctl accept: %s", strerror(errno));
            break;
        }
        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        pthread_create(&t, &a, ctl_conn_thread, (void *)(intptr_t)cfd);
        pthread_attr_destroy(&a);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* housekeeper (1 Hz): cfg hot-reload, state machine, telemetry,       */
/* periodic control legs (0x55 keepalive, 0x57 poll, conn-OK refresh)  */
/* ------------------------------------------------------------------ */
static uint64_t cfg_mtime(void)
{
    struct stat st;
    if (stat(CFG_PATH, &st) != 0) return 0;
    return (uint64_t)st.st_mtim.tv_sec * 1000000000ull + (uint64_t)st.st_mtim.tv_nsec;
}

static void *hk_thread(void *arg)
{
    static uint64_t next_057, next_linkup;
    (void)arg;
    for (;;) {
        uint64_t now = now_ms();

        /* cfg hot reload (mtime watch -- only we write it, but the user
         * or micctl extensions may; apply without restart) */
        if (!E.offline) {
            uint64_t m = cfg_mtime();
            if (m != E.cfg_mtime_ns && m != 0) {
                int old_compat = E.compat;
                E.cfg_mtime_ns = m;
                cfg_set_defaults_for_mode();
                read_cfg(1);
                update_paired();
                bt_audio_cfg_sync();
                mlog("cfg hot-reload: compat=%d poll57=%ds ka055=%d responder=%d%s",
                     E.compat, E.poll_057_sec, E.keepalive_055, E.responder,
                     old_compat != E.compat ? " (mode changed)" : "");
            }
        }

        if (!E.offline)
            update_paired();

        /* FIFO (re)open attempt */
        if (E.fifo_enabled && E.pcm_fd < 0 && E.p_fifo[0])
            E.pcm_fd = open(E.p_fifo, O_WRONLY | O_NONBLOCK);

        now = now_ms();

        /* connected decay: no stream data for 5s -> not connected */
        pthread_mutex_lock(&E.st_mtx);
        if (E.connected && E.last_audio_ms && now - E.last_audio_ms > 5000)
            E.connected = 0;

        /* armed connect that never completes (TX asleep) */
        if (E.state == ST_CONNECTING && E.connect_ms &&
            now > E.connect_ms && now - E.connect_ms > 600000) {
            E.state = ST_IDLE;
            mlog("armed connect incomplete after 600s -> idle (TX asleep?)");
        }

        /* stall -> reconnecting (silence padding on the 5ms grid) */
        if (E.state == ST_STREAMING && E.last_audio_ms &&
            now - E.last_audio_ms > 5000) {
            E.state = ST_RECONNECTING;
            mlog("stream stall -> reconnecting");
        }
        if (E.state == ST_RECONNECTING) {
            if (E.last_audio_ms && now - E.last_audio_ms < 3000 &&
                now - E.last_pad_ms >= FRAME_MS) {
                int frames = (int)((now - E.last_pad_ms) / FRAME_MS);
                if (frames > 200) frames = 200;
                if (frames > 0) {
                    pcm_fill(frames);
                    E.last_pad_ms += (uint64_t)frames * FRAME_MS;
                }
            }
            if (E.last_audio_ms && now - E.last_audio_ms > 60000)
                E.state = ST_IDLE;
        }
        pthread_mutex_unlock(&E.st_mtx);

        /* rec deadline */
        if (E.rec_deadline_ms && now >= E.rec_deadline_ms) {
            char path[256];
            uint64_t bytes = 0;
            if (rec_stop(path, sizeof path, &bytes) == 0)
                mlog("rec auto-stop: %s %llu bytes", path, (unsigned long long)bytes);
        }

        /* RSSI telemetry: 1 Hz TmReadRawRssi poll while connected (the
         * engine's BSA_SEC_RSSI_EVT equivalent; feeds status + the
         * 0x55 quality byte) */
        if (!E.offline && E.rssi_poll && E.connected && E.bd_valid[0]) {
            unsigned char r[MAXPL];
            int rn = bsa_sync(0x582, E.bd[0], 6, r, sizeof r, 1500);
            if (rn >= 12 && rd16b(r) == 0)
                E.rssi = (int)(int8_t)r[10];
            ka_055_tick();
        }

        /* 0x57 status exchange cadence */
        if (!E.offline && E.poll_057 && E.bd_valid[0] &&
            (link_up_gate() || E.state == ST_CONNECTING)) {
            if (now >= next_057) {
                poll_0x57_tick();
                next_057 = now + (uint64_t)E.poll_057_sec * 1000;
            }
        }

        /* periodic conn-OK refresh while streaming (cutover safety net:
         * replays what dji_network sent once per connection; also fires
         * once when a stalled stream is RECONNECTING) */
        if (!E.offline && E.linkup_refresh_sec > 0 && E.bd_valid[0] &&
            E.state == ST_STREAMING && now >= next_linkup) {
            send_conn_ok_pair();
            next_linkup = now + (uint64_t)E.linkup_refresh_sec * 1000;
        }

        status_write();
        sleep(1);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* offline self-test helpers                                           */
/* ------------------------------------------------------------------ */
static void synth_status_block(uint8_t *out119, int battery, int charging,
                               int mute, int recording)
{
    int i, o;
    unsigned s = 0;
    memset(out119, 0xff, 119);
    out119[0] = 0x55; out119[1] = 0x77; out119[2] = 0x05;
    out119[4] = 0x19;              /* cmd: status */
    out119[5] = 0x00;              /* r */
    out119[6] = 0x01;              /* seq */
    o = 7;
    out119[o++] = 0x01; out119[o++] = 0x01; out119[o++] = (uint8_t)(mute ? 1 : 0);
    out119[o++] = 0x05; out119[o++] = 0x01; out119[o++] = (uint8_t)(charging ? 1 : 0);
    out119[o++] = 0x06; out119[o++] = 0x01; out119[o++] = (uint8_t)(recording ? 1 : 0);
    out119[o++] = 0x10; out119[o++] = 0x01; out119[o++] = (uint8_t)battery;
    for (i = 4; i <= 117; i++) s += out119[i];
    out119[3] = (uint8_t)(s & 0x3f);
}

static void feed_synth_status_dgram(void)
{
    uint8_t unit[125], dgram[4 + 6 + 125];
    if (!E.bd_valid[0]) return;
    memset(unit, 0, 6);                       /* clock hdr */
    synth_status_block(unit + 6, 82, 1, 0, 1);
    dgram[0] = 0x01; dgram[1] = 0x42;
    dgram[2] = 0; dgram[3] = (uint8_t)(10 + 125);
    memcpy(dgram + 4, E.bd[0], 6);
    memcpy(dgram + 10, unit, 125);
    mlog("tlv-synth: injecting Wl 0x19 status block {battery=82 charging=1 mute=0 recording=1}");
    dgram_input(dgram, 135);
}

static int probe_templates(void)
{
    uint8_t duss[96], pl[22];
    int dl, fails = 0;
    char hx[3 * 96 + 1];
    int i, o = 0;

    /* REPORT_ctrl_frames section 4, seq=1, bd=71:34:0B:AF:6F:41,
     * btclk=0x0001A2B3, intra=200, systime=0x000000E90F120000 */
    const uint8_t bd[6] = {0x71, 0x34, 0x0b, 0xaf, 0x6f, 0x41};

    wlmicd_duss_seq = 0x0001;
    build_0x57_payload(pl, bd, 0x0001A2B3, 200, 0x000000E90F120000ull);
    dl = duss_build_r(duss, 0x57, pl, 22, 0x18);
    for (i = 0; i < dl; i++) o += snprintf(hx + o, sizeof hx - o, "%02x", duss[i]);
    printf("probe57 %s\n", hx);
    if (strcmp(hx,
        "5523042e07180100401257"
        "00b3a20100c8000000120fe9000000"
        "0171340baf6f41"
        "2a99") != 0) {
        printf("probe57 MISMATCH (expected 5523042e0718010040125700b3a20100c800"
               "0000120fe90000000171340baf6f412a99)\n");
        fails++;
    }

    wlmicd_duss_seq = 0x0001;
    {
        uint8_t p55[7];
        p55[0] = 0x2b;                          /* -57 dBm */
        memcpy(p55 + 1, bd, 6);
        dl = duss_build_r(duss, 0x55, p55, 7, 0x18);
    }
    o = 0;
    for (i = 0; i < dl; i++) o += snprintf(hx + o, sizeof hx - o, "%02x", duss[i]);
    printf("probe55 %s\n", hx);
    if (strcmp(hx, "5514046d071801004012552b71340baf6f41cb49") != 0) {
        printf("probe55 MISMATCH (expected 5514046d071801004012552b71340baf6f41cb49)\n");
        fails++;
    }
    return fails;
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */
static int mic_ctl_alive(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a;
    char line[128], resp[2048];
    FILE *f;
    int alive = 0;
    if (fd < 0) return 0;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, MIC_DIR "/mic_ctl", sizeof a.sun_path - 1);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0) {
        f = fdopen(fd, "r+");
        if (f) {
            setvbuf(f, NULL, _IONBF, 0);
            fprintf(f, "status\n");
            if (fgets(resp, sizeof resp, f)) alive = resp[0] == '{';
            fprintf(f, "quit\n");
            fclose(f);
            close(fd);
            return alive;
        }
    }
    close(fd);
    (void)line;
    return 0;
}

static int engine_start(void)
{
    pthread_mutex_lock(&E.st_mtx);
    if (E.started) { pthread_mutex_unlock(&E.st_mtx); return 0; }
    E.started = 1;
    pthread_mutex_unlock(&E.st_mtx);

    signal(SIGPIPE, SIG_IGN);
    B.fd = -1;
    E.battery = E.charging = E.mute = E.recording = -1;
    E.pcm_fd = -1;

    /* compat auto-detect: the stage-1 hook engine owns the canonical
     * faces while its ctl socket answers */
    if (!E.offline)
        E.compat = mic_ctl_alive();

    cfg_set_defaults_for_mode();
    read_cfg(1);
    E.cfg_mtime_ns = cfg_mtime();

    if (E.offline) {
        snprintf(E.p_status, sizeof E.p_status, "wlmicd_offline_status.json");
        E.p_ctl[0] = 0;
        E.p_fifo[0] = 0;
        E.fifo_enabled = 0;
        E.p_log[0] = 0;                 /* log to stderr only */
        /* offline replay needs a source bd to route dgrams through the
         * configured-slot check; cfg (if present) overrides */
        if (!E.bd_valid[0]) {
            static const uint8_t cap_bd[6] = {0xe4, 0x7a, 0x2c, 0xf2, 0x53, 0x69};
            memcpy(E.bd[0], cap_bd, 6);
            E.bd_valid[0] = 1;
        }
    } else if (E.compat) {
        snprintf(E.p_status, sizeof E.p_status, MIC_DIR "/wlmicd_status.json");
        snprintf(E.p_ctl, sizeof E.p_ctl, MIC_DIR "/wlmicd_ctl");
        snprintf(E.p_fifo, sizeof E.p_fifo, MIC_DIR "/wlmicd.pcm");
        snprintf(E.p_log, sizeof E.p_log, MIC_DIR "/wlmicd.log");
        E.fifo_enabled = 0;             /* hook engine feeds mic.pcm */
    } else {
        snprintf(E.p_status, sizeof E.p_status, MIC_DIR "/mic_status.json");
        snprintf(E.p_ctl, sizeof E.p_ctl, MIC_DIR "/mic_ctl");
        snprintf(E.p_fifo, sizeof E.p_fifo, MIC_DIR "/mic.pcm");
        snprintf(E.p_log, sizeof E.p_log, MIC_DIR "/wlmicd.log");
        E.fifo_enabled = 1;
        mkfifo(E.p_fifo, 0666);
        chmod(E.p_fifo, 0666);
    }

    mlog("wlmicd start: offline=%d compat=%d bd0=%d bd1=%d "
         "poll57=%d/%ds ka055=%d responder=%d linkup_refresh=%ds",
         E.offline, E.compat, E.bd_valid[0], E.bd_valid[1],
         E.poll_057, E.poll_057_sec, E.keepalive_055, E.responder,
         E.linkup_refresh_sec);

    if (E.bd_valid[0])
        lookup_name(E.bd[0], E.name, sizeof E.name);
    if (!E.offline)
        update_paired();

    if (decoder_init() != 0)
        mlog("start: decoder unavailable (engine runs, no audio decode)");

    if (!E.offline) {
        bt_audio_cfg_sync();

        if (up_bind() != 0) {
            mlog("start: FATAL - cannot bind %s (existing consumer?)", DSOCK_UP);
            return -1;
        }
        E.rx_running = 1;
        pthread_create(&E.rx_thread, NULL, rx_thread, NULL);

        if (E.p_ctl[0]) {
            int fd;
            struct sockaddr_un a;
            unlink(E.p_ctl);
            fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd < 0) {
                mlog("ctl socket: %s", strerror(errno));
                return -1;
            }
            memset(&a, 0, sizeof a);
            a.sun_family = AF_UNIX;
            strncpy(a.sun_path, E.p_ctl, sizeof a.sun_path - 1);
            if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, 4) < 0) {
                mlog("ctl bind/listen %s: %s", E.p_ctl, strerror(errno));
                close(fd);
                return -1;
            }
            chmod(E.p_ctl, 0666);
            E.ctl_running = 1;
            pthread_create(&E.ctl_thread, NULL, ctl_accept_thread, (void *)(intptr_t)fd);
        }
    }

    pthread_create(&E.hk_thread, NULL, hk_thread, NULL);
    E.rq_running = 1;
    pthread_create(&E.rq_thread, NULL, rq_thread, NULL);

    status_write();
    return 0;
}

/* ------------------------------------------------------------------ */
/* offline replay (main)                                               */
/* ------------------------------------------------------------------ */
static int replay_file(const char *path, int expect_frames, int tlv_synth, int wav)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long sz;
    long pos = 0;
    uint64_t dgrams = 0;

    if (!f) {
        fprintf(stderr, "replay: cannot open %s: %s\n", path, strerror(errno));
        return 2;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "replay: read failed\n");
        return 2;
    }
    fclose(f);

    if (wav) rec_start(0, NULL);

    while (pos + 10 <= sz) {
        unsigned total = ((unsigned)buf[pos + 2] << 8) | buf[pos + 3];
        if (buf[pos] != 0x01 || total < 11 || (long)pos + (long)total > sz) {
            fprintf(stderr, "replay: bad dgram at %ld (t=%u)\n", pos, total);
            break;
        }
        dgram_input(buf + pos, (int)total);
        pos += (long)total;
        dgrams++;
    }
    printf("replay: %llu dgrams / %ld bytes consumed\n",
           (unsigned long long)dgrams, pos);

    if (tlv_synth)
        feed_synth_status_dgram();

    usleep(300 * 1000);              /* let the RQ worker flush + logs land */

    {
        char sj[1024];
        status_json(sj, sizeof sj);
        printf("status %s\n", sj);
    }
    if (wav) {
        char path_out[256];
        uint64_t bytes = 0;
        if (rec_stop(path_out, sizeof path_out, &bytes) == 0)
            printf("wav %s %llu\n", path_out, (unsigned long long)bytes);
    }
    printf("counters frames=%llu drops=%llu dgrams=%llu other_bd=%llu\n",
           (unsigned long long)E.frames, (unsigned long long)E.drops,
           (unsigned long long)E.dgrams, (unsigned long long)E.dgrams_other_bd);

    {
        int fails = 0;
        if (expect_frames > 0 && (long long)E.frames < expect_frames) {
            printf("FAIL frames %llu < expected %d\n",
                   (unsigned long long)E.frames, expect_frames);
            fails++;
        }
        if (tlv_synth) {
            int b, c, m, rci;
            pthread_mutex_lock(&E.st_mtx);
            b = E.battery; c = E.charging; m = E.mute; rci = E.recording;
            pthread_mutex_unlock(&E.st_mtx);
            if (b != 82 || c != 1 || m != 0 || rci != 1) {
                printf("FAIL tlv battery=%d charging=%d mute=%d recording=%d "
                       "(expect 82/1/0/1)\n", b, c, m, rci);
                fails++;
            } else {
                printf("tlv-ok battery=%d charging=%d mute=%d recording=%d\n",
                       b, c, m, rci);
            }
        }
        return fails == 0 ? 0 : 1;
    }
}

int main(int argc, char **argv)
{
    const char *replay = NULL;
    int expect_frames = 0, tlv_synth = 0, wav = 0, probe = 0, daemonize = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--replay") && i + 1 < argc) replay = argv[++i];
        else if (!strcmp(argv[i], "--expect-frames") && i + 1 < argc) expect_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--tlv-synth")) tlv_synth = 1;
        else if (!strcmp(argv[i], "--wav")) wav = 1;
        else if (!strcmp(argv[i], "--probe57") || !strcmp(argv[i], "--probe55")) probe = 1;
        else if (!strcmp(argv[i], "-d") || !strcmp(argv[i], "--daemon")) daemonize = 1;
        else if (!strcmp(argv[i], "-v")) log_stderr = 1;
        else { fprintf(stderr, "usage: wlmicd [-d] [-v] | --replay <dgram.bin> [--expect-frames N] [--tlv-synth] [--wav] [--probe57]\n"); return 2; }
    }

    if (probe)
        return probe_templates();

    E.offline = replay != NULL || getenv("WLMICD_OFFLINE") != NULL;
    log_stderr = E.offline ? 1 : log_stderr;

    /* daemonize BEFORE engine_start: fork() must happen while this
     * process is still single-threaded (the engine spawns 4 threads) */
    if (daemonize && !E.offline) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return 4; }
        if (pid > 0) { printf("wlmicd daemon pid %ld\n", (long)pid); return 0; }
        setsid();
        {
            int nfd = open("/dev/null", O_RDONLY);
            if (nfd >= 0) {
                dup2(nfd, 0);
                dup2(nfd, 1);      /* release the caller's pipes (adb) */
                dup2(nfd, 2);
                if (nfd > 2) close(nfd);
            }
        }
    }

    if (engine_start() != 0)
        return 3;

    if (replay)
        return replay_file(replay, expect_frames, tlv_synth, wav);

    mlog("wlmicd running (pid %ld)", (long)getpid());
    for (;;) pause();
    return 0;
}
