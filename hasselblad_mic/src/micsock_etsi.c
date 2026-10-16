/*
 * micsock_etsi.c — same as micsock but decoding via open-source ETSI LC3plus (PLC enabled) for the Hasselblad camera.
 * One process owns the whole chain:
 *   bsa_server control socket -> DgEnable -> DgOpen(TX) -> DG_OPEN_EVT
 *   -> connect bt-dg-socket<handle> -> parse Wl blocks -> LC3plus decode -> WAV
 * Requires bt_bsa_app to be dead (DG subsystem ownership) — run as:
 *   kill -9 $(ps -A | grep bt_bsa_app ...); micsock <seconds> <out.wav>
 * usage: micsock [seconds=1800] [out.wav=/data/usbmic/mic.wav] [bd_hex=e47a...]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>

#define BT_SOCK "/data/misc/bluetooth/bt-daemon-socket"
#define DG_DIR  "/data/misc/bluetooth/"

typedef int (*f_get_size)(int, int);
typedef int (*f_init)(void*, int, int, int, int);
typedef int (*f_ms)(void*, int);
typedef int (*f_ns)(void*);
typedef int (*f_dec16)(void*, unsigned char*, int, short*, void*, int);
static f_get_size lc3plus_dec_get_size;
static f_init lc3plus_dec_init;
static f_ms lc3plus_dec_set_frame_dms;
static f_ns lc3plus_dec_get_output_samples;
static f_dec16 lc3plus_dec16;

void __assert_fail(const char *e, const char *f, unsigned l, const char *fn)
{ fprintf(stderr, "liblc3plus assert: %s:%u %s: %s\n", f, l, fn?fn:"", e); abort(); }
void __gmon_start__(void) {}
void _ITM_deregisterTMCloneTable(void) {}
void _ITM_registerTMCloneTable(void) {}

static volatile sig_atomic_t stop = 0;
static void on_sig(int s) { (void)s; stop = 1; }

static int ctl = -1;   /* control socket to bsa_server */

static int bsa_send(unsigned id, const void *payload, int plen)
{
    unsigned char hdr[4];
    int len = plen + 2;
    hdr[0] = len & 0xff; hdr[1] = (len >> 8) & 0xff;
    hdr[2] = id & 0xff;  hdr[3] = (id >> 8) & 0xff;
    if (write(ctl, hdr, 4) != 4) return -1;
    if (plen > 0 && write(ctl, payload, plen) != plen) return -1;
    return 0;
}

/* read exactly one message; returns plen, fills id; -1 on error/timeout */
static int bsa_recv(unsigned *id, unsigned char *buf, int max, int timeout_ms)
{
    unsigned char hdr[4];
    int got = 0;
    while (got < 4) {
        fd_set rf; struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
        FD_ZERO(&rf); FD_SET(ctl, &rf);
        if (select(ctl + 1, &rf, NULL, NULL, &tv) <= 0) return -1;
        int r = read(ctl, hdr + got, 4 - got);
        if (r <= 0) return -1;
        got += r;
    }
    int len = hdr[0] | (hdr[1] << 8);
    *id = hdr[2] | (hdr[3] << 8);
    int plen = len - 2;
    if (plen < 0 || plen > max) return -1;
    got = 0;
    while (got < plen) {
        int r = read(ctl, buf + got, plen - got);
        if (r <= 0) return -1;
        got += r;
    }
    return plen;
}

static void ts(const char *tag)
{
    char b[16]; time_t t = time(NULL); struct tm tmv;
    localtime_r(&t, &tmv); strftime(b, sizeof b, "%H:%M:%S", &tmv);
    printf("[%s] %s\n", b, tag); fflush(stdout);
}

static int16_t *pcm_out = NULL;
static int pcm_cap = 0;
static long pcm_len = 0;
static FILE *wav = NULL;
static long data_bytes = 0;

static void pcm_push(const int16_t *src, int n)
{
    fwrite(src, 2, n, wav);
    data_bytes += n * 2;
    pcm_len += n;
    (void)pcm_out; (void)pcm_cap;
}

