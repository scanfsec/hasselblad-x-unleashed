/* Per-TX decoder ownership. The caller serializes each instance; closing one
 * instance cannot invalidate another's library reference, state or PCM buffer.
 * Included after engine constants and mlog(). */
#ifndef MIC_DECODER_H
#define MIC_DECODER_H
typedef int (*fe_get_size)(int, int);
typedef int (*fe_init)(void *, int, int, int, int);
typedef int (*fe_dms)(void *, int);
typedef int (*fe_ns)(void *);
typedef int (*fe_free)(void *);
typedef int (*fe_dec16)(void *, unsigned char *, int, int16_t **, void *, int);
typedef int (*f_get_size)(int, int);
typedef int (*f_init)(void *, int, int, int);
typedef int (*f_hr)(void *, int);
typedef int (*f_ms)(void *, float);
typedef int (*f_ns)(void *);
typedef int (*f_dec16)(void *, const void *, int, void **);
struct mic_decoder {
    void *context;
    void *library;
    int etsi;                  /* 1 = ETSI API in use */
    int initialized;
    int samples;
    int16_t pcm[NSAMP];
    uint8_t input[128];         /* dec16 may write into its input */
    fe_get_size e_get_size;
    fe_init e_init;
    fe_dms e_dms;
    fe_ns e_ns;
    fe_dec16 e_dec16;
    fe_free e_free;
    f_get_size lc3_dec_get_size;
    f_init lc3_dec_init;
    f_hr lc3_dec_set_hrmode;
    f_ms lc3_dec_set_frame_ms;
    f_ns lc3_dec_get_output_samples;
    f_dec16 lc3_dec16;
    void *iov[2];
};

/* Caller owns feed_mtx or has not enabled input yet. */
static void decoder_close(struct mic_decoder *decoder)
{
    if (decoder->context && decoder->initialized && decoder->e_free)
        decoder->e_free(decoder->context); /* FFT/DCT allocations are not in the context block */
    free(decoder->context);
    decoder->context = NULL;
    if (decoder->library) dlclose(decoder->library);
    decoder->library = NULL;
    decoder->etsi = 0;
    decoder->initialized = 0;
    decoder->e_free = NULL;
    decoder->samples = NSAMP;
    decoder->e_get_size = NULL; decoder->e_init = NULL; decoder->e_dms = NULL; decoder->e_ns = NULL; decoder->e_dec16 = NULL;
    decoder->lc3_dec_get_size = NULL; decoder->lc3_dec_init = NULL; decoder->lc3_dec_set_hrmode = NULL;
    decoder->lc3_dec_set_frame_ms = NULL; decoder->lc3_dec_get_output_samples = NULL; decoder->lc3_dec16 = NULL;
    decoder->iov[0] = decoder->iov[1] = NULL;
}

static int decoder_init_etsi(struct mic_decoder *decoder, const char *library)
{
    void *h = dlopen(library, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        mlog("decoder_init: ETSI dlopen: %s", dlerror());
        return -1;
    }
    decoder->library = h;
#define RS(var, name) do { *(void **)(&(var)) = dlsym(h, name); \
        if (!(var)) { mlog("decoder_init: dlsym %s failed", name); goto failed; } } while (0)
    RS(decoder->e_get_size, "lc3plus_dec_get_size");
    RS(decoder->e_init, "lc3plus_dec_init");
    RS(decoder->e_dms, "lc3plus_dec_set_frame_dms");
    RS(decoder->e_ns, "lc3plus_dec_get_output_samples");
    RS(decoder->e_dec16, "lc3plus_dec16");
    RS(decoder->e_free, "lc3plus_dec_free_memory");
