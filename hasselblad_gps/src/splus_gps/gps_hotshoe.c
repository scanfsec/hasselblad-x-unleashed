#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "gps_hotshoe.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define HOTSHOE_QUEUE_SIZE 2048
#define HOTSHOE_CANDIDATES 12
#define HOTSHOE_CLOCK_STEP_NS UINT64_C(10000000)
#define HOTSHOE_FLUSH_DELAY_NS UINT64_C(2000000)
#define SECOND UINT64_C(1000000000)

typedef struct {
    uint64_t start_ns;
    unsigned bit;
    uint8_t byte;
} UartCandidate;

typedef struct {
    UartCandidate pending[HOTSHOE_CANDIDATES];
    size_t count;
    uint64_t last_event_ns, last_start_ns, excluded_until_ns;
    bool level, initialized, have_byte;
    GpsHotshoeByteFn emit;
    void *context;
} HotshoeDecoder;

typedef struct {
    uint64_t received_ns;
    uint8_t byte;
    bool discontinuity;
} HotshoeByte;

struct GpsHotshoe {
    int gpio_fd, notify_fd, stop_fd;
    pthread_t thread;
    pthread_mutex_t lock;
    HotshoeByte queue[HOTSHOE_QUEUE_SIZE];
    size_t head, count;
    GpsHotshoeStats stats;
    HotshoeDecoder decoder;
    uint64_t real_base_ns, boot_base_ns;
};

static uint64_t clock_ns(clockid_t clock)
{
    struct timespec now;
    if (clock_gettime(clock, &now))
        return 0;
    return (uint64_t)now.tv_sec * SECOND + (uint64_t)now.tv_nsec;
}

static uint64_t sample_time(const UartCandidate *candidate)
{
    /* GPIO122's measured waveform is decoded at phase0.2 of each data bit. */
    return candidate->start_ns + ((uint64_t)candidate->bit * 5 + 6) * SECOND / 48000;
}

static void decoder_gap(HotshoeDecoder *decoder, uint64_t when)
{
    decoder->count = 0;
    decoder->have_byte = false;
    decoder->excluded_until_ns = 0;
    decoder->emit(decoder->context, 0, when, true);
}

static void remove_candidate(HotshoeDecoder *decoder, size_t index)
{
    --decoder->count;
    memmove(decoder->pending + index, decoder->pending + index + 1,
            (decoder->count - index) * sizeof(*decoder->pending));
}

static void decoder_advance(HotshoeDecoder *decoder, uint64_t until)
{
    for (size_t index = 0; index < decoder->count;) {
        UartCandidate *candidate = &decoder->pending[index];
        while (candidate->bit < 8 && sample_time(candidate) < until) {
            candidate->byte |= (uint8_t)decoder->level << candidate->bit;
            ++candidate->bit;
        }
        if (candidate->bit < 8 || sample_time(candidate) >= until) {
            ++index;
            continue;
        }
        if (!decoder->level || candidate->start_ns < decoder->excluded_until_ns) {
            remove_candidate(decoder, index);
            continue;
        }
        uint64_t start = candidate->start_ns;
        uint8_t byte = candidate->byte;
        uint64_t receipt = sample_time(candidate);
        if (decoder->have_byte && start - decoder->last_start_ns > UINT64_C(115) * SECOND / 96000)
            decoder->emit(decoder->context, 0, receipt, true);
        decoder->emit(decoder->context, byte, receipt, false);
        decoder->last_start_ns = start;
        decoder->have_byte = true;
        decoder->excluded_until_ns = start + UINT64_C(93) * SECOND / 96000;
        while (decoder->count && decoder->pending[0].start_ns < decoder->excluded_until_ns)
            remove_candidate(decoder, 0);
        index = 0;
    }
}

