#include "bsa_transport.h"
#include "gps_dbus.h"
#include "gps_mic.h"
#include "gps_publication.h"
#include "gps_hotshoe.h"
#include "gps_selection.h"

#include <dbus/dbus.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <android/log.h>
#define gps_log(level, ...) __android_log_print(ANDROID_LOG_##level, "splus-gpsd", __VA_ARGS__)
#else
#define gps_log(level, ...) fprintf(stderr, __VA_ARGS__)
#endif

#define GPS_ROOT "/data/hasselblad-gps"
#define GPS_LOCK GPS_ROOT "/run/gps.lock"
#define GPS_ENABLED GPS_ROOT "/enabled"
#define GPS_SYSTEM "com.hasselblad.system"
#define GPS_POWER_PATH "/system"
#define GPS_WAKE_SETTLE_NS (8 * GPS_NS_PER_SECOND)

typedef struct {
    GpsPolicy policy;
    GpsPublication publication;
    GpsSelection selection;
    GpsHotshoe *hotshoe;
    GpsDbus *dbus;
    DBusPendingCall *power_query;
    uint64_t next_power_ns, next_bridge_ns, next_mic_ns, sequence, next_publication_ns, stop_deadline_ns;
    uint64_t bridge_start, candidate_start, bridge_ready_deadline_ns;
    uint64_t confirm_after_ns, last_mic_admission_ns;
    uint64_t suspend_offset_ns, wake_settle_until_ns;
    int bridge;
    pid_t bridge_pid, candidate_pid;
    char power_owner[64];
    bool power_known, suspend_offset_known;
    bool ble_attempt_failed;
    int hotshoe_io_error;
} GpsDaemon;

static volatile sig_atomic_t stopping;

static void on_signal(int signal)
{
    (void)signal;
    stopping = 1;
}

static uint64_t clock_ns(clockid_t clock)
{
    struct timespec t;
    if (clock_gettime(clock, &t) < 0)
        return 0;
    return (uint64_t)t.tv_sec * GPS_NS_PER_SECOND + t.tv_nsec;
}

static uint64_t boottime(void)
{
    return clock_ns(CLOCK_BOOTTIME);
}

static bool process_is(pid_t pid, const char *expected)
{
    if (pid <= 1)
        return false;
    char path[64], exe[64];
    snprintf(path, sizeof(path), "/proc/%d/exe", (int)pid);
    ssize_t length = readlink(path, exe, sizeof(exe) - 1);
    return length > 0 && (size_t)length == strlen(expected) && memcmp(exe, expected, (size_t)length) == 0;
}

static uint64_t process_start(pid_t pid)
{
    char path[64], data[4096];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    FILE *file = fopen(path, "r");
    if (!file)
        return 0;
    bool read = fgets(data, sizeof(data), file) != NULL;
    fclose(file);
    char *fields = read ? strrchr(data, ')') : NULL;
    if (!fields || fields[1] != ' ')
        return 0;
    fields += 2;
    for (unsigned i = 0; i < 19; ++i) {
        fields = strchr(fields, ' ');
        if (!fields)
            return 0;
        ++fields;
    }
    char *end;
    uint64_t start = strtoull(fields, &end, 10);
    return end != fields && *end == ' ' ? start : 0;
}

