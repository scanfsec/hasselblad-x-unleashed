/* Asynchronous mono/stereo 48 kHz AAC-LC/ADTS sink using the public FDK ABI.
 * Producer calls only copy bounded PCM or advance a silence timeline. Encoding,
 * file writes and flushing belong to the worker. Callers serialize start/free
 * with producer access; snapshots and finish requests are thread safe.
 */
#ifndef MIC_AAC_H
#define MIC_AAC_H
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "mic_pcm_observation.h"
#include "mic_aac_timeline.h"
#include "mic_record_file.h"

#define MIC_AAC_CHUNK 2400
#define MIC_AAC_QUEUE 32
typedef struct { int count; void **buffers; int *ids, *sizes, *elements; } mic_aac_buffers;
typedef struct { int samples, ancillary; } mic_aac_input;
typedef struct { int bytes, samples, ancillary, reservoir; } mic_aac_output;
typedef struct {
    unsigned (*open)(void **, unsigned, unsigned);
    unsigned (*param)(void *, unsigned, unsigned);
    unsigned (*encode)(void *, const mic_aac_buffers *, const mic_aac_buffers *, const mic_aac_input *, mic_aac_output *);
    unsigned (*info)(void *, void *);
    unsigned (*close)(void **);
} mic_aac_api;
typedef struct {
    uint64_t submitted, input_samples, actual_samples, silent_samples;
    uint64_t packets, bytes, overflows, overflow_samples;
    unsigned frame_length, encoder_delay_samples, bitrate, channels;
    int completed, encoder_flushed, error;
    uint64_t queued_chunks, padding_frames;
    uint64_t consumed_observed_ns, input_closed_ns, flush_observed_ns, completed_observed_ns;
    mic_pcm_submission first_queued, last_queued, first_overflow, last_overflow, last_padding;
} mic_aac_stats;
typedef struct {
    uint64_t offset;
    int count, silent;
    int16_t pcm[MIC_AAC_CHUNK * 2];
} mic_aac_chunk;
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t wake;
    pthread_t thread;
    mic_aac_api api;
    void *library, *encoder;
    FILE *file;
    char path[256];
    mic_aac_stats stats;
    mic_input_journal input_journal;
    mic_aac_chunk queue[MIC_AAC_QUEUE];
    unsigned head, count;
    int closing;
} mic_aac;

_Static_assert(sizeof(int) == 4 && sizeof(int16_t) == 2, "FDK integer ABI");
#ifdef __aarch64__
_Static_assert(sizeof(mic_aac_buffers) == 40, "FDK AArch64 descriptor ABI");
#endif

static mic_aac_stats mic_aac_snapshot(mic_aac *a)
{
    pthread_mutex_lock(&a->mutex);
    mic_aac_stats result = a->stats;
    pthread_mutex_unlock(&a->mutex);
    return result;
}

/* Closing still owns the file/encoder, but no producer may extend its take. */
static int mic_aac_accepting(mic_aac *a)
{
    pthread_mutex_lock(&a->mutex);
    int accepting = !a->closing && !a->stats.completed;
    pthread_mutex_unlock(&a->mutex);
    return accepting;
}

/* Validate and count every ADTS frame, including multiple frames per output. */
static int mic_aac_packet(mic_aac *a, const unsigned char *p, int n)
{
    if (n < 0 || n > 8192) return -1;
    unsigned packets = 0;
    for (int offset = 0; offset < n;) {
        const unsigned char *h = p + offset;
        if (n - offset < 7 || h[0] != 0xff || (h[1] & 0xf6) != 0xf0 ||
            (h[2] >> 6) != 1 || ((h[2] >> 2) & 15) != 3 ||
            (((h[2] & 1) << 2) | (h[3] >> 6)) != a->stats.channels || (h[6] & 3)) return -1;
        int length = ((h[3] & 3) << 11) | (h[4] << 3) | (h[5] >> 5);
        if (length < ((h[1] & 1) ? 7 : 9) || length > n - offset) return -1;
        offset += length; packets++;
    }
    if (n && fwrite(p, 1, (size_t)n, a->file) != (size_t)n) return -1;
    pthread_mutex_lock(&a->mutex);
    a->stats.bytes += (unsigned)n; a->stats.packets += packets;
    pthread_mutex_unlock(&a->mutex);
    return 0;
}