#undef RS
    int need = decoder->e_get_size(SRATE, 1);
    if (need <= 0) goto failed;
    decoder->context = calloc(1, (size_t)need);
    if (!decoder->context) goto failed;
    if (decoder->e_init(decoder->context, SRATE, 1, 1 /* PLC_ADVANCED */, 0 /* hrmode */)) {
        mlog("decoder_init: ETSI dec_init failed");
        goto failed;
    }
    decoder->initialized = 1;
    if (decoder->e_dms(decoder->context, 4 /* 5MS */)) {
        mlog("decoder_init: ETSI set_frame_dms(4) failed");
        goto failed;
    }
    decoder->samples = decoder->e_ns(decoder->context);
    if (decoder->samples <= 0 || decoder->samples > NSAMP) decoder->samples = NSAMP;
    decoder->etsi = 1;
    mlog("decoder_init: ETSI ok (%d samples/frame, PLC enabled)", decoder->samples);
    return 0;
failed:
    decoder_close(decoder);
    return -1;
}

static int decoder_init(struct mic_decoder *decoder, const char *etsi_path, const char *fallback_path)
{
    decoder_close(decoder);
    if (decoder_init_etsi(decoder, etsi_path) == 0) return 0;

    /* fallback: surgical lc3_* API */
    void *h = dlopen(fallback_path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        mlog("decoder_init: fallback dlopen %s: %s", fallback_path, dlerror());
        return -1;
    }
    decoder->library = h;
#define RS(var, name) do { *(void **)(&(var)) = dlsym(h, name); \
        if (!(var)) { mlog("decoder_init: dlsym %s failed", name); goto failed; } } while (0)
    RS(decoder->lc3_dec_get_size, "lc3_dec_get_size");
    RS(decoder->lc3_dec_init, "lc3_dec_init");
    RS(decoder->lc3_dec_set_hrmode, "lc3_dec_set_hrmode");
    RS(decoder->lc3_dec_set_frame_ms, "lc3_dec_set_frame_ms");
    RS(decoder->lc3_dec_get_output_samples, "lc3_dec_get_output_samples");
    RS(decoder->lc3_dec16, "lc3_dec16");
#undef RS
    int need = decoder->lc3_dec_get_size(SRATE, 1);
    if (need <= 0) goto failed;
    decoder->context = calloc(1, (size_t)need);
    if (!decoder->context) goto failed;
    if (decoder->lc3_dec_init(decoder->context, SRATE, 1, 0) || decoder->lc3_dec_set_hrmode(decoder->context, 0)) goto failed;
    if (decoder->lc3_dec_set_frame_ms(decoder->context, (float)FRAME_MS)) {
        mlog("decoder_init: set_frame_ms(%d) failed", FRAME_MS);
        goto failed;
    }
    decoder->samples = decoder->lc3_dec_get_output_samples(decoder->context);
    if (decoder->samples <= 0 || decoder->samples > NSAMP) decoder->samples = NSAMP;
    decoder->iov[0] = decoder->pcm;
    decoder->iov[1] = (char *)decoder->pcm + decoder->samples * 2;
    decoder->etsi = 0;
    mlog("decoder_init: surgical fallback ok (%d samples/frame, no PLC)", decoder->samples);
    return 0;
failed:
    decoder_close(decoder);
    return -1;
}

/* decode one 111B frame into decoder->pcm; 0 on success */
static int decoder_decode(struct mic_decoder *decoder, const uint8_t *frame, int plen)
{
    if (!decoder->context || !frame || plen <= 0 || plen > (int)sizeof decoder->input) return -1;
    if (decoder->etsi) {
        int16_t *outp = decoder->pcm;
        memcpy(decoder->input, frame, (size_t)plen);   /* dec16 writes into input */
        return decoder->e_dec16(decoder->context, decoder->input, plen, &outp, NULL, 0);
    }
    return decoder->lc3_dec16(decoder->context, frame, plen, decoder->iov);
}

/* PLC concealment frame into decoder->pcm; 0 on success */
static int decoder_conceal(struct mic_decoder *decoder)
{
    if (!decoder->context) return -1;
    if (decoder->etsi) {
        int16_t *outp = decoder->pcm;
        /* ETSI requires bfi_ext=1 for a NULL lost-frame input. A concealed
         * frame may return LC3PLUS_DECODE_ERROR (2) with valid PCM. */
        int rc = decoder->e_dec16(decoder->context, NULL, 0, &outp, NULL, 1);
        return rc == 2 ? 0 : rc;
    }
    return 1;                                     /* no PLC on surgical */
}

#endif