static bool photo_writer_ready(uint64_t now)
{
    int fd = open(GPS_PHOTO_MAILBOX_PATH, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return false;
    struct stat info;
    size_t size = sizeof(struct gps_photo_mailbox_status);
    void *mapping = MAP_FAILED;
    if (fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == geteuid() && info.st_nlink == 1 &&
        (info.st_mode & 0777) == 0600 && info.st_size >= (off_t)size)
        mapping = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (mapping == MAP_FAILED)
        return false;
    const struct gps_photo_mailbox_status *status = mapping;
    uint64_t magic = __atomic_load_n(&status->magic, __ATOMIC_SEQ_CST);
    uint64_t ready = __atomic_load_n(&status->storage_ready, __ATOMIC_SEQ_CST);
    uint64_t heartbeat = __atomic_load_n(&status->capture_heartbeat_ns, __ATOMIC_SEQ_CST);
    uint64_t capture = __atomic_load_n(&status->capture_pid, __ATOMIC_SEQ_CST);
    uint64_t storage = __atomic_load_n(&status->storage_pid, __ATOMIC_SEQ_CST);
    munmap(mapping, size);
    return magic == GPS_PHOTO_MAILBOX_MAGIC && ready == 1 && heartbeat && heartbeat <= now &&
           now - heartbeat < 3 * GPS_NS_PER_SECOND && capture <= INT32_MAX && storage <= INT32_MAX &&
           process_is((pid_t)capture, "/system/bin/camera-service") &&
           process_is((pid_t)storage, "/system/bin/camera-storage");
}

static void get_status(void *context, GpsDbusStatus *status)
{
    GpsDaemon *daemon = context;
    uint64_t now = boottime();
    gps_dbus_policy_status(status, &daemon->policy, now);
    status->available = daemon->power_known;
    status->hotshoe_active = daemon->hotshoe != NULL;
    status->bluetooth_active = daemon->policy.link != GPS_LINK_IDLE &&
                               daemon->policy.link != GPS_LINK_UNCERTAIN;
    status->pending = !daemon->policy.enabled && daemon->policy.release_requested &&
                      daemon->policy.link != GPS_LINK_UNCERTAIN;
    if (status->pending)
        status->enabled = true;
    if (daemon->policy.enabled && !photo_writer_ready(now))
        status->photo_write_state = "unavailable";
}

static bool save_enabled(bool enabled)
{
    if (!enabled)
        return unlink(GPS_ENABLED) == 0 || errno == ENOENT;
    char temporary[] = GPS_ENABLED ".XXXXXX";
    int fd = mkostemp(temporary, O_CLOEXEC);
    if (fd < 0)
        return false;
    bool written = write(fd, "1\n", 2) == 2 && fsync(fd) == 0;
    if (close(fd) < 0)
        written = false;
    if (written && rename(temporary, GPS_ENABLED) == 0)
        return true;
    unlink(temporary);
    return false;
}

static bool set_enabled(void *context, bool enabled)
{
    GpsDaemon *daemon = context;
    uint64_t now = boottime();
    if (!now || stopping || (daemon->policy.enabled != enabled && !save_enabled(enabled)))
        return false;
    gps_policy_set_enabled(&daemon->policy, enabled, now);
    gps_publication_write(&daemon->publication, &daemon->policy, now);
    daemon->next_power_ns = 0;
    daemon->next_mic_ns = daemon->last_mic_admission_ns = 0;
    return true;
}

static void cancel_power_query(GpsDaemon *daemon)
{
    if (!daemon->power_query)
        return;
    dbus_pending_call_cancel(daemon->power_query);
    dbus_pending_call_unref(daemon->power_query);
    daemon->power_query = NULL;
}

static void require_fresh_mic_status(GpsDaemon *daemon)
{
    daemon->last_mic_admission_ns = daemon->next_mic_ns = 0;
    daemon->policy.conditions.mic_known = false;
    daemon->policy.conditions.mic_direct = false;
    daemon->policy.conditions.mic_scan = false;
}

static DBusHandlerResult power_changed(DBusConnection *connection, DBusMessage *message, void *context)
{
    (void)connection;
    GpsDaemon *daemon = context;
    if (!daemon->power_owner[0] ||
        !dbus_message_is_signal(message, DBUS_INTERFACE_PROPERTIES, "PropertiesChanged") ||
        !dbus_message_has_path(message, GPS_POWER_PATH) ||
        !dbus_message_has_sender(message, daemon->power_owner) ||
        !dbus_message_has_signature(message, "sa{sv}as"))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    DBusMessageIter args, changed;
    const char *interface;
    dbus_message_iter_init(message, &args);
    dbus_message_iter_get_basic(&args, &interface);
    if (strcmp(interface, GPS_SYSTEM))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    dbus_message_iter_next(&args);
    dbus_message_iter_recurse(&args, &changed);
    while (dbus_message_iter_get_arg_type(&changed) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry, value;
        const char *property;
        dbus_message_iter_recurse(&changed, &entry);
        dbus_message_iter_get_basic(&entry, &property);
        if (!strcmp(property, "system_state")) {
            dbus_message_iter_next(&entry);
            dbus_message_iter_recurse(&entry, &value);
            if (dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_INT32) {
                dbus_int32_t state;
                dbus_message_iter_get_basic(&value, &state);
                cancel_power_query(daemon);
                daemon->next_power_ns = 0;
                if (state != 1) {
                    if (daemon->policy.conditions.awake)
                        gps_log(INFO, "System power state %d", state);
                    daemon->power_known = true;
                    daemon->policy.conditions.awake = false;
                    require_fresh_mic_status(daemon);
                }
            }
            break;
        }
        dbus_message_iter_next(&changed);
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static bool watch_power(GpsDaemon *daemon)
{
    DBusConnection *connection = gps_dbus_connection(daemon->dbus);
    if (!dbus_connection_add_filter(connection, power_changed, daemon, NULL))
        return false;
    DBusError error = DBUS_ERROR_INIT;
    dbus_bus_add_match(connection, "type='signal',sender='" GPS_SYSTEM "',path='" GPS_POWER_PATH
                       "',interface='" DBUS_INTERFACE_PROPERTIES "',member='PropertiesChanged',arg0='" GPS_SYSTEM "'",
                       &error);
    bool matched = !dbus_error_is_set(&error);
    dbus_error_free(&error);
    return matched;
}

static void power_reply(GpsDaemon *daemon)
{
    DBusMessage *reply = dbus_pending_call_steal_reply(daemon->power_query);
    dbus_pending_call_unref(daemon->power_query);
    daemon->power_query = NULL;
    daemon->power_known = false;
    daemon->power_owner[0] = '\0';
    if (reply && dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_METHOD_RETURN) {
        DBusMessageIter root, value;
        const char *sender = dbus_message_get_sender(reply);
        if (sender && sender[0] == ':' && strlen(sender) < sizeof daemon->power_owner &&
            dbus_message_iter_init(reply, &root) && dbus_message_iter_get_arg_type(&root) == DBUS_TYPE_VARIANT) {
            dbus_message_iter_recurse(&root, &value);
            if (dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_INT32) {
                dbus_int32_t state;
                dbus_message_iter_get_basic(&value, &state);
                strcpy(daemon->power_owner, sender);
                daemon->power_known = true;
                daemon->policy.conditions.awake = state == 1;
            }
        }
    }
    if (reply)
        dbus_message_unref(reply);
    if (!daemon->power_known)
        daemon->policy.conditions.awake = false;
    if (!daemon->policy.conditions.awake)
        require_fresh_mic_status(daemon);
}

static void poll_power(GpsDaemon *daemon, uint64_t now)
{
    if (daemon->power_query) {
        if (!dbus_pending_call_get_completed(daemon->power_query))
            return;
        power_reply(daemon);
    }
    if (now < daemon->next_power_ns)
        return;
    daemon->next_power_ns = now + (daemon->policy.enabled && daemon->policy.conditions.awake ?
                                  GPS_NS_PER_SECOND : 3 * GPS_NS_PER_SECOND);
    const char *interface = GPS_SYSTEM, *property = "system_state";
    DBusMessage *request = dbus_message_new_method_call(interface, GPS_POWER_PATH, DBUS_INTERFACE_PROPERTIES, "Get");
    if (!request)
        return;
    if (dbus_message_append_args(request, DBUS_TYPE_STRING, &interface, DBUS_TYPE_STRING, &property, DBUS_TYPE_INVALID))
        dbus_connection_send_with_reply(gps_dbus_connection(daemon->dbus), request, &daemon->power_query, 650);
    dbus_message_unref(request);
}

static void observe_suspend(GpsDaemon *daemon)
{
    uint64_t before = clock_ns(CLOCK_MONOTONIC);
    uint64_t now = boottime();
    uint64_t after = clock_ns(CLOCK_MONOTONIC);
    if (!before || after < before || after - before > GPS_NS_PER_SECOND / 100 || !now)
        return;
    uint64_t offset = now > after ? now - after : 0;
    /* A deep sleep can happen entirely between two system-state queries. */
    if (daemon->suspend_offset_known && offset > daemon->suspend_offset_ns + GPS_NS_PER_SECOND / 20) {
        gps_log(INFO, "System resumed after %llu ms suspended",
                (unsigned long long)((offset - daemon->suspend_offset_ns) / 1000000));
        cancel_power_query(daemon);
        daemon->next_power_ns = 0;
        daemon->policy.conditions.awake = false;
        daemon->wake_settle_until_ns = now + GPS_WAKE_SETTLE_NS;
        require_fresh_mic_status(daemon);
    }
    daemon->suspend_offset_ns = offset;
    daemon->suspend_offset_known = true;
}

static void disconnect_bridge(GpsDaemon *daemon)
{
    if (daemon->bridge >= 0)
        close(daemon->bridge);
    daemon->bridge = -1;
    daemon->bridge_ready_deadline_ns = 0;
    daemon->stop_deadline_ns = 0;
    daemon->confirm_after_ns = 0;
    daemon->next_mic_ns = 0;
    gps_policy_bridge_lost(&daemon->policy);
}

static bool bridge_peer(int fd, pid_t *pid, uint64_t *start)
{
    struct ucred peer;
    socklen_t size = sizeof(peer);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) < 0 || size != sizeof(peer) ||
        peer.uid != geteuid() || peer.pid <= 1)
        return false;
    uint64_t identity = process_start(peer.pid);
    if (!identity || !process_is(peer.pid, "/system/bin/bt_bsa_app") || process_start(peer.pid) != identity)
        return false;
    *pid = peer.pid;
    *start = identity;
    return true;
}

static void connect_bridge(GpsDaemon *daemon, uint64_t now)
{
    if (daemon->bridge >= 0 || now < daemon->next_bridge_ns || !daemon->policy.enabled ||
        !daemon->policy.conditions.awake || stopping ||
        (daemon->policy.source != GPS_SOURCE_BLE && daemon->policy.link != GPS_LINK_UNCERTAIN))
        return;
    daemon->next_bridge_ns = now + 2 * GPS_NS_PER_SECOND;
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return;
    const struct sockaddr_un address = {.sun_family = AF_UNIX, .sun_path = GPS_BSA_SOCKET};
    pid_t pid;
    uint64_t start;
    /* The verified resident sends READY only after its session is inactive and
     * clean. A clean reconnect can be acknowledged by the same process too. */
    if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) < 0 || !bridge_peer(fd, &pid, &start)) {
        close(fd);
        return;
    }
    daemon->candidate_pid = pid;
    daemon->candidate_start = start;
    daemon->bridge = fd;
    daemon->bridge_ready_deadline_ns = now + GPS_NS_PER_SECOND;
}

