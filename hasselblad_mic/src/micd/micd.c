/*
 * micd.c -- standalone (zero-injection) DJI Mic 2 daemon for X2D II.
 *
 * Runs the miccore engine as its own process instead of inside
 * bt_bsa_app. Nothing of the stock Bluetooth stack is touched:
 *   - link bring-up: bt_bsa_app's own DJI branch (TX HFP connect ->
 *     AG close -> app_dg_open_ex2) + its own clock/version sync;
 *   - audio: bt_bsa_app already forwards every accepted SPP payload to
 *     @/dsock/57012 (the Action 5 wireless_mic_service port, unbound on
 *     X2D II) -- the engine binds that port;
 *   - scan/pair/unpair/RSSI/disconnect: an extra BSA client on
 *     bt-daemon-socket (same as btman), never the bt_bsa_app client.
 *
 * The engine's constructor starts everything (miccore_start detects it is
 * not inside bt_bsa_app and switches to STANDALONE). main() only parks.
 * Faces are unchanged: /data/mic/mic.pcm, mic_status.json, mic_ctl.
 */
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

#include "miccore.h"

static volatile sig_atomic_t stop;

static void on_sig(int s) { (void)s; stop = 1; }

/* A successful status exchange, not a PID/file, proves the shared engine is ready. */
static int probe(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct timeval timeout = { .tv_sec = 1 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    strcpy(a.sun_path, "/data/mic/mic_ctl");
    char buf[4096]; int n = 0;
    if (!connect(fd, (struct sockaddr *)&a, sizeof a) && write(fd, "status\n", 7) == 7) {
        while (n < (int)sizeof buf - 1) {
            int got = read(fd, buf + n, sizeof buf - 1 - n);
            if (got <= 0) break;
            n += got;
            if (memchr(buf, '\n', n)) break;
        }
    }
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    if (!strstr(buf, "\"protocol_version\":4") || !strstr(buf, "\"session_api\":1")) return -1;
    return strstr(buf, "\"engine_ready\":true") ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--ensure")) {
        if (!access("/data/mic/mic-service.sh", X_OK)) {
            execl("/system/bin/sh", "sh", "/data/mic/mic-service.sh", "ensure", (char *)NULL);
            perror("micd: native service launcher"); return 1;
        }
        int ready = probe();
        if (ready) { puts(ready > 0 ? "OK ready" : "FAIL incompatible-engine"); return ready < 0; }
        pid_t pid = fork();
        if (pid < 0) return 1;
        if (pid > 0) {
            for (int i = 0; i < 40; i++) {
                ready = probe();
                if (ready) { puts(ready > 0 ? "OK ready" : "FAIL incompatible-engine"); return ready < 0; }
                /* A concurrent starter can win the engine lock; keep probing. */
                waitpid(pid, NULL, WNOHANG);
                usleep(250000);
            }
            puts("FAIL engine-not-ready"); return 1;
        }
        setsid();
        int nullfd = open("/dev/null", O_RDWR);
        if (nullfd >= 0) {
            dup2(nullfd, 0); dup2(nullfd, 1); dup2(nullfd, 2);
            if (nullfd > 2) close(nullfd);
        }
    } else if (argc != 1) {
        fprintf(stderr, "usage: micd [--ensure]\n"); return 2;
    }
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    if (miccore_start() != 0) {         /* idempotent; ctor already ran */
        fprintf(stderr, "micd: engine start failed\n");
        return 1;
    }
    for (;;) {
        /* Avoid the lost-signal race of while (!stop) pause(). */
        if (stop) {
            stop = 0;
            if (!miccore_stop_if_idle()) break;
        }
        sleep(1);
    }
    return 0;
}
