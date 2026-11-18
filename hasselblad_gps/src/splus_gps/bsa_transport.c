#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "bsa_transport.h"
#include "arm64_hook.h"

#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

_Static_assert(sizeof(struct gps_bsa_message) == 280, "Unexpected GPS bridge wire layout");

enum {
    GATT_REGISTER = 0x6a8, GATT_DEREGISTER = 0x6a9, GATT_OPEN = 0x6aa, GATT_CLOSE = 0x6ab,
    GATT_WRITE = 0x6ad, GATT_SEARCH = 0x6ae, GATT_REGISTER_NOTIFY = 0x6af, GATT_MTU = 0x6b3,
    EVENT_DEREGISTER = 0x6c5, EVENT_OPEN = 0x6c6, EVENT_CLOSE = 0x6c7, EVENT_WRITE = 0x6c9,
    EVENT_SERVICE = 0x6ca, EVENT_SEARCH = 0x6cb, EVENT_DATA = 0x6cc, EVENT_MTU = 0x6d0,
};

enum phase { IDLE, OPENING, MTU, SEARCHING, SUBSCRIBING, STREAMING, CLOSING, DEREGISTERING, FAULTED };

struct dispatch_event {
    uint64_t boottime_ns;
    uint64_t generation;
    uint16_t id;
    uint16_t size;
    uint8_t data[0x56 + GPS_BSA_DATA_MAX];
};

static const uint8_t service_uuid[] = {0xa5, 0x72, 0, 1, 0x0d, 0x52, 0x4d, 0x11,
                                       0xa8, 0x25, 0x8b, 0x94, 0xb6, 0x3b, 0xf1, 0x0c};
static int (*bsa_sync)(int, const void *, int, void *, int);
static int (*bsa_command)(int, const void *, int);
static const volatile uint8_t *bsa_ipc_active;
static ps_arm64_hook dispatch_hook;
static int events[2] = {-1, -1};
static atomic_bool observing;
static atomic_bool lost_event;
static _Atomic uint64_t observed_generation;
static _Atomic uint64_t observed_peer;
static atomic_uint observed_client, observed_connection;
static struct {
    gps_bsa_report_fn report;
    void *context;
    enum phase phase;
    uint64_t sequence, generation, deadline;
    uint32_t result;
    uint16_t connection;
    uint8_t client, peer[6], characteristic[52], cccd[396];
    bool service_found, notify_enabled, stop, uncertain, installed;
} session;

static uint16_t load16(const uint8_t *data)
{
    return (uint16_t)(data[0] | (uint16_t)data[1] << 8);
}

static void store16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
}

static uint64_t pack_peer(const uint8_t *data)
{
    uint64_t packed = 0;
    for (unsigned i = 0; i < 6; ++i)
        packed |= (uint64_t)data[i] << (i * 8);
    return packed;
}