static void decoder_event(HotshoeDecoder *decoder, uint64_t when, bool level)
{
    if (!decoder->initialized) {
        decoder->initialized = true;
        decoder->level = !level;
    } else if (when < decoder->last_event_ns || level == decoder->level) {
        decoder_gap(decoder, when);
    }
    decoder_advance(decoder, when);
    bool falling = decoder->level && !level;
    decoder->level = level;
    decoder->last_event_ns = when;
    if (!falling || when < decoder->excluded_until_ns)
        return;
    if (decoder->count == HOTSHOE_CANDIDATES)
        decoder_gap(decoder, when);
    decoder->pending[decoder->count++] = (UartCandidate){.start_ns = when};
}

static void notify(int fd)
{
    uint64_t one = 1;
    if (fd >= 0) {
        ssize_t written;
        do {
            written = write(fd, &one, sizeof(one));
        } while (written < 0 && errno == EINTR);
    }
}

static void enqueue(GpsHotshoe *receiver, HotshoeByte byte)
{
    pthread_mutex_lock(&receiver->lock);
    bool signal = receiver->count == 0;
    if (receiver->count == HOTSHOE_QUEUE_SIZE) {
        receiver->head = receiver->count = 0;
        ++receiver->stats.overflows;
        receiver->queue[receiver->count++] = (HotshoeByte){
            .received_ns = byte.received_ns, .discontinuity = true};
        signal = true;
    }
    size_t position = (receiver->head + receiver->count++) % HOTSHOE_QUEUE_SIZE;
    receiver->queue[position] = byte;
    if (byte.discontinuity)
        ++receiver->stats.discontinuities;
    else {
        ++receiver->stats.bytes;
        receiver->stats.last_byte_ns = byte.received_ns;
    }
    pthread_mutex_unlock(&receiver->lock);
    if (signal)
        notify(receiver->notify_fd);
}

static uint64_t event_boottime(const GpsHotshoe *receiver, uint64_t real)
{
    if (real > receiver->real_base_ns)
        return receiver->boot_base_ns + (real - receiver->real_base_ns);
    uint64_t age = receiver->real_base_ns - real;
    return age <= receiver->boot_base_ns ? receiver->boot_base_ns - age : 0;
}

static void receive_byte(void *context, uint8_t byte, uint64_t real, bool gap)
{
    GpsHotshoe *receiver = context;
    enqueue(receiver, (HotshoeByte){.byte = byte, .received_ns = event_boottime(receiver, real),
                                    .discontinuity = gap});
}

static void failure(GpsHotshoe *receiver, int error)
{
    pthread_mutex_lock(&receiver->lock);
    receiver->stats.io_error = error;
    pthread_mutex_unlock(&receiver->lock);
    notify(receiver->notify_fd);
}

static bool update_clocks(GpsHotshoe *receiver, uint64_t before, uint64_t real, uint64_t after)
{
    if (!before || !real || !after || after < before)
        return false;
    uint64_t boot = before + (after - before) / 2;
    int64_t old_offset = (int64_t)receiver->real_base_ns - (int64_t)receiver->boot_base_ns;
    int64_t offset = (int64_t)real - (int64_t)boot;
    /* A preemption between clock reads widens uncertainty, without proving a step. */
    if (receiver->real_base_ns && after - before > HOTSHOE_CLOCK_STEP_NS &&
        old_offset >= (int64_t)real - (int64_t)after &&
        old_offset <= (int64_t)real - (int64_t)before)
        return true;
    int64_t change = offset - old_offset;
    if (receiver->real_base_ns && (change > (int64_t)HOTSHOE_CLOCK_STEP_NS ||
                                  change < -(int64_t)HOTSHOE_CLOCK_STEP_NS)) {
        receiver->decoder.count = 0;
        receiver->decoder.initialized = false;
        receiver->decoder.have_byte = false;
        receiver->decoder.excluded_until_ns = 0;
        pthread_mutex_lock(&receiver->lock);
        receiver->head = receiver->count = 0;
        /* This already converted BOOTTIME receipt remains valid across a
         * wall-clock step. Only new bytes may extend its liveness timeout. */
        ++receiver->stats.clock_resets;
        pthread_mutex_unlock(&receiver->lock);
        receiver->real_base_ns = real;
        receiver->boot_base_ns = boot;
        enqueue(receiver, (HotshoeByte){.received_ns = boot, .discontinuity = true});
    }
    receiver->real_base_ns = real;
    receiver->boot_base_ns = boot;
    return true;
}