static bool send_action(GpsDaemon *daemon, GpsAction action)
{
    const uint8_t peer[] = {0x14, 0xc1, 0x9f, 0x2e, 0xbb, 0xa9};
    struct gps_bsa_message message = {.version = GPS_BSA_VERSION, .sequence = daemon->sequence};
    if (action == GPS_ACTION_NONE)
        return true;
    if (action == GPS_ACTION_OPEN || action == GPS_ACTION_DISCOVER) {
        message.sequence = ++daemon->sequence;
        message.type = GPS_BSA_START;
        message.status = action == GPS_ACTION_DISCOVER ? GPS_BSA_DISCOVER : 0;
        message.size = sizeof(peer);
        memcpy(message.data, peer, sizeof(peer));
    } else if (action == GPS_ACTION_CONFIRM) {
        message.type = GPS_BSA_CONFIRM;
    } else {
        message.type = GPS_BSA_STOP;
        daemon->stop_deadline_ns = clock_ns(CLOCK_MONOTONIC) + 12 * GPS_NS_PER_SECOND;
        if (!daemon->policy.conditions.awake)
            gps_log(INFO, "Stopping GPS Bluetooth for standby, session %llu",
                    (unsigned long long)message.sequence);
    }
    bool sent = send(daemon->bridge, &message, sizeof(message), MSG_NOSIGNAL) == sizeof(message);
    if (sent && action == GPS_ACTION_DISCOVER)
        gps_log(INFO, "GPS BLE discovery sent, session %llu", (unsigned long long)message.sequence);
    return sent;
}

