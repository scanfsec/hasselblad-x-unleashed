#define _GNU_SOURCE
#include "bsa_bridge.h"
#include "bsa_transport.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define DISCOVERY_SOCKET "/data/misc/bluetooth/bt-daemon-socket"

static struct {
    int listen_fd, client_fd, discovery_fd;
    uid_t daemon_uid;
    uint64_t sequence, discovery_deadline;
    uint8_t peer[6], discovery_bytes[2048];
    size_t discovery_used;
    bool active, faulted, discovery_connecting, discovery_accepted, discovery_finished, discovery_found;
} bridge = {.listen_fd = -1, .client_fd = -1, .discovery_fd = -1};
static atomic_bool started;

static uint16_t load16(const uint8_t *data)
{
    return (uint16_t)(data[0] | (uint16_t)data[1] << 8);
}

static void close_discovery(void)
{
    if (bridge.discovery_fd >= 0)
        close(bridge.discovery_fd);
    bridge.discovery_fd = -1;
}

static void drop_client(void)
{
    if (bridge.client_fd >= 0)
        close(bridge.client_fd);
    bridge.client_fd = -1;
    if (!bridge.active)
        return;
    if (bridge.discovery_deadline) {
        close_discovery();
        bridge.discovery_deadline = 0;
        bridge.active = false;
    } else {
        gps_bsa_stop();
    }
}

static void send_event(void *context, const struct gps_bsa_message *message)
{
    (void)context;
    if (message->type == GPS_BSA_STOPPED || message->type == GPS_BSA_FAULT) {
        bridge.active = false;
        bridge.faulted |= message->type == GPS_BSA_FAULT;
    }
    if (bridge.client_fd >= 0 && send(bridge.client_fd, message, sizeof *message, MSG_NOSIGNAL) != sizeof *message)
        drop_client();
}

static void state_event(uint32_t type, uint32_t status)
{
    const struct gps_bsa_message message = {.version = GPS_BSA_VERSION, .type = type,
                                           .sequence = bridge.sequence, .status = status,
                                           .boottime_ns = gps_bsa_boottime_ns()};
    send_event(NULL, &message);
}

static void cancel_discovery(uint32_t reason)
{
    close_discovery();
    bridge.discovery_deadline = 0;
    state_event(GPS_BSA_STOPPED, reason);
}

static int send_scan(void)
{
    /* Dedicated LE inquiry. Closing this socket cancels only this inquiry
     * on the qualified server. Never send global discovery abort. */
    const uint8_t request[44] = {[0] = 42, [2] = 0xc8, [4] = 3,
                                 [36] = 0x10, [39] = 1, [40] = 1};
    return send(bridge.discovery_fd, request, sizeof request, MSG_NOSIGNAL) == sizeof request ? 0 : -1;
}

static void start_discovery(void)
{
    bridge.discovery_deadline = gps_bsa_monotonic_ns() + UINT64_C(7500000000);
    bridge.discovery_used = 0;
    bridge.discovery_accepted = bridge.discovery_finished = bridge.discovery_found = false;
    bridge.discovery_connecting = false;
    bridge.discovery_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (bridge.discovery_fd < 0) {
        cancel_discovery(errno);
        return;
    }
    struct sockaddr_un target = {.sun_family = AF_UNIX, .sun_path = DISCOVERY_SOCKET};
    if (connect(bridge.discovery_fd, (void *)&target, sizeof target) < 0) {
        if (errno == EINPROGRESS) {
            bridge.discovery_connecting = true;
            return;
        }
        cancel_discovery(errno);
        return;
    }
    if (send_scan() < 0)
        cancel_discovery(EIO);
}

static int discovery_record(uint16_t type, const uint8_t *body, size_t size)
{
    switch (type) {
    case 0x80c8:
        if (size != 2 || bridge.discovery_accepted)
            return EPROTO;
        if (load16(body))
            return load16(body);
        bridge.discovery_accepted = true;
        break;
    case 0x40cb:
        if (size != 0x20c || bridge.discovery_finished)
            return EPROTO;
        if (!memcmp(bridge.peer, body, sizeof bridge.peer))
            bridge.discovery_found = true;
        break;
    case 0x40cc:
        if (size || bridge.discovery_finished)
            return EPROTO;
        bridge.discovery_finished = true;
        break;
    }
    return 0;
}

