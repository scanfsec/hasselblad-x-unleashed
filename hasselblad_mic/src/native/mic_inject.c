/* Small, deliberately restricted Android/ARM64 injector. No SELinux changes.
 * Only borrow the main thread while it is in nanosleep. Other threads run,
 * including any loader-lock owner. Restore GPRs, SIMD and errno before detach.
 * An in-flight dlopen must finish: never timeout/kill this process mid-call.
 */
#define _GNU_SOURCE
#include <asm/ptrace.h>
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include "bt_resolver.h"

#ifndef MIC_INJECT_TEST
#define TARGET_EXE "/system/bin/bt_bsa_app"
#define LIB_PREFIX "/data/mic/releases/"
#else
#define TARGET_EXE "/data/local/tmp/mic-inject-test-target"
#define LIB_PREFIX "/data/local/tmp/mic-inject-test-"
#endif

static int regset(pid_t pid, int request, int note, void *data, size_t size)
{
    struct iovec iov = { data, size };
    return ptrace(request, pid, (void *)(uintptr_t)note, &iov) < 0 || iov.iov_len != size ? -1 : 0;
}
static int memory(pid_t pid, uintptr_t addr, void *data, size_t size, int writing)
{
    for (size_t p = 0; p < size; p += sizeof(long)) {
        size_t n = size - p < sizeof(long) ? size - p : sizeof(long);
        long word; errno = 0;
        word = ptrace(PTRACE_PEEKDATA, pid, (void *)(addr + p), 0);
        if (errno) return -1;
        if (writing) {
            memcpy(&word, (char *)data + p, n);
            if (ptrace(PTRACE_POKEDATA, pid, (void *)(addr + p), (void *)word)) return -1;
        } else memcpy((char *)data + p, &word, n);
    }
    return 0;
}
static uintptr_t remote_symbol(pid_t pid, const char *symbol)
{
    Dl_info info; void *local = dlsym(RTLD_DEFAULT, symbol);
    if (!local || !dladdr(local, &info)) return 0;
    char path[64], line[1024], filename[768]; unsigned long lo, hi, off; char perm[8];
    snprintf(path, sizeof path, "/proc/%d/maps", pid);
    FILE *f = fopen(path, "r"); if (!f) return 0;
    uintptr_t result = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%lx-%lx %7s %lx %*s %*s %767s", &lo, &hi, perm, &off, filename) == 5 &&
            off == 0 && !strcmp(filename, info.dli_fname)) {
            result = lo + ((uintptr_t)local - (uintptr_t)info.dli_fbase); break;
        }
    }
    fclose(f); return result;
}
static int wait_stop(pid_t pid, int *status)
{
    pid_t r; do { r = waitpid(pid, status, __WALL); } while (r < 0 && errno == EINTR);
    return r == pid && WIFSTOPPED(*status) ? 0 : -1;
}
static int call(pid_t pid, const struct user_pt_regs *saved, uintptr_t function,
                const uintptr_t args[6], uintptr_t *result)
{
    struct user_pt_regs r = *saved; int status;
    for (int i = 0; i < 6; ++i) r.regs[i] = args[i];
    r.pc = function; r.regs[30] = 0;
    r.sp = (saved->sp - 256) & ~(uintptr_t)15;
    if (regset(pid, PTRACE_SETREGSET, NT_PRSTATUS, &r, sizeof r) ||
        ptrace(PTRACE_CONT, pid, 0, 0)) return -1;
    for (;;) {
        if (wait_stop(pid, &status) || regset(pid, PTRACE_GETREGSET, NT_PRSTATUS, &r, sizeof r)) return -1;
        if (WSTOPSIG(status) == SIGSEGV && r.pc == 0) {
            *result = r.regs[0]; return 0; /* our return sentinel, suppress it */
        }
        /* Do not rewind a partially executed loader operation after a fault.
         * Forward genuine signals; normal fatal-signal semantics are retained. */
        if (ptrace(PTRACE_CONT, pid, 0, (void *)(uintptr_t)WSTOPSIG(status))) return -1;
    }
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--probe")) {
        struct mic_bt_abi abi; int rc = mic_abi_load(argv[2], &abi);
        printf("%s abi-discovery=%d play=0x%lx inited=0x%lx dg_table=0x%lx\n",
               rc ? "FAIL" : "OK", rc, (unsigned long)abi.offset[MIC_ABI_PLAY_DATA],
               (unsigned long)abi.inited, (unsigned long)abi.dg_table);
        return rc ? 3 : 0;
    }
    if (argc != 3) { fprintf(stderr, "usage: mic-inject <pid> <absolute-release-library>\n"); return 2; }
    char *end, path[128], exe[512]; long value = strtol(argv[1], &end, 10);
    if (*end || value <= 1 || value > 2147483647 || strlen(argv[2]) > 512 ||
        strncmp(argv[2], LIB_PREFIX, strlen(LIB_PREFIX)) || strstr(argv[2], "..")) return 2;
    pid_t pid = (pid_t)value;
    snprintf(path, sizeof path, "/proc/%d/exe", pid);
    ssize_t n = readlink(path, exe, sizeof exe - 1);
    if (n <= 0) return 3; exe[n] = 0;
    if (strcmp(exe, TARGET_EXE)) { fprintf(stderr, "FAIL target executable\n"); return 3; }