static int mic_aac_encode_pcm(mic_aac *a, const int16_t *pcm, int count, int silent)
{
    unsigned char packet[8192];
    int channels = (int)a->stats.channels;
    while (count > 0) {
        void *in_data = (void *)pcm, *out_data = packet;
        int in_id = 0, out_id = 3, in_bytes = count * channels * 2, out_bytes = sizeof packet;
        int in_element = 2, out_element = 1;
        mic_aac_buffers in = {1, &in_data, &in_id, &in_bytes, &in_element};
        mic_aac_buffers out = {1, &out_data, &out_id, &out_bytes, &out_element};
        mic_aac_input args = {count * channels, 0}; mic_aac_output result = {0};
        unsigned error = a->api.encode(a->encoder, &in, &out, &args, &result);
        if (error || result.samples <= 0 || result.samples > count * channels || result.samples % channels) return -1;
        /* FDK counts interleaved values; public timelines count frames per
         * channel, so adding stereo must not double duration or pad length.
         * https://android.googlesource.com/platform/external/aac/+/master/libAACenc/include/aacenc_lib.h */
        int consumed = result.samples / channels;
        pthread_mutex_lock(&a->mutex);
        a->stats.input_samples += (unsigned)consumed;
        a->stats.consumed_observed_ns = mic_pcm_observed_ns();
        if (silent) a->stats.silent_samples += (unsigned)consumed;
        else a->stats.actual_samples += (unsigned)consumed;
        pthread_mutex_unlock(&a->mutex);
        if (mic_aac_packet(a, packet, result.bytes)) return -1;
        count -= consumed; pcm += result.samples;
    }
    return 0;
}

static int mic_aac_flush(mic_aac *a)
{
    unsigned char packet[8192]; void *data = packet;
    int id = 3, bytes = sizeof packet, element = 1;
    mic_aac_buffers out = {1, &data, &id, &bytes, &element};
    int16_t dummy[2] = {0}; void *input_data = dummy;
    int input_id = 0, input_bytes = sizeof dummy, input_element = 2;
    mic_aac_buffers in = {1, &input_data, &input_id, &input_bytes, &input_element};
    mic_aac_input args = {-1, 0};
    for (int attempt = 0; attempt < 64; attempt++) {
        mic_aac_output result = {0};
        unsigned error = a->api.encode(a->encoder, &in, &out, &args, &result);
        if (error == 0x80) {
            pthread_mutex_lock(&a->mutex); a->stats.encoder_flushed = 1;
            a->stats.flush_observed_ns = mic_pcm_observed_ns();
            pthread_mutex_unlock(&a->mutex); return 0;
        }
        if (error || mic_aac_packet(a, packet, result.bytes)) return -1;
    }
    return -1;
}