static bool receive_bridge(GpsDaemon *daemon)
{
    for (unsigned i = 0; i < 128; ++i) {
        struct gps_bsa_message message;
        ssize_t received = recv(daemon->bridge, &message, sizeof(message), MSG_TRUNC);
        if (received < 0 && errno == EINTR)
            continue;
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return true;
        uint64_t now = boottime();
        if (received != sizeof(message) || message.version != GPS_BSA_VERSION ||
            message.size > sizeof(message.data) ||
            message.boottime_ns > now)
            return false;
        if (!daemon->policy.conditions.bridge_available) {
            if (message.type != GPS_BSA_READY || message.sequence || message.status || message.size)
                return false;
            if (daemon->policy.link == GPS_LINK_UNCERTAIN)
                gps_policy_stopped(&daemon->policy, 0, now);
            daemon->bridge_pid = daemon->candidate_pid;
            daemon->bridge_start = daemon->candidate_start;
            require_fresh_mic_status(daemon);
            daemon->policy.conditions.bridge_available = true;
            daemon->bridge_ready_deadline_ns = daemon->next_mic_ns = 0;
            continue;
        }
        if (!daemon->sequence || message.sequence != daemon->sequence)
            return false;
        switch (message.type) {
        case GPS_BSA_DISCOVERED:
            daemon->confirm_after_ns = message.boottime_ns + UINT64_C(350000000);
            break;
        case GPS_BSA_CONNECTED:
            gps_policy_connected(&daemon->policy, message.boottime_ns);
            break;
        case GPS_BSA_DATA:
            gps_policy_receive(&daemon->policy, message.data, message.size, message.boottime_ns);
            daemon->next_publication_ns = 0;
            break;
        case GPS_BSA_STOPPED:
        {
            GpsLinkState previous = daemon->policy.link;
            if (daemon->policy.link == GPS_LINK_STOPPING)
                gps_log(INFO, "GPS Bluetooth session %llu released, status %u",
                        (unsigned long long)message.sequence, message.status);
            else
                gps_log(INFO, "GPS Bluetooth session %llu ended, link %u status %u",
                        (unsigned long long)message.sequence, (unsigned)previous, message.status);
            gps_policy_stopped(&daemon->policy, message.status, now);
            if (daemon->policy.source == GPS_SOURCE_BLE &&
                (previous == GPS_LINK_DISCOVERING || previous == GPS_LINK_OPENING) &&
                !(previous == GPS_LINK_OPENING && message.status == 133 &&
                  daemon->policy.conditions.mic_direct))
                daemon->ble_attempt_failed = true;
            daemon->stop_deadline_ns = 0;
            daemon->confirm_after_ns = 0;
            break;
        }
        case GPS_BSA_FAULT:
            gps_log(ERROR, "GPS Bluetooth session %llu failed with status %u",
                    (unsigned long long)message.sequence, message.status);
            daemon->policy.link = GPS_LINK_UNCERTAIN;
            return false;
        default:
            return false;
        }
    }
    return true;
}