int main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 1800;
    const char *outpath = argc > 2 ? argv[2] : "/data/usbmic/mic.wav";
    const char *bdhex = argc > 3 ? argv[3] : "e47a2cf25369";
    unsigned char bd[6];
    for (int i = 0; i < 6; i++) sscanf(bdhex + 2 * i, "%2hhx", &bd[i]);

    /* ---- 1. control connection ---- */
    ctl = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, BT_SOCK, sizeof a.sun_path - 1);
    if (connect(ctl, (struct sockaddr *)&a, sizeof a) < 0) {
        perror("connect " BT_SOCK); return 1;
    }
    ts("connected to bsa_server");

    /* ---- 2. DG enable + open ---- */
    unsigned char p64[64]; unsigned char z8[8] = {0};
    unsigned id; unsigned char buf[2200];
    bsa_send(0x4B0, z8, 8);                       /* DgEnable (take DG slot) */
    int n = bsa_recv(&id, buf, sizeof buf, 3000);
    printf("DgEnable resp id=0x%x status=%d\n", id, (n >= 1) ? buf[0] : -1);
    memset(p64, 0, sizeof p64);
    memcpy(p64, bd, 6);
    p64[6] = 1;                                    /* service = SPP */
    bsa_send(0x4B3, p64, 64);                      /* DgOpen */
    int handle = -1;
    time_t twait = time(NULL) + 30;
    while (time(NULL) < twait && handle < 0) {
        n = bsa_recv(&id, buf, sizeof buf, 2000);
        if (n < 0) continue;
        if ((id & 0xFFF) == 0x4B7 && n > 0x0D && buf[0] == 0) {
            handle = buf[0xC] | (buf[0xD] << 8);
            printf("DG_OPEN_EVT OK handle=%d\n", handle);
        } else if ((id & 0xFFF) == 0x4B7) {
            printf("DG_OPEN_EVT status=%d\n", n > 0 ? buf[0] : -1);
        }
    }
    if (handle < 0) { ts("no DG_OPEN within 30s"); return 1; }

    /* ---- 3. connect data socket ---- */
    char dgpath[128];
    snprintf(dgpath, sizeof dgpath, "%sbt-dg-socket%d", DG_DIR, handle - 11 >= 0 ? handle - 11 : handle);
    int ds = socket(AF_UNIX, SOCK_STREAM, 0);
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, dgpath, sizeof a.sun_path - 1);
    if (connect(ds, (struct sockaddr *)&a, sizeof a) < 0) {
        perror(dgpath); return 1;
    }
    char tag[96]; snprintf(tag, sizeof tag, "SPP data socket %s connected, recording", dgpath);
    ts(tag);

    /* ---- 4. decoder ---- */
    void *h = dlopen("/data/mic/libLC3plus_bionic.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    #define RS(var, name) do { *(void**)(&(var)) = dlsym(h, name); \
        if (!(var)) { fprintf(stderr, "dlsym %s\n", name); return 1; } } while (0)
    RS(lc3plus_dec_get_size, "lc3plus_dec_get_size"); RS(lc3plus_dec_init, "lc3plus_dec_init");
    RS(lc3plus_dec_set_frame_dms, "lc3plus_dec_set_frame_dms");
    RS(lc3plus_dec_get_output_samples, "lc3plus_dec_get_output_samples"); RS(lc3plus_dec16, "lc3plus_dec16");
    #undef RS
    int need = lc3plus_dec_get_size(48000, 1);
    void *dec = calloc(1, need);
    if (lc3plus_dec_init(dec, 48000, 1, 1, 0)  /* PLC_ADVANCED=1 */) { fprintf(stderr, "dec_init fail\n"); return 1; }
    if (lc3plus_dec_set_frame_dms(dec, 4)  /* 5MS=4 */) { fprintf(stderr, "dms fail\n"); return 1; }
    int ns = lc3plus_dec_get_output_samples(dec);
    printf("decoder ready: %d samples/frame\n", ns);

    /* ---- 5. WAV ---- */
    wav = fopen(outpath, "wb");
    if (!wav) { perror(outpath); return 1; }
    unsigned char whdr[44];
    memset(whdr, 0, 44);
    memcpy(whdr, "RIFF", 4); memcpy(whdr + 8, "WAVEfmt ", 8);
    *(uint32_t*)(whdr + 16) = 16; *(uint16_t*)(whdr + 20) = 1; *(uint16_t*)(whdr + 22) = 1;
    *(uint32_t*)(whdr + 24) = 48000; *(uint32_t*)(whdr + 28) = 96000;
    *(uint16_t*)(whdr + 32) = 2; *(uint16_t*)(whdr + 34) = 16;
    memcpy(whdr + 36, "data", 4);
    fwrite(whdr, 1, 44, wav);

    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);

    /* ---- 6. stream parse + decode ---- */
    static uint8_t sbuf[1 << 20];
    int slen = 0;
    unsigned char rbuf[65536];
    int16_t pcm[4096];
    static unsigned char framebuf[512];
    long blocks = 0, audio = 0, bad = 0, errs = 0;
    int have_seq = 0; uint8_t last_seq = 0;
    time_t t0 = time(NULL), last_rep = 0;

    while (!stop && time(NULL) - t0 < seconds) {
        struct timeval tv = { 1, 0 };
        fd_set rf; FD_ZERO(&rf); FD_SET(ds, &rf);
        if (select(ds + 1, &rf, NULL, NULL, &tv) <= 0) {
            long el = time(NULL) - t0;
            if (el - last_rep >= 15) { ts("stream idle..."); last_rep = el; }
            continue;
        }
        int r = read(ds, rbuf, sizeof rbuf);
        if (r <= 0) { ts("data socket closed"); break; }
        if (slen + r > (int)sizeof sbuf) slen = 0;
        memcpy(sbuf + slen, rbuf, r);
        slen += r;

        long pos = 0;
        while (pos + 12 <= slen) {
            uint8_t *p = memchr(sbuf + pos, 0x55, slen - pos - 11);
            if (!p) { pos = slen; break; }
            long off = p - sbuf;
            if (p[1] != 0x77 || p[2] != 0x05) { pos = off + 1; continue; }
            int len = p[1];
            if (off + len > slen) break;
            unsigned s = 0;
            for (int i = 4; i <= len - 2; i++) s += p[i];
            if ((s & 0x3f) != (p[3] & 0x3f)) { bad++; pos = off + 1; continue; }
            blocks++; pos = off + len;
            if (p[4] != 0x10) continue;
            audio++;
            uint8_t seq = p[6];
            int plen = len - 8;
            if (have_seq) {
                int dd = (seq - last_seq) & 0xff;
                if (dd == 0 || dd >= 200) continue;
                if (dd > 1) {
                    /* PLC concealment: feed erased frames to the decoder */
                    for (int g = 0; g < dd - 1; g++) {
                        memset(pcm, 0, sizeof pcm);
                        if (lc3plus_dec16(dec, NULL, 0, (short*[]){pcm}, NULL, 0)) {
                            static const int16_t z[4096] = {0};
                            int want = ns;
                            while (want > 0) { int k = want > 4096 ? 4096 : want;
                                fwrite(z, 2, k, wav); data_bytes += k * 2; pcm_len += k; want -= k; }
                        } else { pcm_push(pcm, ns); }
                    }
                }
            }
            have_seq = 1; last_seq = seq;
            memset(pcm, 0, sizeof pcm);
            memcpy(framebuf, p + 7, plen);
            { int16_t *outp = pcm;
              int derr = lc3plus_dec16(dec, framebuf, plen, &outp, NULL, 0);
              if (derr) { if (errs < 3) fprintf(stderr, "dec16 err=%d plen=%d\n", derr, plen); errs++; continue; } }
            pcm_push(pcm, ns);
        }
        if (pos > 0) { memmove(sbuf, sbuf + pos, slen - pos); slen -= pos; }

        long el = time(NULL) - t0;
        if (el - last_rep >= 10) {
            char b2[16]; time_t t = time(NULL); struct tm tmv;
            localtime_r(&t, &tmv); strftime(b2, sizeof b2, "%H:%M:%S", &tmv);
            printf("[%s] %lds blocks=%ld audio=%ld bad=%ld errs=%ld wav=%.1fMB (%.0fs)\n",
                   b2, el, blocks, audio, bad, errs, data_bytes / 1e6, pcm_len / 48000.0);
            fflush(stdout);
            last_rep = el;
        }
    }

    fflush(wav);
    fseek(wav, 0, SEEK_SET);
    *(uint32_t*)(whdr + 4) = 36 + (uint32_t)data_bytes;
    *(uint32_t*)(whdr + 40) = (uint32_t)data_bytes;
    fwrite(whdr, 1, 44, wav);
    fclose(wav);
    printf("done: blocks=%ld audio=%ld bad=%ld errs=%ld -> %s (%.1fs)\n",
           blocks, audio, bad, errs, outpath, pcm_len / 48000.0);
    return 0;
}