static bool observe_clocks(GpsHotshoe *receiver)
{
    uint64_t before = clock_ns(CLOCK_BOOTTIME);
    uint64_t real = clock_ns(CLOCK_REALTIME);
    uint64_t after = clock_ns(CLOCK_BOOTTIME);
    return update_clocks(receiver, before, real, after);
}

static void *reader_thread(void *context)
{
    GpsHotshoe *receiver = context;
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(3, &cpus);
    int scheduling = sched_setaffinity(0, sizeof(cpus), &cpus) ? errno : 0;
    struct sched_param priority = {.sched_priority = 1};
    int priority_error = sched_setscheduler(0, SCHED_FIFO, &priority) ? errno : 0;
    pthread_mutex_lock(&receiver->lock);
    receiver->stats.scheduling_error = scheduling ? scheduling : priority_error;
    pthread_mutex_unlock(&receiver->lock);
    struct pollfd descriptors[] = {{.fd = receiver->gpio_fd, .events = POLLIN},
                                  {.fd = receiver->stop_fd, .events = POLLIN}};
    while (true) {
        int timeout = receiver->decoder.count ? 5 : -1;
        int ready = poll(descriptors, 2, timeout);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0) {
            failure(receiver, errno);
            break;
        }
        if (descriptors[1].revents)
            break;
        if (descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            failure(receiver, EIO);
            break;
        }
        if (!observe_clocks(receiver)) {
            failure(receiver, EIO);
            break;
        }
        struct gpioevent_data events[64];
        ssize_t bytes;
        do {
            bytes = read(receiver->gpio_fd, events, sizeof(events));
            if (bytes < 0 && errno == EINTR)
                continue;
            if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break;
            if (bytes <= 0 || bytes % (ssize_t)sizeof(*events)) {
                failure(receiver, bytes < 0 ? errno : EPROTO);
                return NULL;
            }
            for (size_t i = 0; i < (size_t)bytes / sizeof(*events); ++i) {
                bool rising = events[i].id == GPIOEVENT_EVENT_RISING_EDGE;
                if ((!rising && events[i].id != GPIOEVENT_EVENT_FALLING_EDGE) ||
                    events[i].timestamp > receiver->real_base_ns + SECOND ||
                    events[i].timestamp + SECOND < receiver->real_base_ns) {
                    decoder_gap(&receiver->decoder, receiver->real_base_ns);
                    receiver->decoder.initialized = false;
                    continue;
                }
                decoder_event(&receiver->decoder, events[i].timestamp, rising);
            }
        } while (bytes > 0);
        if (receiver->real_base_ns > HOTSHOE_FLUSH_DELAY_NS)
            decoder_advance(&receiver->decoder, receiver->real_base_ns - HOTSHOE_FLUSH_DELAY_NS);
    }
    return NULL;
}

static int open_gpio(void)
{
    int chip = open("/dev/gpiochip3", O_RDONLY | O_CLOEXEC);
    if (chip < 0)
        return -1;
    struct gpiochip_info info = {0};
    struct gpioline_info line = {.line_offset = 26};
    int result = ioctl(chip, GPIO_GET_CHIPINFO_IOCTL, &info) ||
                 ioctl(chip, GPIO_GET_LINEINFO_IOCTL, &line);
    if (!result && (strcmp(info.label, "f8143000.gpio3") || info.lines != 32 ||
                    line.consumer[0] || (line.flags & (GPIOLINE_FLAG_KERNEL | GPIOLINE_FLAG_IS_OUT |
                    GPIOLINE_FLAG_ACTIVE_LOW | GPIOLINE_FLAG_OPEN_DRAIN | GPIOLINE_FLAG_OPEN_SOURCE)))) {
        errno = ENODEV;
        result = -1;
    }
    struct gpioevent_request request = {.lineoffset = 26, .handleflags = GPIOHANDLE_REQUEST_INPUT,
        .eventflags = GPIOEVENT_REQUEST_BOTH_EDGES, .consumer_label = "splus-hotshoe-gps", .fd = -1};
    if (!result)
        result = ioctl(chip, GPIO_GET_LINEEVENT_IOCTL, &request);
    int error = errno;
    close(chip);
    if (result) {
        errno = error;
        return -1;
    }
    if (fcntl(request.fd, F_SETFD, FD_CLOEXEC) || fcntl(request.fd, F_SETFL, O_NONBLOCK)) {
        error = errno;
        close(request.fd);
        errno = error;
        return -1;
    }
    return request.fd;
}