static uint64_t clock_ns(clockid_t clock)
{
    struct timespec now;
    if (clock_gettime(clock, &now) < 0)
        return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

uint64_t gps_bsa_boottime_ns(void)
{
    return clock_ns(CLOCK_BOOTTIME);
}

uint64_t gps_bsa_monotonic_ns(void)
{
    return clock_ns(CLOCK_MONOTONIC);
}

static size_t dispatch_size(int id, const uint8_t *body)
{
    unsigned client = atomic_load_explicit(&observed_client, memory_order_acquire);
    unsigned connection = atomic_load_explicit(&observed_connection, memory_order_acquire);
    uint64_t peer = atomic_load_explicit(&observed_peer, memory_order_acquire);
    switch (id) {
    case EVENT_DEREGISTER: return client && body[2] == client ? 3 : 0;
    case EVENT_OPEN:
    case EVENT_CLOSE:
        return client && body[4] == client && pack_peer(body + 5) == peer ? (id == EVENT_OPEN ? 11 : 14) : 0;
    case EVENT_WRITE: return connection && load16(body) == connection ? 4 : 0;
    case EVENT_SEARCH: return connection && load16(body + 2) == connection ? 4 : 0;
    case EVENT_SERVICE: return connection && load16(body) == connection ? 32 : 0;
    case EVENT_MTU: return connection && load16(body + 2) == connection ? 6 : 0;
    case EVENT_DATA: {
        if (!connection || load16(body) != connection || pack_peer(body + 2) != peer)
            return 0;
        size_t payload = load16(body + 0x54);
        return 0x56 + (payload > 4 && payload <= GPS_BSA_DATA_MAX ? payload : 0);
    }
    default: return 0;
    }
}

static void observe_dispatch(int id, const uint8_t *body)
{
    int saved_errno = errno;
    uint64_t generation = atomic_load_explicit(&observed_generation, memory_order_acquire);
    if (body && atomic_load_explicit(&observing, memory_order_acquire)) {
        size_t size = dispatch_size(id, body);
        if (size && generation == atomic_load_explicit(&observed_generation, memory_order_acquire)) {
            struct dispatch_event event = {.boottime_ns = gps_bsa_boottime_ns(), .id = (uint16_t)id,
                                           .generation = generation, .size = (uint16_t)size};
            memcpy(event.data, body, size);
            if (send(events[1], &event, offsetof(struct dispatch_event, data) + size, MSG_NOSIGNAL) < 0)
                atomic_store_explicit(&lost_event, true, memory_order_release);
        }
    }
    errno = saved_errno;
    void (*original)(int, const uint8_t *) = ps_arm64_hook_original(&dispatch_hook);
    original(id, body);
}

static int main_image(struct dl_phdr_info *info, size_t size, void *context)
{
    (void)size;
    if (info->dlpi_name[0] && strcmp(info->dlpi_name, "/system/bin/bt_bsa_app"))
        return 0;
    *(uintptr_t *)context = info->dlpi_addr;
    return 1;
}

int gps_bsa_install(gps_bsa_report_fn report, void *context)
{
    static const uint32_t sync_prefix[] = {0xa9ba7bfd, 0x7120005f, 0x910003fd, 0xa90153f3};
    static const uint32_t command_prefix[] = {0xa9be7bfd, 0x52800044, 0x910003fd, 0x91007ba3};
    static const uint32_t dispatch_prefix[] = {0xa9bf7bfd, 0x511b1402, 0x2a0003e3, 0x7100605f};
    char executable[64];
    ssize_t len = readlink("/proc/self/exe", executable, sizeof executable - 1);
    if (!report || session.installed || len < 0)
        return -1;
    executable[len] = 0;
    if (strcmp(executable, "/system/bin/bt_bsa_app"))
        return -1;

    /* The loader qualifies the whole ELF. These guards additionally verify
     * the mapped live entry bytes before the process can send GPS commands. */
    uintptr_t base = 0;
    dl_iterate_phdr(main_image, &base);
    if (!base || memcmp((void *)(base + 0x3e484), sync_prefix, sizeof sync_prefix) ||
        memcmp((void *)(base + 0x3e884), command_prefix, sizeof command_prefix))
        return -1;
    if (socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, events) < 0)
        return -1;
    bsa_sync = (void *)(base + 0x3e484);
    bsa_command = (void *)(base + 0x3e884);
    /* The live request gate tested by the sync wrapper at 0x3e4f0. */
    bsa_ipc_active = (const volatile uint8_t *)(base + 0x83059);
    session.report = report;
    session.context = context;
    int result = ps_arm64_hook_install(&dispatch_hook, (void *)(base + 0x46ce8), dispatch_prefix,
                                       (void *)observe_dispatch);
    if (result < 0) {
        close(events[0]);
        close(events[1]);
        events[0] = events[1] = -1;
        return -1;
    }
    session.installed = true;
    if (result != 0)
        session.phase = FAULTED;
    return result;
}

int gps_bsa_event_fd(void)
{
    return events[0];
}

uint64_t gps_bsa_deadline_ns(void)
{
    return session.deadline;
}

static void report(uint32_t type, uint32_t status, uint64_t boottime, const uint8_t *data, size_t size)
{
    struct gps_bsa_message message = {.version = GPS_BSA_VERSION, .type = type, .sequence = session.sequence,
                                      .boottime_ns = boottime, .status = status, .size = (uint32_t)size};
    if (size)
        memcpy(message.data, data, size);
    session.report(session.context, &message);
}

static void wait_until(enum phase next, unsigned ms)
{
    session.phase = next;
    session.deadline = gps_bsa_monotonic_ns() + (uint64_t)ms * 1000000;
}

static void finished(void)
{
    atomic_store_explicit(&observing, false, memory_order_release);
    if (atomic_exchange_explicit(&lost_event, false, memory_order_acq_rel)) {
        session.uncertain = true;
        if (!session.result)
            session.result = EOVERFLOW;
    }
    session.phase = session.uncertain ? FAULTED : IDLE;
    session.deadline = 0;
    report(session.uncertain ? GPS_BSA_FAULT : GPS_BSA_STOPPED, session.result,
           gps_bsa_boottime_ns(), NULL, 0);
}

static void deregister(void)
{
    if (!session.client) {
        finished();
        return;
    }
    wait_until(DEREGISTERING, 1500);
    if (bsa_command(GATT_DEREGISTER, &session.client, 1)) {
        session.uncertain = true;
        if (!session.result)
            session.result = EIO;
        finished();
    }
}

