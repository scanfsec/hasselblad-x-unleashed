/*
 * bootstrap_preload.c -- LD_PRELOAD constructor that initializes the BT
 * stack inside bt_bsa_app when nobody else will (the wlmicd cutover gap).
 *
 * THE GAP (live-tested 2026-10-03): bt_bsa_app's main() only creates the
 * WMS dsocks and idles -- the BSA stack (bsa_server client, AG/HFP + DG
 * profile registration, visibility) comes up ONLY when WMS dsock cmd 0x00
 * arrives via /dsock/90000, source-verified to /dsock/80000 which
 * dji_network owns and never re-sends for a manually started instance
 * (35 s observation: no init, no pull-up; second-client AgEnable probe
 * SUCCEEDED = AG was never registered).  The stage-1 frida engine used
 * to bootstrap it with an in-process app_init() call; retiring the
 * engine retired the initialization and TX's HFP dance had no AG to
 * connect to -- dead chain.
 *
 * THE FIX (OPERATIONS.md stage-2 plan, now landed): start bt_bsa_app as
 *
 *   cd /data/misc/bluetooth && \
 *   LD_PRELOAD=/data/mic/bootstrap_preload.so setsid /system/bin/bt_bsa_app
 *
 * This tiny .so interposes NOTHING.  Its constructor spawns one thread
 * that waits for main() to bind /dsock/90000 (visible in /proc/net/unix;
 * constructor code runs before main, so app_init must be delayed), then
 * performs exactly what the WMS cmd 0x00 handler does:
 *
 *   app_init()                     at base+0x2b168   int f(void)
 *   stack-inited flag = 1          at base+0x825a6   u8
 *
 * Both offsets verified against the live firmware 316 binary
 * (md5 de64e90af24d1b06697e87b0e12b8907) and are the same constants the
 * frida engine used in production (src/decoder/miccore.c bb_init).  The
 * load base is resolved from /proc/self/maps: the FIRST r--p mapping
 * with file offset 00000000 whose path contains "bt_bsa_app" (never the
 * relro segment at +0x7b000, which is also r--p -- using it once caused
 * a SIGSEGV, see BACKEND_NOTES 7.3).
 *
 * Idempotent and non-intrusive: if the inited flag is already 1 (a real
 * WMS cmd 0x00 won the race, e.g. normal product boot), the thread
 * exits without calling anything.  WLMICD_NO_BOOTSTRAP=1 in the
 * environment disables the whole mechanism (debug kill-switch).
 *
 * Log: /data/mic/bootstrap.log (one line per lifecycle step).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LOG_PATH   "/data/mic/bootstrap.log"
#define BSA_SOCK   "/data/misc/bluetooth/bt-daemon-socket"

/* bt_bsa_app firmware 316 (PIE) -- same constants as miccore.c bb_init */
#define OFF_APP_INIT 0x2b168      /* int app_init(void): the WMS cmd 0x00 action */
#define OFF_INITED   0x825a6      /* u8: set by the cmd 0x00 handler after app_init */
#define OFF_DG_LISTEN 0x252c8     /* int app_dg_listen(void): arm SPP listener */
#define OFF_DM_VIS   0xda30       /* void app_dm_set_visibility(int, int) */

static int g_log_fd = -1;

static void blog(const char *fmt, ...)
{
    char b[300];
    struct timespec now;
    struct tm tmv;
    va_list ap;
    int n;
    time_t t;

    if (g_log_fd < 0) {
        g_log_fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (g_log_fd < 0) return;
    }
    clock_gettime(CLOCK_REALTIME, &now);
    t = now.tv_sec;
    localtime_r(&t, &tmv);
    n = snprintf(b, sizeof b, "[%02d:%02d:%02d.%03ld] bootstrap: ",
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec, now.tv_nsec / 1000000);
    va_start(ap, fmt);
    n += vsnprintf(b + n, sizeof b - n - 1, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof b - 2) n = (int)sizeof b - 2;
    b[n++] = '\n';
    if (write(g_log_fd, b, (size_t)n) < 0) { /* ignore */ }
}

/* Resolve the bt_bsa_app PIE load base from our own maps: the first
 * r--p mapping with file offset 00000000 on a path containing
 * "bt_bsa_app" (the ELF header mapping). */
static unsigned long resolve_base(void)
{
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    unsigned long best = 0;

    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long lo;
        char perms[12], off[24];
        if (strstr(line, "bt_bsa_app") &&
            sscanf(line, "%lx %11s %23s", &lo, perms, off) == 3) {
            if (!strcmp(off, "00000000") && strstr(perms, "r")) {
                best = lo;
                break;
            }
            if (best == 0 || lo < best) best = lo;
        }
    }
    fclose(f);
    return best;
}

/* Wait until the abstract socket "@/dsock/90000" shows up in
 * /proc/net/unix (= main() finished its early setup) or timeout. */