GpsHotshoe *gps_hotshoe_open(void)
{
    GpsHotshoe *receiver = calloc(1, sizeof(*receiver));
    if (!receiver)
        return NULL;
    receiver->gpio_fd = receiver->notify_fd = receiver->stop_fd = -1;
    int result = pthread_mutex_init(&receiver->lock, NULL);
    if (result) {
        free(receiver);
        errno = result;
        return NULL;
    }
    receiver->gpio_fd = open_gpio();
    if (receiver->gpio_fd < 0)
        goto failed;
    receiver->notify_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    receiver->stop_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (receiver->notify_fd < 0 || receiver->stop_fd < 0)
        goto failed;
    receiver->decoder.emit = receive_byte;
    receiver->decoder.context = receiver;
    result = pthread_create(&receiver->thread, NULL, reader_thread, receiver);
    if (result) {
        errno = result;
        goto failed;
    }
    return receiver;
failed:
    result = errno;
    if (receiver->gpio_fd >= 0) close(receiver->gpio_fd);
    if (receiver->notify_fd >= 0) close(receiver->notify_fd);
    if (receiver->stop_fd >= 0) close(receiver->stop_fd);
    pthread_mutex_destroy(&receiver->lock);
    free(receiver);
    errno = result;
    return NULL;
}

void gps_hotshoe_close(GpsHotshoe *receiver)
{
    if (!receiver)
        return;
    notify(receiver->stop_fd);
    pthread_join(receiver->thread, NULL);
    close(receiver->gpio_fd);
    close(receiver->notify_fd);
    close(receiver->stop_fd);
    pthread_mutex_destroy(&receiver->lock);
    free(receiver);
}

int gps_hotshoe_fd(const GpsHotshoe *receiver)
{
    return receiver ? receiver->notify_fd : -1;
}

int gps_hotshoe_drain(GpsHotshoe *receiver, GpsHotshoeByteFn receive, void *context)
{
    if (!receiver || !receive) {
        errno = EINVAL;
        return -1;
    }
    uint64_t notified;
    ssize_t size;
    do {
        size = read(receiver->notify_fd, &notified, sizeof(notified));
    } while (size < 0 && errno == EINTR);
    if (size < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        return -1;
    HotshoeByte bytes[HOTSHOE_QUEUE_SIZE];
    pthread_mutex_lock(&receiver->lock);
    size_t count = receiver->count;
    for (size_t i = 0; i < count; ++i)
        bytes[i] = receiver->queue[(receiver->head + i) % HOTSHOE_QUEUE_SIZE];
    receiver->head = receiver->count = 0;
    pthread_mutex_unlock(&receiver->lock);
    for (size_t i = 0; i < count; ++i)
        receive(context, bytes[i].byte, bytes[i].received_ns, bytes[i].discontinuity);
    return (int)count;
}

GpsHotshoeStats gps_hotshoe_stats(GpsHotshoe *receiver)
{
    if (!receiver)
        return (GpsHotshoeStats){.io_error = EINVAL};
    pthread_mutex_lock(&receiver->lock);
    GpsHotshoeStats stats = receiver->stats;
    pthread_mutex_unlock(&receiver->lock);
    return stats;
}