static void scan_io(short revents)
{
    if (bridge.discovery_connecting && (revents & POLLOUT)) {
        int error;
        socklen_t size = sizeof error;
        bridge.discovery_connecting = false;
        if (getsockopt(bridge.discovery_fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0 || error || send_scan() < 0) {
            cancel_discovery(EIO);
            return;
        }
    }
    if (revents & POLLIN) {
        uint8_t *bytes = bridge.discovery_bytes;
        ssize_t got = recv(bridge.discovery_fd, bytes + bridge.discovery_used,
                           sizeof bridge.discovery_bytes - bridge.discovery_used, MSG_DONTWAIT);
        if (got <= 0) {
            if (!got || (errno != EAGAIN && errno != EINTR))
                cancel_discovery(EIO);
            return;
        }
        bridge.discovery_used += (size_t)got;
        size_t consumed = 0;
        while (bridge.discovery_used - consumed >= 4) {
            const uint8_t *record = bytes + consumed;
            size_t length = load16(record);
            if (length < 2 || length + 2 > sizeof bridge.discovery_bytes) {
                cancel_discovery(EPROTO);
                return;
            }
            if (bridge.discovery_used - consumed < length + 2)
                break;
            int error = discovery_record(load16(record + 2), record + 4, length - 2);
            if (error) {
                cancel_discovery((uint32_t)error);
                return;
            }
            consumed += length + 2;
        }
        bridge.discovery_used -= consumed;
        memmove(bytes, bytes + consumed, bridge.discovery_used);
        if (bridge.discovery_accepted && bridge.discovery_finished) {
            if (!bridge.discovery_found) {
                cancel_discovery(ENOENT);
                return;
            }
            close_discovery();
            bridge.discovery_deadline = gps_bsa_monotonic_ns() + UINT64_C(2500000000);
            state_event(GPS_BSA_DISCOVERED, 0);
            return;
        }
    }
    if (revents & (POLLERR | POLLHUP | POLLNVAL))
        cancel_discovery(EIO);
}

static void accept_client(void)
{
    int fd = accept4(bridge.listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd < 0)
        return;
    struct ucred peer;
    socklen_t size = sizeof peer;
    if (bridge.client_fd >= 0 || bridge.active || bridge.faulted ||
        getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) < 0 || size != sizeof peer ||
        peer.uid != bridge.daemon_uid) {
        close(fd);
        return;
    }
    bridge.client_fd = fd;
    const struct gps_bsa_message ready = {.version = GPS_BSA_VERSION, .type = GPS_BSA_READY,
                                         .boottime_ns = gps_bsa_boottime_ns()};
    send_event(NULL, &ready);
}

static void start_link(void)
{
    if (gps_bsa_start(bridge.sequence, bridge.peer) < 0)
        state_event(GPS_BSA_FAULT, EBUSY);
}

static bool valid_peer(const uint8_t *peer)
{
    static const uint8_t zero[6] = {0};
    static const uint8_t broadcast[6] = {255, 255, 255, 255, 255, 255};
    return memcmp(peer, zero, 6) && memcmp(peer, broadcast, 6);
}