static void receive_hotshoe(void *context, uint8_t byte, uint64_t received_ns, bool discontinuity)
{
    GpsDaemon *daemon = context;
    uint64_t navigation = daemon->policy.last_navigation_ns;
    uint64_t fix = daemon->policy.last_fix_ns;
    gps_policy_receive_hotshoe(&daemon->policy, byte, received_ns, discontinuity);
    if (navigation != daemon->policy.last_navigation_ns || fix != daemon->policy.last_fix_ns)
        daemon->next_publication_ns = 0;
}

static bool ble_attempt_allowed(const GpsPolicy *policy)
{
    return policy->conditions.bridge_available && policy->conditions.mic_known && policy->conditions.mic_direct;
}

static const char *hotshoe_close_reason(const GpsSelectionInput *input, GpsSource source,
                                        GpsSelectionPhase phase)
{
    if (!input->enabled) return "disabled";
    if (!input->awake) return "standby";
    if (input->video) return "video";
    if (source != GPS_SOURCE_HOTSHOE) return "source-change";
    if (input->hotshoe_io_failed) return "io-error";
    return phase == GPS_SELECTION_HOTSHOE_PROBE ? "probe-timeout" : "byte-timeout";
}

static int64_t hotshoe_activity_age_ms(uint64_t stamp, uint64_t confirmed, uint64_t now)
{
    if (stamp > now) stamp = 0;
    if (confirmed <= now && confirmed > stamp) stamp = confirmed;
    return stamp ? (int64_t)((now - stamp) / UINT64_C(1000000)) : -1;
}