static void *mic_aac_worker(void *opaque)
{
    mic_aac *a = opaque;
    static const int16_t silence[MIC_AAC_CHUNK * 2] = {0};
    uint64_t cursor = 0;
    int failed = 0;
    for (;;) {
        mic_aac_chunk next;
        int have_chunk = 0, closing;
        uint64_t until;
        pthread_mutex_lock(&a->mutex);
        while (!a->count && cursor == a->stats.submitted && !a->closing)
            pthread_cond_wait(&a->wake, &a->mutex);
        closing = a->closing;
        until = a->stats.submitted;
        if (a->count) {
            next = a->queue[a->head]; a->head = (a->head + 1) % MIC_AAC_QUEUE;
            a->count--; have_chunk = 1; until = next.offset;
        }
        pthread_mutex_unlock(&a->mutex);
        while (cursor < until && !failed) {
            int n = until - cursor > MIC_AAC_CHUNK ? MIC_AAC_CHUNK : (int)(until - cursor);
            failed = mic_aac_encode_pcm(a, silence, n, 1); cursor += n;
        }
        if (have_chunk && !failed) {
            failed = mic_aac_encode_pcm(a, next.silent ? silence : next.pcm, next.count, next.silent);
            cursor += next.count;
        }
        if (failed || (!have_chunk && closing)) break;
    }
    if (!failed) failed = mic_aac_flush(a);
    if (fflush(a->file)) failed = 1;
    if (fsync(fileno(a->file))) failed = 1;
    if (!mic_record_file_present(a->file, a->path)) failed = 1;
    if (fclose(a->file)) failed = 1;
    a->file = NULL;
    if (a->api.close(&a->encoder)) failed = 1;
    pthread_mutex_lock(&a->mutex);
    a->stats.error = !!failed;
    a->stats.completed = 1;
    a->stats.completed_observed_ns = mic_pcm_observed_ns();
    pthread_mutex_unlock(&a->mutex);
    return NULL;
}

static mic_aac *mic_aac_start_api_channels(const char *path, const mic_aac_api *api,
                                          void *library, unsigned channels)
{
    mic_aac *a = calloc(1, sizeof *a);
    int created = 0, mutex_ready = 0, cond_ready = 0;
    if (!a) { if (library) dlclose(library); return NULL; }
    a->api = *api; a->library = library;
    if (!path || strlen(path) >= sizeof a->path) goto fail;
    snprintf(a->path, sizeof a->path, "%s", path);
    if (channels != 1 && channels != 2) goto fail;
    a->stats.channels = channels;
    if (!api->open || !api->param || !api->encode || !api->info || !api->close) goto fail;
    if (api->open(&a->encoder, 1, channels)) goto fail;
    const unsigned parameters[][2] = {{0x100, 2}, {0x103, 48000}, {0x106, channels}, {0x101, 128000 * channels}, {0x300, 2}};
    for (unsigned i = 0; i < sizeof parameters / sizeof parameters[0]; i++)
        if (api->param(a->encoder, parameters[i][0], parameters[i][1])) goto fail;
    if (api->encode(a->encoder, NULL, NULL, NULL, NULL)) goto fail;
    /* Common prefix of camera FDK info versions; reserve space for either tail. */
    uint32_t info[128] = {0};
    if (api->info(a->encoder, info) || info[3] != channels || info[4] != 1024 ||
        !info[0] || info[0] > 8192 || info[5] > 48000) goto fail;
    a->stats.frame_length = info[4]; a->stats.encoder_delay_samples = info[5];
    a->stats.bitrate = 128000 * channels;
    if (pthread_mutex_init(&a->mutex, NULL)) goto fail;
    mutex_ready = 1;
    if (pthread_cond_init(&a->wake, NULL)) goto fail;
    cond_ready = 1;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) goto fail;
    created = 1; a->file = fdopen(fd, "wb");
    if (!a->file) { close(fd); goto fail; }
    if (pthread_create(&a->thread, NULL, mic_aac_worker, a)) goto fail;
    return a;
fail:
    if (a->file) fclose(a->file);
    if (created) unlink(path);
    if (cond_ready) pthread_cond_destroy(&a->wake);
    if (mutex_ready) pthread_mutex_destroy(&a->mutex);
    if (a->encoder && api->close) api->close(&a->encoder);
    if (library) dlclose(library);
    free(a); return NULL;
}

static mic_aac *mic_aac_start_api(const char *path, const mic_aac_api *api, void *library)
{
    return mic_aac_start_api_channels(path, api, library, 1);
}