#ifndef MIC_INJECT_TEST
    struct mic_bt_abi abi;
    if (mic_abi_load(path, &abi)) { fprintf(stderr, "FAIL ABI discovery; no offset fallback\n"); return 3; }
#endif
    int library = open(argv[2], O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (library < 0) { perror("library"); return 3; } close(library);
    snprintf(path, sizeof path, "/data/local/tmp/mic-inject-%d.lock", pid);
    int lock = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) return 4;
    uintptr_t mm = remote_symbol(pid, "mmap"), dl = remote_symbol(pid, "dlopen");
    uintptr_t mu = remote_symbol(pid, "munmap"), er = remote_symbol(pid, "__errno");
    if (!mm || !dl || !mu || !er) { fprintf(stderr, "FAIL resolve system library symbols\n"); return 3; }
    /* Keep termination signals pending until the borrowed thread is restored. */
    sigset_t blocked, previous; sigfillset(&blocked); sigprocmask(SIG_BLOCK, &blocked, &previous);
    struct user_pt_regs saved; struct user_fpsimd_state fp;
    int attached = 0, have_regs = 0, have_errno = 0, status, rc = 5, saved_errno = 0;
    uintptr_t args[6] = {0}, scratch = 0, result = 0, errno_addr = 0;
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (ptrace(PTRACE_SEIZE, pid, 0, 0)) goto done;
        attached = 1;
        if (ptrace(PTRACE_INTERRUPT, pid, 0, 0) || wait_stop(pid, &status)) goto done;
        if (regset(pid, PTRACE_GETREGSET, NT_PRSTATUS, &saved, sizeof saved)) goto done;
        uint32_t svc = 0;
#ifdef MIC_INJECT_TEST
        if (!attempt) fprintf(stderr, "test stop: syscall=%llu x0=%lld pc=%llx\n",
            (unsigned long long)saved.regs[8], (long long)saved.regs[0], (unsigned long long)saved.pc);
#endif
        /* SYS_nanosleep=101. Interrupted sleep is allowed to return EINTR;
         * never hijack a thread in malloc, dlopen, BSA or arbitrary code. */
        if (saved.regs[8] == 101) {
            /* PTRACE_INTERRUPT normally presents the rewound SVC and original
             * arguments, ready to restart the sleep. Preserve that verbatim. */
            if ((int64_t)saved.regs[0] > 0 &&
                !memory(pid, saved.pc, &svc, sizeof svc, 0) && svc == 0xd4000001) break;
            if ((int64_t)saved.regs[0] == -516 &&
                !memory(pid, saved.pc - 4, &svc, sizeof svc, 0) && svc == 0xd4000001) {
                saved.regs[0] = (uintptr_t)-EINTR; break;
            }
        }
        ptrace(PTRACE_DETACH, pid, 0, 0); attached = 0;
        usleep(50000);
    }
    if (!attached) { fprintf(stderr, "FAIL no safe nanosleep point\n"); goto done; }
    if (regset(pid, PTRACE_GETREGSET, NT_FPREGSET, &fp, sizeof fp)) goto done;
    have_regs = 1;
    if (call(pid, &saved, er, args, &errno_addr) || !errno_addr ||
        memory(pid, errno_addr, &saved_errno, sizeof saved_errno, 0)) goto done;
    have_errno = 1;
    args[1] = 4096; args[2] = PROT_READ | PROT_WRITE;
    args[3] = MAP_PRIVATE | MAP_ANONYMOUS; args[4] = (uintptr_t)-1;
    if (call(pid, &saved, mm, args, &scratch) || scratch == (uintptr_t)-1 || !scratch) { scratch = 0; goto done; }
    if (memory(pid, scratch, argv[2], strlen(argv[2]) + 1, 1)) goto done;
    memset(args, 0, sizeof args); args[0] = scratch; args[1] = RTLD_NOW | RTLD_GLOBAL;
    if (call(pid, &saved, dl, args, &result)) goto done;
    rc = result ? 0 : 6;
done:
    if (attached) {
        if (have_regs) {
            if (scratch) {
                memset(args, 0, sizeof args); args[0] = scratch; args[1] = 4096;
                if (call(pid, &saved, mu, args, &result)) rc = 7;
            }
            if (have_errno && memory(pid, errno_addr, &saved_errno, sizeof saved_errno, 1)) rc = 7;
            if (regset(pid, PTRACE_SETREGSET, NT_FPREGSET, &fp, sizeof fp) ||
                regset(pid, PTRACE_SETREGSET, NT_PRSTATUS, &saved, sizeof saved)) rc = 7;
            struct user_pt_regs check_regs; struct user_fpsimd_state check_fp;
            if (regset(pid, PTRACE_GETREGSET, NT_PRSTATUS, &check_regs, sizeof check_regs) ||
                regset(pid, PTRACE_GETREGSET, NT_FPREGSET, &check_fp, sizeof check_fp) ||
                memcmp(&saved, &check_regs, sizeof saved) || memcmp(&fp, &check_fp, sizeof fp)) rc = 7;
        }
        if (ptrace(PTRACE_DETACH, pid, 0, 0)) rc = 7;
    }
    fprintf(stderr, "%s injector rc=%d pid=%d (library load only; service must verify hook readiness)\n", rc ? "FAIL" : "OK", rc, pid);
    sigprocmask(SIG_SETMASK, &previous, NULL); close(lock); return rc;
}