static int wait_for_90000(int timeout_ms)
{
    uint64_t t0 = (uint64_t)time(NULL);
    for (;;) {
        FILE *f = fopen("/proc/net/unix", "r");
        char line[256];
        int found = 0;
        if (f) {
            while (fgets(line, sizeof line, f))
                if (strstr(line, "/dsock/90000")) { found = 1; break; }
            fclose(f);
        }
        if (found) return 0;
        if ((uint64_t)time(NULL) - t0 >= (uint64_t)(timeout_ms / 1000) + 1)
            return -1;
        usleep(100 * 1000);
    }
}

static void *bootstrap_thread(void *arg)
{
    unsigned long base;
    uint8_t *flag;
    int (*p_app_init)(void);
    int rc = -1, tries;

    (void)arg;

    if (wait_for_90000(10000) != 0) {
        blog("ERROR: /dsock/90000 not seen within 10s -- bt_bsa_app main() "
             "did not come up; NOT initializing (check the process)");
        return NULL;
    }
    blog("dsock/90000 up (main() ready)");

    base = resolve_base();
    if (!base) {
        blog("ERROR: bt_bsa_app base not found in /proc/self/maps");
        return NULL;
    }
    flag = (uint8_t *)(uintptr_t)(base + OFF_INITED);
    p_app_init = (int (*)(void))(uintptr_t)(base + OFF_APP_INIT);
    blog("base=0x%lx app_init=+%p flag=+%p already_inited=%d",
         base, (void *)(uintptr_t)OFF_APP_INIT, (void *)(uintptr_t)OFF_INITED,
         (int)*flag);

    if (*flag) {
        /* a genuine WMS cmd 0x00 (product boot path via dji_network) won
         * the race -- do nothing, we are the redundant leg */
        blog("stack already initialized (WMS cmd 0x00 path) -- nothing to do");
        return NULL;
    }

    /* let main() settle into its loop, then fire the cmd-0x00 action */
    usleep(100 * 1000);
    for (tries = 0; tries < 5; tries++) {
        if (*flag) {
            blog("inited flag set by someone else during retry -- standing down");
            return NULL;
        }
        rc = p_app_init();
        blog("app_init() try %d = %d", tries + 1, rc);
        if (rc == 0) break;
        /* likely bsa_server not reachable yet: wait and retry */
        struct stat st;
        if (stat(BSA_SOCK, &st) != 0)
            blog("%s not present (bsa_server down?) -- retrying", BSA_SOCK);
        sleep(2);
    }
    if (rc != 0) {
        blog("ERROR: app_init() kept failing -- TX HFP dance will not work; "
             "check bsa_server / bt-daemon-socket");
        return NULL;
    }
    *flag = 1;                          /* exactly what the cmd 0x00 handler does */
    blog("stack initialized (inited flag set) -- AG/DG registered, "
         "wlmicd chain armed");

    /* Product boot also arms the SPP listener + visibility immediately
     * (wms_start -> quickly_start_dg_listen).  The TX's reconnect window
     * is ~2 min after link loss; waiting 3-4 min for the engine inject
     * means the TX has already given up.  Arm both NOW. */
    {
        int (*p_dg_listen)(void) =
            (int (*)(void))(uintptr_t)(base + OFF_DG_LISTEN);
        void (*p_dm_vis)(int, int) =
            (void (*)(int, int))(uintptr_t)(base + OFF_DM_VIS);
        int lrc = p_dg_listen();
        blog("early dg_listen() = %d (SPP listener armed for TX redial)", lrc);
        p_dm_vis(1, 1);
        blog("early visibility(1,1) (discoverable+connectable for TX)");
    }

    return NULL;
}

/* Is THIS process actually bt_bsa_app?  The LD_PRELOAD env propagates
 * to helpers the start script spawns along the way (setsid forks, etc.)
 * -- their constructors would run too, find /dsock/90000 (bound by the
 * real bt_bsa_app), fail the base lookup and spam ERROR lines.  Gate the
 * whole mechanism on the executable name. */
static int we_are_bt_bsa_app(void)
{
    char exe[128];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return 0;
    exe[n] = 0;
    return strstr(exe, "bt_bsa_app") != NULL;
}

__attribute__((constructor)) static void bootstrap_ctor(void)
{
    pthread_t t;
    pthread_attr_t a;

    if (getenv("WLMICD_NO_BOOTSTRAP")) {
        blog("disabled via WLMICD_NO_BOOTSTRAP");
        return;
    }
    if (!we_are_bt_bsa_app())
        return;                         /* setsid/sh helper: silently skip */
    blog("constructor: bt_bsa_app image confirmed, spawn bootstrap thread");
    /* The thread blocks until main() is up (90000 poll), so nothing runs
     * under the loader lock.  Detached: no joiner, dies with the process. */
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, bootstrap_thread, NULL) != 0)
        blog("ERROR: cannot spawn bootstrap thread: %s", strerror(errno));
    pthread_attr_destroy(&a);
}