static void close_hotshoe(GpsDaemon *daemon, GpsHotshoeStats stats, uint64_t confirmed,
                           uint64_t now, const char *reason)
{
    if (!daemon->hotshoe)
        return;
    gps_log(INFO, "Closing hot-shoe GPS reason=%s age_ms=%lld errno=%d clock_resets=%llu "
            "overflows=%llu bytes=%llu last_byte_ns=%llu confirmed_ns=%llu now_ns=%llu",
            reason, (long long)hotshoe_activity_age_ms(stats.last_byte_ns, confirmed, now),
            daemon->hotshoe_io_error ? daemon->hotshoe_io_error : stats.io_error,
            (unsigned long long)stats.clock_resets, (unsigned long long)stats.overflows,
            (unsigned long long)stats.bytes, (unsigned long long)stats.last_byte_ns,
            (unsigned long long)confirmed, (unsigned long long)now);
    gps_hotshoe_close(daemon->hotshoe);
    daemon->hotshoe = NULL;
    daemon->hotshoe_io_error = 0;
}

static void select_source(GpsDaemon *daemon, uint64_t now)
{
    GpsPolicy *policy = &daemon->policy;
    GpsHotshoeStats stats = gps_hotshoe_stats(daemon->hotshoe);
    /* The reader can advance last_byte_ns while acquiring the stats lock. */
    now = boottime();
    if (!now)
        return;
    GpsSelectionInput input = {
        .enabled = policy->enabled, .awake = policy->conditions.awake,
        .video = policy->conditions.video,
        .ble_allowed = ble_attempt_allowed(policy),
        .ble_mic_busy = policy->conditions.bridge_available && policy->conditions.mic_known &&
                        !policy->conditions.mic_direct,
        .ble_failed = daemon->ble_attempt_failed, .ble_link = policy->link,
        .hotshoe_open = daemon->hotshoe != NULL,
        .hotshoe_navigation = policy->source == GPS_SOURCE_HOTSHOE &&
                              policy->last_navigation_ns >= daemon->selection.started_ns &&
                              policy->last_navigation_ns != 0,
        .hotshoe_io_failed = daemon->hotshoe_io_error || stats.io_error,
        .hotshoe_last_byte_ns = stats.last_byte_ns,
    };
    GpsSource previous = policy->source;
    GpsSource selection_source = daemon->selection.source;
    GpsSelectionPhase phase = daemon->selection.phase;
    uint64_t deadline = daemon->selection.deadline_ns;
    uint64_t confirmed = daemon->selection.hotshoe_last_confirmed_ns;
    GpsSelectionAction action = gps_selection_step(&daemon->selection, &input, now);
    if (daemon->selection.phase == GPS_SELECTION_WAIT_BLE_ADMISSION && input.ble_mic_busy &&
        daemon->selection.deadline_ns != deadline)
        gps_log(INFO, "GPS BLE waiting for Mic admission, link %u", (unsigned)input.ble_link);
    if (phase == GPS_SELECTION_WAIT_BLE_ADMISSION && daemon->selection.phase == GPS_SELECTION_TRY_BLE)
        gps_log(INFO, "GPS BLE admitted after %llu ms",
                (unsigned long long)((now - daemon->selection.started_ns) / UINT64_C(1000000)));
    gps_policy_set_source(policy, daemon->selection.source, now);
    switch (action) {
    case GPS_SELECT_START_BLE:
        daemon->ble_attempt_failed = false;
        /* An absent peer can leave the vendor OPEN pending past our deadline.
         * Prove presence by discovery before allocating another GATT client. */
        policy->needs_discovery = true;
        policy->retry_ns = 0;
        policy->retry_seconds = 2;
        daemon->next_bridge_ns = daemon->next_mic_ns = 0;
        break;
    case GPS_SELECT_STOP_BLE:
        policy->release_requested = true;
        break;
    case GPS_SELECT_OPEN_HOTSHOE:
        daemon->hotshoe_io_error = 0;
        daemon->hotshoe = gps_hotshoe_open();
        if (!daemon->hotshoe) {
            daemon->hotshoe_io_error = errno ? errno : EIO;
            gps_log(WARN, "Hot-shoe GPS receiver open failed: %d", daemon->hotshoe_io_error);
        }
        break;
    case GPS_SELECT_CLOSE_HOTSHOE:
        close_hotshoe(daemon, stats, confirmed, now, hotshoe_close_reason(&input, selection_source, phase));
        break;
    case GPS_SELECT_NONE:
        break;
    }
    if (previous != policy->source || action != GPS_SELECT_NONE) {
        gps_log(INFO, "GPS source %s selection %u action %u", gps_source_name(policy->source),
                (unsigned)daemon->selection.phase, (unsigned)action);
        daemon->next_publication_ns = 0;
    }
}

