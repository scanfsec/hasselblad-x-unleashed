#include "gps_mic.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MIC_SOCKET "/data/mic/mic_ctl"
#define MIC_REPLY_NS UINT64_C(150000000)
#define MIC_STREAM_GRACE_NS UINT64_C(1500000000)

typedef struct { const char *begin, *end; } Span;

static const char *space(const char *p)
{
    return p + strspn(p, " \t\r\n");
}

static const char *end_string(const char *p)
{
    for (++p; *p; ++p) {
        if (*p == '"')
            return p + 1;
        if (*p == '\\' && *++p == '\0')
            break;
    }
    return NULL;
}

static const char *end_value(const char *p, unsigned depth)
{
    if (depth > 12 || !*p)
        return NULL;
    if (*p == '"')
        return end_string(p);
    if (*p != '{' && *p != '[')
        return p + strcspn(p, ",}] \t\r\n");
    bool object = *p == '{';
    char end = object ? '}' : ']';
    p = space(p + 1);
    while (*p != end) {
        if (object) {
            if (*p != '"' || !(p = end_string(p)) || *space(p) != ':')
                return NULL;
            p = space(space(p) + 1);
        }
        const char *next = end_value(p, depth + 1);
        if (!next || next == p)
            return NULL;
        p = space(next);
        if (*p != ',')
            return *p == end ? p + 1 : NULL;
        p = space(p + 1);
    }
    return p + 1;
}

static bool equal(Span span, const char *word)
{
    return span.begin && (size_t)(span.end - span.begin) == strlen(word) &&
           memcmp(span.begin, word, (size_t)(span.end - span.begin)) == 0;
}

static Span field(const char *object, const char *key)
{
    if (*object != '{')
        return (Span){0};
    Span match = {0};
    const char *p = space(object + 1);
    while (*p == '"') {
        const char *end_key = end_string(p);
        if (!end_key || *space(end_key) != ':')
            return (Span){0};
        const char *value = space(space(end_key) + 1), *end = end_value(value, 0);
        if (!end || end == value)
            return (Span){0};
        if (equal((Span){p + 1, end_key - 1}, key)) {
            if (match.begin)
                return (Span){0};
            match = (Span){value, end};
        }
        p = space(end);
        if (*p != ',')
            break;
        p = space(p + 1);
    }
    return match;
}

GpsMicStatus gps_mic_parse_status(const char *line, pid_t pid)
{
    line = space(line);
    const char *end = end_value(line, 0);
    if (!end || *space(end) || !equal(field(line, "tx_status_api"), "1"))
        return GPS_MIC_UNAVAILABLE;
    Span transmitters = field(line, "transmitters");
    if (!transmitters.begin || *transmitters.begin != '[')
        return GPS_MIC_UNAVAILABLE;
    const char *p = space(transmitters.begin + 1);
    bool quiet = true, blocked = false, recording = false;
    char pid_text[24];
    snprintf(pid_text, sizeof(pid_text), "%d", (int)pid);
    for (unsigned i = 0; i < 2; ++i) {
        const char *object_end = end_value(p, 0);
        if (!object_end || *p != '{' || !equal(field(p, "engine_pid"), pid_text))
            return GPS_MIC_BLOCKED;
        if (!equal(field(p, "engine_ready"), "true") || !equal(field(p, "connection_queued"), "false") ||
            !equal(field(p, "retrying"), "false"))
            blocked = true;
        Span local = field(p, "local_recording"), state = field(p, "state"), connected = field(p, "connected");
        if (equal(local, "true"))
            recording = true;
        else if (!equal(local, "false"))
            blocked = true;
        if (equal(state, "\"streaming\"") && equal(connected, "true"))
            quiet = false;
        else if ((!equal(state, "\"idle\"") && !equal(state, "\"offline\"")) || !equal(connected, "false"))
            blocked = true;
        if (equal(state, "\"idle\"") && !equal(field(p, "configured_bd"), "null"))
            quiet = false;
        p = space(object_end);
        if (*p != (i == 0 ? ',' : ']'))
            return GPS_MIC_BLOCKED;
        ++p;
        p = space(p);
    }
    if (recording)
        return GPS_MIC_RECORDING;
    if (blocked)
        return GPS_MIC_BLOCKED;
    return quiet ? GPS_MIC_SCAN : GPS_MIC_DIRECT;
}

static uint64_t monotonic_ns(void)
{
    struct timespec clock;
    if (clock_gettime(CLOCK_MONOTONIC, &clock) < 0)
        return 0;
    return (uint64_t)clock.tv_sec * GPS_NS_PER_SECOND + clock.tv_nsec;
}