static void close_connection(void)
{
    if (!session.connection) {
        deregister();
        return;
    }
    if (session.notify_enabled) {
        session.notify_enabled = false;
        session.cccd[0x3c] = 0;
        bsa_command(GATT_WRITE, session.cccd, sizeof session.cccd);
    }
    uint8_t body[2];
    store16(body, session.connection);
    wait_until(CLOSING, 1500);
    if (bsa_command(GATT_CLOSE, body, sizeof body)) {
        session.uncertain = true;
        if (!session.result)
            session.result = EIO;
        deregister();
    }
}

static void fail(uint32_t reason, bool uncertain)
{
    if (!session.result)
        session.result = reason;
    session.uncertain |= uncertain;
    close_connection();
}

int gps_bsa_start(uint64_t sequence, const uint8_t peer[6])
{
    if (!sequence || !peer || !session.installed || session.phase != IDLE)
        return -1;
    session.sequence = sequence;
    memcpy(session.peer, peer, sizeof session.peer);
    session.connection = session.client = session.result = 0;
    session.service_found = session.notify_enabled = session.stop = session.uncertain = false;
    atomic_store_explicit(&lost_event, false, memory_order_release);
    if (!*bsa_ipc_active) {
        session.result = 105;
        finished();
        return 0;
    }
    atomic_store_explicit(&observed_peer, pack_peer(peer), memory_order_release);
    atomic_store_explicit(&observed_client, 0, memory_order_release);
    atomic_store_explicit(&observed_connection, 0, memory_order_release);
    atomic_store_explicit(&observed_generation, ++session.generation, memory_order_release);
    atomic_store_explicit(&observing, true, memory_order_release);

    uint8_t body[40] = {2, 0, 0, 0, 0x72, 0xa5};
    uint8_t response[4] = {0xff, 0xff, 0xff, 0xff};
    int rc = bsa_sync(GATT_REGISTER, body, sizeof body, response, sizeof response);
    uint16_t status = load16(response);
    if (rc || status || response[2] < 1 || response[2] > 10) {
        session.result = rc ? (uint32_t)rc : (status ? status : EPROTO);
        /* 114 is issued before send when the native transaction slot cannot
         * be reserved. 105 can also be returned after a send. */
        session.uncertain = rc ? rc != 114 : status == 0 || status == 0xffff;
        finished();
        return 0;
    }
    session.client = response[2];
    atomic_store_explicit(&observed_client, session.client, memory_order_release);
    uint8_t open[8] = {session.client};
    memcpy(open + 1, peer, sizeof session.peer);
    open[7] = 1;
    wait_until(OPENING, 5500);
    if (bsa_command(GATT_OPEN, open, sizeof open))
        fail(EIO, true);
    return 0;
}

void gps_bsa_stop(void)
{
    session.stop = true;
    if (session.phase >= MTU && session.phase <= STREAMING)
        close_connection();
    /* An open in flight must resolve before either close or deregistration. */
}

static void found_service(const uint8_t *body)
{
    if (load16(body + 4) != sizeof service_uuid)
        return;
    bool forward = !memcmp(body + 8, service_uuid, sizeof service_uuid);
    bool reversed = true;
    for (size_t i = 0; i < sizeof service_uuid; ++i)
        reversed &= body[8 + i] == service_uuid[sizeof service_uuid - 1 - i];
    if (!forward && !reversed)
        return;
    memset(session.characteristic, 0, sizeof session.characteristic);
    memcpy(session.characteristic, body + 4, 28);
    session.characteristic[28] = sizeof service_uuid;
    memcpy(session.characteristic + 32, body + 8, sizeof service_uuid);
    session.characteristic[32 + (forward ? 3 : 12)] = 2;
    session.service_found = true;
}

static void subscribe(void)
{
    uint8_t notify[64] = {session.client};
    memcpy(notify + 4, session.characteristic, sizeof session.characteristic);
    memcpy(notify + 0x38, session.peer, sizeof session.peer);
    if (bsa_command(GATT_REGISTER_NOTIFY, notify, sizeof notify)) {
        fail(EIO, false);
        return;
    }
    memset(session.cccd, 0, sizeof session.cccd);
    store16(session.cccd, session.connection);
    session.cccd[0x38] = 2;
    store16(session.cccd + 0x3a, 2);
    session.cccd[0x3c] = 1;
    memcpy(session.cccd + 0x13c, session.characteristic, sizeof session.characteristic);
    store16(session.cccd + 0x170, 2);
    store16(session.cccd + 0x174, 0x2902);
    session.cccd[0x188] = 1;
    session.notify_enabled = true;
    wait_until(SUBSCRIBING, 5000);
    if (bsa_command(GATT_WRITE, session.cccd, sizeof session.cccd))
        fail(EIO, false);
}