static mic_aac *mic_aac_start_channels(const char *path, unsigned channels)
{
    void *library = dlopen("/system/lib64/libAACenc.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) return NULL;
    mic_aac_api api = {
        .open = (void *)dlsym(library, "aacEncOpen"),
        .param = (void *)dlsym(library, "aacEncoder_SetParam"),
        .encode = (void *)dlsym(library, "aacEncEncode"),
        .info = (void *)dlsym(library, "aacEncInfo"),
        .close = (void *)dlsym(library, "aacEncClose")
    };
    return mic_aac_start_api_channels(path, &api, library, channels);
}

static mic_aac *mic_aac_start(const char *path)
{
    return mic_aac_start_channels(path, 1);
}

/* count is sample frames per channel; pcm contains count*channels values.
 * Returns 1 queued, 0 timeline-preserving overflow, -1 invalid/closed. */
static int mic_aac_write_observed(mic_aac *a, const int16_t *pcm, int count, int silent,
                                  unsigned real_mask, const mic_pcm_origin origins[2])
{
    if (count <= 0 || count > MIC_AAC_CHUNK || (!pcm && !silent)) return -1;
    pthread_mutex_lock(&a->mutex);
    if (a->closing || a->stats.completed) { pthread_mutex_unlock(&a->mutex); return -1; }
    int accepted = a->count < MIC_AAC_QUEUE;
    mic_pcm_submission observation = {.first_sample = a->stats.submitted,
        .frames = (unsigned)count, .enqueue_ns = mic_pcm_observed_ns(),
        .real_mask = silent ? 0 : real_mask & 3};
    if (origins) memcpy(observation.origins, origins, sizeof observation.origins);
    if (accepted) {
        mic_aac_chunk *p = &a->queue[(a->head + a->count) % MIC_AAC_QUEUE];
        p->offset = a->stats.submitted; p->count = count; p->silent = !!silent;
        if (!silent) memcpy(p->pcm, pcm, (size_t)count * a->stats.channels * 2);
        a->count++;
        if (!a->stats.queued_chunks++) a->stats.first_queued = observation;
        a->stats.last_queued = observation;
    } else {
        observation.real_mask = 0; /* worker replaces dropped payload with zeros */
        if (!a->stats.overflows++) a->stats.first_overflow = observation;
        a->stats.last_overflow = observation;
        a->stats.overflow_samples += (unsigned)count;
    }
    mic_input_append(&a->input_journal, accepted ? MIC_INPUT_QUEUED : MIC_INPUT_OVERFLOW, &observation);
    a->stats.submitted += (unsigned)count;
    pthread_cond_signal(&a->wake); pthread_mutex_unlock(&a->mutex);
    return accepted;
}

static int mic_aac_write(mic_aac *a, const int16_t *pcm, int count, int silent)
{
    return mic_aac_write_observed(a, pcm, count, silent, silent ? 0 : (1u << a->stats.channels) - 1, NULL);
}

static void mic_aac_pad(mic_aac *a, uint64_t samples)
{
    pthread_mutex_lock(&a->mutex);
    if (!a->closing && !a->stats.completed && samples > a->stats.submitted) {
        a->stats.last_padding = (mic_pcm_submission){.first_sample = a->stats.submitted,
            .frames = samples - a->stats.submitted, .enqueue_ns = mic_pcm_observed_ns()};
        a->stats.padding_frames += samples - a->stats.submitted;
        mic_input_append(&a->input_journal, MIC_INPUT_PADDING, &a->stats.last_padding);
        a->stats.submitted = samples; pthread_cond_signal(&a->wake);
    }
    pthread_mutex_unlock(&a->mutex);
}

/* Nonblocking. Completion is true only after flushing, closing file and codec. */
static void mic_aac_finish(mic_aac *a)
{
    pthread_mutex_lock(&a->mutex);
    if (!a->closing) a->stats.input_closed_ns = mic_pcm_observed_ns();
    a->closing = 1; pthread_cond_signal(&a->wake);
    pthread_mutex_unlock(&a->mutex);
}

/* Only free after a completed snapshot, or outside all realtime paths at shutdown. */
static void mic_aac_free(mic_aac *a)
{
    if (!a) return;
    mic_aac_finish(a); pthread_join(a->thread, NULL);
    if (a->library) dlclose(a->library);
    pthread_cond_destroy(&a->wake); pthread_mutex_destroy(&a->mutex); free(a);
}
#endif
