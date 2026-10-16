#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <link.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/stat.h>
#include "bt_resolver.h"
#include "arm64_hook.h"
#include "miccore.h"

static int (*original_play)(void *, const uint8_t *, int);
static atomic_int hook_ready;
static struct mic_bt_abi resolved_abi;
static uintptr_t executable_base;
const struct mic_bt_abi *mic_native_abi(void) { return &resolved_abi; }
void *mic_native_base(void) { return (void *)executable_base; }
static int locate_main(struct dl_phdr_info *info, size_t size, void *unused)
{
    (void)size; (void)unused;
    if (!info->dlpi_name[0] || !strcmp(info->dlpi_name, "/system/bin/bt_bsa_app")) {
        executable_base = info->dlpi_addr; return 1;
    }
    return 0;
}
int mic_native_hook_active(void) { return atomic_load(&hook_ready); }
static int play(void *address, const uint8_t *bytes, int length)
{
    int saved_errno = errno;
    /* The audited PLAY_DATA signature includes both six-byte address
     * comparisons. x0 is the peer address, not an opaque connection object. */
    miccore_feed_bd(address, bytes, length);
    errno = saved_errno;
    return __atomic_load_n(&original_play, __ATOMIC_ACQUIRE)(address, bytes, length);
}
static void hook_log(const char *message)
{
    mkdir("/blackbox/hbl-logs", 0755);
    mkdir("/blackbox/hbl-logs/mic", 0755);
    const char *path = "/blackbox/hbl-logs/mic/hook.log"; struct stat st;
    if (!stat(path, &st) && st.st_size > 262144) rename(path, "/blackbox/hbl-logs/mic/hook.log.1");
    FILE *f = fopen(path, "a");
    if (f) { fprintf(f, "pid=%d %s\n", getpid(), message); fclose(f); }
}
static void *start(void *unused)
{
    (void)unused;
    /* Constructor returns before engine startup: never run BSA or join worker
     * threads under the Android dynamic loader lock. */
    if (miccore_start()) hook_log("FAIL engine start; hook retained, no ready claim");
    else hook_log("OK native hook and in-process engine started");
    return NULL;
}
__attribute__((constructor)) static void load(void)
{
    if (mic_abi_load("/proc/self/exe", &resolved_abi)) { hook_log("FAIL ABI discovery"); return; }
    dl_iterate_phdr(locate_main, NULL);
    if (!executable_base) { hook_log("FAIL executable mapping missing"); return; }
    int rc = mic_hook_install((void *)(executable_base + resolved_abi.offset[MIC_ABI_PLAY_DATA]), sig_PLAY_DATA,
                              (void *)play, (void **)&original_play);
    if (rc < 0) { hook_log("FAIL hook prologue/allocation/protection; engine not started"); return; }
    atomic_store(&hook_ready, 1);
    if (rc) hook_log("WARNING hook installed but text page could not return to RX");
    pthread_t thread;
    if (pthread_create(&thread, NULL, start, NULL)) { hook_log("FAIL startup thread"); return; }
    pthread_detach(thread);
}
