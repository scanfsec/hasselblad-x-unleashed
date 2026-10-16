/*
 * micctl.c -- control-face CLI client for the miccore engine (stage 1).
 *
 * Speaks the frozen INTERFACE.md line protocol over AF_UNIX
 * /data/mic/mic_ctl. Every command maps 1:1 to the protocol verbs:
 *
 *   micctl                      -> status
 *   micctl status [bd] | status_all
 *   micctl connect [bd] | disconnect [bd]
 *   micctl scan <sec>
 *   micctl pair <bd> [1|2]     -> prints "OK paired" | "FAIL <why>"
 *   micctl unpair <bd>
 *   micctl rec <sec> [path]
 *   micctl stop
 *   micctl kickstart            -> extension (diagnostics; not in v1 spec)
 *   micctl raw <line>           -> escape hatch: send any line verbatim
 *
 * Response lines are printed as received. The connection is closed with
 * "quit" per spec when the server has not already closed it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>

#ifndef CTL_PATH
#define CTL_PATH "/data/mic/mic_ctl"
#endif

static int conn_ctl(void)
{
    struct sockaddr_un a;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, CTL_PATH, sizeof a.sun_path - 1);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        fprintf(stderr, "micctl: connect %s: %s (engine not running?)\n",
                CTL_PATH, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static int send_line(const char *line)
{
    int fd = conn_ctl();
    char buf[8192];
    FILE *f;
    int got = 0;

    if (fd < 0) return 2;
    if (write(fd, line, strlen(line)) != (int)strlen(line) ||
        write(fd, "\n", 1) != 1) {
        perror("write");
        close(fd);
        return 1;
    }
    f = fdopen(fd, "r");
    if (!f) { close(fd); return 1; }
    /* one command = one response line (INTERFACE.md); the server keeps the
     * connection open for more commands, so we must say quit to get EOF. */
    while (fgets(buf, sizeof buf, f)) {
        printf("%s", buf);
        fflush(stdout);
        got = 1;
        if (strchr(buf, '\n')) break; /* a dual snapshot may span several reads */
    }
    write(fd, "quit\n", 5);
    /* drain until server closes (bounded) */
    {
        struct timeval tv = { 2, 0 };
        fd_set rf;
        FD_ZERO(&rf); FD_SET(fd, &rf);
        if (select(fd + 1, &rf, NULL, NULL, &tv) > 0)
            while (fgets(buf, sizeof buf, f)) ;
    }
    fclose(f);
    return got ? 0 : 1;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: micctl status [bd] | status_all | connect [bd] | disconnect [bd] | scan <sec> | "
        "pair <bd> [1|2] | unpair <bd> | rec <sec> [path] | stop | kickstart [bd] | raw <line>\n");
}

int main(int argc, char **argv)
{
    char line[1024];

    if (argc < 2) {
        snprintf(line, sizeof line, "status");
    } else if (!strcmp(argv[1], "status") || !strcmp(argv[1], "connect") ||
               !strcmp(argv[1], "disconnect") ||
               !strcmp(argv[1], "kickstart")) {
        if (argc > 3) { usage(); return 2; }
        snprintf(line, sizeof line, argc == 3 ? "%s %s" : "%s", argv[1], argc == 3 ? argv[2] : "");
    } else if ((!strcmp(argv[1], "stop") || !strcmp(argv[1], "status_all")) && argc == 2) {
        snprintf(line, sizeof line, "%s", argv[1]);
    } else if (!strcmp(argv[1], "scan")) {
        snprintf(line, sizeof line, "scan %s", argc > 2 ? argv[2] : "10");
    } else if ((!strcmp(argv[1], "pair") || !strcmp(argv[1], "unpair")) && argc > 2) {
        if (argc > 4 || (argc == 4 && strcmp(argv[1], "pair"))) { usage(); return 2; }
        snprintf(line, sizeof line, argc == 4 ? "%s %s %s" : "%s %s",
                 argv[1], argv[2], argc == 4 ? argv[3] : "");
    } else if (!strcmp(argv[1], "rec")) {
        if (argc > 3)
            snprintf(line, sizeof line, "rec %s %s", argv[2], argv[3]);
        else
            snprintf(line, sizeof line, "rec %s", argc > 2 ? argv[2] : "0");
    } else if (!strcmp(argv[1], "raw") && argc > 2) {
        snprintf(line, sizeof line, "%s", argv[2]);
    } else {
        usage();
        return 2;
    }
    return send_line(line);
}