static void connected(uint64_t boottime)
{
    session.phase = STREAMING;
    session.deadline = 0;
    report(GPS_BSA_CONNECTED, 0, boottime, NULL, 0);
}

static void handle(const struct dispatch_event *event)
{
    const uint8_t *body = event->data;
    uint16_t first = load16(body);
    uint16_t second = event->size >= 4 ? load16(body + 2) : 0;
    if (event->id == EVENT_OPEN && session.phase == OPENING && body[4] == session.client &&
        !memcmp(body + 5, session.peer, sizeof session.peer)) {
        if (second) {
            session.connection = second;
            atomic_store_explicit(&observed_connection, second, memory_order_release);
        }
        if (first || !second) {
            fail(first ? first : EPROTO, !first);
            return;
        }
        if (session.stop) {
            close_connection();
            return;
        }
        uint8_t mtu[4] = {0, 0, 247, 0};
        store16(mtu, second);
        wait_until(MTU, 5000);
        if (bsa_command(GATT_MTU, mtu, sizeof mtu))
            fail(EIO, false);
    } else if (event->id == EVENT_CLOSE && session.connection && second == session.connection &&
               body[4] == session.client && !memcmp(body + 5, session.peer, sizeof session.peer)) {
        if (first) {
            session.uncertain = true;
            if (!session.result)
                session.result = first;
        } else if (session.phase != CLOSING && !session.result) {
            session.result = load16(body + 12) ? load16(body + 12) : ECONNRESET;
        }
        session.connection = 0;
        atomic_store_explicit(&observed_connection, 0, memory_order_release);
        deregister();
    } else if (event->id == EVENT_DEREGISTER && session.phase == DEREGISTERING &&
               body[2] == session.client && first == 0) {
        session.client = 0;
        finished();
    } else if (event->id == EVENT_MTU && session.phase == MTU && second == session.connection) {
        if (first || load16(body + 4) < 23) {
            fail(first ? first : EPROTO, false);
            return;
        }
        uint8_t search[24] = {0};
        store16(search, session.connection);
        wait_until(SEARCHING, 5000);
        if (bsa_command(GATT_SEARCH, search, sizeof search))
            fail(EIO, false);
    } else if (event->id == EVENT_SERVICE && session.phase == SEARCHING && first == session.connection) {
        found_service(body);
    } else if (event->id == EVENT_SEARCH && session.phase == SEARCHING && second == session.connection) {
        if (first || !session.service_found)
            fail(first ? first : ENOENT, false);
        else
            subscribe();
    } else if (event->id == EVENT_WRITE && session.phase == SUBSCRIBING && first == session.connection) {
        if (second)
            fail(second, false);
        else
            connected(event->boottime_ns);
    } else if (event->id == EVENT_DATA && (session.phase == SUBSCRIBING || session.phase == STREAMING) &&
               first == session.connection && !memcmp(body + 2, session.peer, sizeof session.peer) &&
               event->size > 0x56) {
        if (session.phase == SUBSCRIBING)
            connected(event->boottime_ns);
        if (session.phase == STREAMING)
            report(GPS_BSA_DATA, 0, event->boottime_ns, body + 0x56, event->size - 0x56);
    }
}

void gps_bsa_tick(void)
{
    struct dispatch_event event;
    for (unsigned i = 0; i < 64; ++i) {
        ssize_t size = recv(events[0], &event, sizeof event, MSG_DONTWAIT);
        if (size < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (size >= (ssize_t)offsetof(struct dispatch_event, data) &&
            size == (ssize_t)(offsetof(struct dispatch_event, data) + event.size) &&
            event.generation == session.generation && session.phase != IDLE && session.phase != FAULTED) {
            if (!event.boottime_ns)
                atomic_store_explicit(&lost_event, true, memory_order_release);
            else
                handle(&event);
        }
    }
    if (atomic_exchange_explicit(&lost_event, false, memory_order_acq_rel) &&
        session.phase != IDLE && session.phase != FAULTED) {
        if (session.phase == CLOSING || session.phase == DEREGISTERING)
            session.uncertain = true;
        else
            fail(EOVERFLOW, true);
    }
    if (!session.deadline || gps_bsa_monotonic_ns() < session.deadline)
        return;
    if (!session.result)
        session.result = ETIMEDOUT;
    switch (session.phase) {
    case OPENING: session.uncertain = true; deregister(); break;
    case CLOSING: session.uncertain = true; deregister(); break;
    case DEREGISTERING: session.uncertain = true; finished(); break;
    default: close_connection(); break;
    }
}