static int lock_daemon(void)
{
    int fd = open(GPS_LOCK, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool daemon_step(GpsDaemon *daemon, uint64_t now)
{
    observe_suspend(daemon);
    if (!gps_dbus_dispatch(daemon->dbus, 0))
        return false;
    poll_power(daemon, now);
    connect_bridge(daemon, now);
    if (daemon->bridge >= 0 && !receive_bridge(daemon))
        disconnect_bridge(daemon);
    if (daemon->hotshoe && gps_hotshoe_drain(daemon->hotshoe, receive_hotshoe, daemon) < 0)
        daemon->hotshoe_io_error = errno ? errno : EIO;
    if (daemon->confirm_after_ns && now >= daemon->confirm_after_ns) {
        daemon->confirm_after_ns = daemon->next_mic_ns = 0;
        gps_policy_discovered(&daemon->policy);
    }
    if (now >= daemon->wake_settle_until_ns && daemon->bridge_pid > 1 &&
        daemon->policy.enabled && daemon->policy.conditions.awake &&
        now >= daemon->next_mic_ns) {
        GpsMicStatus status = gps_mic_status(daemon->bridge_pid);
        now = boottime();
        gps_mic_apply(&daemon->policy, &daemon->last_mic_admission_ns, status, now);
        daemon->next_mic_ns = now + GPS_NS_PER_SECOND / 2;
    }
    now = boottime();
    if (daemon->bridge_ready_deadline_ns && now >= daemon->bridge_ready_deadline_ns)
        disconnect_bridge(daemon);
    if (daemon->stop_deadline_ns && clock_ns(CLOCK_MONOTONIC) >= daemon->stop_deadline_ns) {
        daemon->stop_deadline_ns = 0;
        disconnect_bridge(daemon);
    }
    select_source(daemon, now);
    const GpsFix *last = gps_fix_cache_latest(&daemon->policy.cache);
    if (gps_selection_cache_expired(&daemon->selection, now, last ? last->received_ns : 0,
                                    daemon->policy.enabled_since_ns) && set_enabled(daemon, false)) {
        gps_log(INFO, "GPS disabled after one hour without an available source");
        select_source(daemon, now);
    }
    GpsAction action = gps_policy_step(&daemon->policy, now);
    if (!send_action(daemon, action))
        disconnect_bridge(daemon);
    if (action != GPS_ACTION_NONE || now >= daemon->next_publication_ns) {
        gps_publication_write(&daemon->publication, &daemon->policy, now);
        daemon->next_publication_ns = now + GPS_NS_PER_SECOND;
    }
    bool active = daemon->policy.link != GPS_LINK_IDLE && daemon->policy.link != GPS_LINK_UNCERTAIN;
    if (!active && daemon->bridge >= 0 && (!daemon->policy.enabled || !daemon->policy.conditions.awake ||
                                         daemon->policy.source != GPS_SOURCE_BLE)) {
        close(daemon->bridge);
        daemon->bridge = -1;
        daemon->bridge_ready_deadline_ns = 0;
        daemon->last_mic_admission_ns = 0;
        daemon->policy.conditions.bridge_available = false;
    }
    struct pollfd ready[] = {{.fd = gps_dbus_fd(daemon->dbus), .events = POLLIN},
                             {.fd = daemon->bridge, .events = POLLIN},
                             {.fd = gps_hotshoe_fd(daemon->hotshoe), .events = POLLIN}};
    int result = poll(ready, 3, active || daemon->hotshoe || daemon->power_query ? 100 : 1000);
    return result >= 0 || errno == EINTR;
}

int main(void)
{
    if (getuid() != 0)
        return 1;
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);
    int lock = lock_daemon();
    if (lock < 0)
        return 1;
    GpsDaemon daemon = {.bridge = -1};
    uint64_t now = boottime();
    gps_policy_init(&daemon.policy, access(GPS_ENABLED, F_OK) == 0, now);
    gps_selection_init(&daemon.selection);
    gps_policy_set_source(&daemon.policy, GPS_SOURCE_NONE, now);
    if (!gps_publication_open(&daemon.publication, GPS_PHOTO_CACHE_PATH, now)) {
        close(lock);
        return 1;
    }
    const GpsDbusCallbacks callbacks = {.get_status = get_status, .set_enabled = set_enabled};
    daemon.dbus = gps_dbus_open(&callbacks, &daemon);
    int result = daemon.dbus && watch_power(&daemon) ? 0 : 1;
    uint64_t shutdown_deadline = 0;
    while (!result && (now = boottime()) != 0) {
        if (stopping && !shutdown_deadline) {
            shutdown_deadline = now + 12 * GPS_NS_PER_SECOND;
            gps_policy_set_enabled(&daemon.policy, false, now);
            gps_publication_write(&daemon.publication, &daemon.policy, now);
        }
        if (shutdown_deadline && (now >= shutdown_deadline || daemon.policy.link == GPS_LINK_IDLE ||
                                 daemon.policy.link == GPS_LINK_UNCERTAIN))
            break;
        if (!daemon_step(&daemon, now)) {
            result = 1;
            break;
        }
    }
    disconnect_bridge(&daemon);
    close_hotshoe(&daemon, gps_hotshoe_stats(daemon.hotshoe), daemon.selection.hotshoe_last_confirmed_ns,
                   boottime(), "shutdown");
    cancel_power_query(&daemon);
    if (daemon.dbus)
        gps_dbus_close(daemon.dbus);
    gps_publication_close(&daemon.publication);
    close(lock);
    return result;
}