static GpsMicStatus read_status(int fd, pid_t pid)
{
    char line[16384];
    size_t used = 0;
    uint64_t now = monotonic_ns();
    if (!now)
        return GPS_MIC_UNAVAILABLE;
    const uint64_t deadline = now + MIC_REPLY_NS;
    struct pollfd waiter = {.fd = fd, .events = POLLIN};
    while (now && now < deadline && used < sizeof(line) - 1) {
        int ready = poll(&waiter, 1, (int)((deadline - now + 999999) / 1000000));
        if (ready > 0 && (waiter.revents & POLLIN)) {
            ssize_t n = recv(fd, line + used, sizeof(line) - 1 - used, 0);
            if (n > 0) {
                char *newline = memchr(line + used, '\n', (size_t)n);
                if (newline) {
                    *newline = '\0';
                    return gps_mic_parse_status(line, pid);
                }
                used += (size_t)n;
            } else if (!n || (errno != EAGAIN && errno != EINTR)) {
                break;
            }
        } else if (ready >= 0 || errno != EINTR) {
            break;
        }
        now = monotonic_ns();
    }
    return GPS_MIC_UNAVAILABLE;
}

static GpsMicStatus query_status(pid_t pid)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return GPS_MIC_UNAVAILABLE;
    const struct sockaddr_un address = {.sun_family = AF_UNIX, .sun_path = MIC_SOCKET};
    struct ucred peer;
    socklen_t length = sizeof(peer);
    const char request[] = "status_all\n";
    GpsMicStatus result = GPS_MIC_UNAVAILABLE;
    if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) == 0) {
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &length) < 0 || length != sizeof(peer) ||
            peer.pid != pid || peer.uid != geteuid())
            result = GPS_MIC_BLOCKED;
        else if (send(fd, request, sizeof(request) - 1, MSG_NOSIGNAL) == sizeof(request) - 1)
            result = read_status(fd, pid);
    }
    close(fd);
    return result;
}

static bool missing(const char *path)
{
    struct stat info;
    return lstat(path, &info) < 0 && errno == ENOENT;
}

static bool mic_absent(pid_t pid)
{
    const char *paths[] = {"/data/mic/service.enabled", MIC_SOCKET, "/data/mic/active-release",
                           "/data/mic/managed.sha256", "/data/mic/micctl", "/data/mic/micd",
                           "/data/mic/mic-service.sh", "/data/mic/mic-inject", "/data/mic/libmiccore.so",
                           "/data/mic/libmiccore_hook.so", "/data/mic/libLC3plus_bionic.so",
                           "/system/etc/init/splus_mic.rc", "/system/bin/splus_mic_boot.sh"};
    for (size_t i = 0; i < sizeof(paths) / sizeof(*paths); ++i)
        if (!missing(paths[i]))
            return false;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", (int)pid);
    FILE *maps = fopen(path, "r");
    if (!maps)
        return false;
    bool observed = false, mapped = false;
    char line[4096];
    while (fgets(line, sizeof(line), maps)) {
        observed = true;
        if (strstr(line, "/data/mic/") && strstr(line, "libmiccore")) {
            mapped = true;
            break;
        }
    }
    bool complete = observed && !mapped && feof(maps);
    fclose(maps);
    return complete;
}

GpsMicStatus gps_mic_status(pid_t pid)
{
    if (pid <= 1)
        return GPS_MIC_BLOCKED;
    GpsMicStatus status = query_status(pid);
    return status == GPS_MIC_UNAVAILABLE && mic_absent(pid) ? GPS_MIC_ABSENT : status;
}

void gps_mic_apply(GpsPolicy *policy, uint64_t *last_admission_ns, GpsMicStatus status, uint64_t now)
{
    GpsConditions *conditions = &policy->conditions;
    bool admit = status == GPS_MIC_DIRECT || status == GPS_MIC_SCAN || status == GPS_MIC_ABSENT;
    /* A late reply can preserve a running direct link, but cannot start or confirm an inquiry. */
    bool grace = status == GPS_MIC_UNAVAILABLE && conditions->mic_direct && policy->link == GPS_LINK_STREAMING &&
                 !policy->release_requested && *last_admission_ns && now >= *last_admission_ns &&
                 now - *last_admission_ns < MIC_STREAM_GRACE_NS;
    if (admit || !grace)
        *last_admission_ns = admit ? now : 0;
    conditions->mic_known = status != GPS_MIC_UNAVAILABLE || grace;
    conditions->mic_installed = status != GPS_MIC_ABSENT;
    conditions->mic_direct = admit || grace;
    conditions->mic_scan = status == GPS_MIC_SCAN || status == GPS_MIC_ABSENT;
    conditions->video = status == GPS_MIC_RECORDING;
}