static void control(void)
{
    static const uint8_t zero[GPS_BSA_DATA_MAX] = {0};
    struct gps_bsa_message message;
    ssize_t size = recv(bridge.client_fd, &message, sizeof message, MSG_DONTWAIT | MSG_TRUNC);
    if (size < 0 && (errno == EAGAIN || errno == EINTR))
        return;
    if (size != sizeof message || message.version != GPS_BSA_VERSION || !message.sequence ||
        message.boottime_ns || memcmp(message.reserved, zero, sizeof message.reserved) ||
        message.size > sizeof message.data || memcmp(message.data + message.size, zero, sizeof message.data - message.size)) {
        drop_client();
        return;
    }
    if (message.type == GPS_BSA_START && message.size == sizeof bridge.peer && valid_peer(message.data) &&
        !(message.status & ~GPS_BSA_DISCOVER) && !bridge.active) {
        bridge.sequence = message.sequence;
        if (bridge.faulted) {
            state_event(GPS_BSA_FAULT, EBUSY);
            return;
        }
        bridge.active = true;
        memcpy(bridge.peer, message.data, sizeof bridge.peer);
        if (message.status & GPS_BSA_DISCOVER)
            start_discovery();
        else
            start_link();
        return;
    }
    if (message.sequence != bridge.sequence || message.size || message.status) {
        drop_client();
        return;
    }
    if (message.type == GPS_BSA_STOP) {
        if (bridge.discovery_deadline)
            cancel_discovery(0);
        else if (bridge.active)
            gps_bsa_stop();
        return;
    }
    if (message.type == GPS_BSA_CONFIRM && bridge.active && bridge.discovery_deadline && bridge.discovery_fd < 0 &&
        gps_bsa_monotonic_ns() < bridge.discovery_deadline) {
        bridge.discovery_deadline = 0;
        start_link();
        return;
    }
    drop_client();
}

static int poll_timeout(void)
{
    uint64_t deadline = gps_bsa_deadline_ns();
    if (bridge.discovery_deadline && (!deadline || bridge.discovery_deadline < deadline))
        deadline = bridge.discovery_deadline;
    if (!deadline)
        return -1;
    uint64_t now = gps_bsa_monotonic_ns();
    if (now >= deadline)
        return 0;
    uint64_t milliseconds = (deadline - now + 999999) / 1000000;
    return milliseconds > INT_MAX ? INT_MAX : (int)milliseconds;
}

static void *worker(void *unused)
{
    (void)unused;
    for (;;) {
        struct pollfd fds[] = {{bridge.listen_fd, POLLIN, 0}, {bridge.client_fd, POLLIN, 0},
                               {gps_bsa_event_fd(), POLLIN, 0},
                               {bridge.discovery_fd, POLLIN | (bridge.discovery_connecting ? POLLOUT : 0), 0}};
        if (poll(fds, sizeof fds / sizeof *fds, poll_timeout()) < 0)
            continue;
        if (fds[1].revents & POLLIN)
            control();
        if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL))
            drop_client();
        if (fds[0].revents & POLLIN)
            accept_client();
        if (bridge.discovery_fd >= 0 && fds[3].fd == bridge.discovery_fd && fds[3].revents)
            scan_io(fds[3].revents);
        if (bridge.discovery_deadline && gps_bsa_monotonic_ns() >= bridge.discovery_deadline)
            cancel_discovery(ETIMEDOUT);
        if (bridge.active || fds[2].revents)
            gps_bsa_tick();
    }
    return NULL;
}

int gps_bsa_bridge_start(const char *socket_path, uid_t daemon_uid)
{
    struct sockaddr_un endpoint = {.sun_family = AF_UNIX};
    if (!socket_path || strlen(socket_path) >= sizeof endpoint.sun_path ||
        atomic_exchange_explicit(&started, true, memory_order_acq_rel))
        return -1;
    strcpy(endpoint.sun_path, socket_path);
    bridge.daemon_uid = daemon_uid;
    bridge.listen_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (bridge.listen_fd < 0)
        return -1;
    /* Do not remove an existing socket belonging to another live bridge. The
     * installer/supervisor may clear a stale endpoint after proving owner exit. */
    if (bind(bridge.listen_fd, (void *)&endpoint, sizeof endpoint) < 0) {
        close(bridge.listen_fd);
        bridge.listen_fd = -1;
        return -1;
    }
    pthread_t thread;
    if (chmod(socket_path, 0600) < 0 || listen(bridge.listen_fd, 2) < 0 ||
        gps_bsa_install(send_event, NULL) || pthread_create(&thread, NULL, worker, NULL)) {
        close(bridge.listen_fd);
        bridge.listen_fd = -1;
        unlink(socket_path);
        return -1;
    }
    pthread_detach(thread);
    return 0;
}
