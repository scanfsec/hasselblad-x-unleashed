/* Fixed-prologue hook: one aligned B instruction, never a torn 16-byte patch.
 * Relocate only an explicitly audited stack-save/allocation instruction. */
#ifndef MIC_ARM64_HOOK_H
#define MIC_ARM64_HOOK_H
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
static int mic_branch(uintptr_t from, uintptr_t to, uint32_t *word)
{
    int64_t delta = (int64_t)to - (int64_t)from;
    if ((delta & 3) || delta < -134217728 || delta > 134217724) return -1;
    *word = 0x14000000u | ((uint32_t)(delta / 4) & 0x03ffffffu); return 0;
}
static void mic_abs_jump(unsigned char *code, uintptr_t target)
{
    const uint32_t op[2] = {0x58000050, 0xd61f0200}; /* ldr x16,+8; br x16 */
    memcpy(code, op, sizeof op); memcpy(code + 8, &target, sizeof target);
}
static int mic_hook_install(void *entry, const uint32_t expected[4], void *replacement, void **original)
{
    if (((uintptr_t)entry & 3) || memcmp(entry, expected, 16) ||
        (expected[0] != 0xa9bd57f6 && expected[0] != 0xf81a0ffc && expected[0] != 0xa9bc5ffc &&
         expected[0] != 0xd10283ff && expected[0] != 0xd10503ff && expected[0] != 0xf81e0ff3 &&
         expected[0] != 0xd10103ff && expected[0] != 0xd101c3ff)) return -1; /* sub sp,sp,#0x40/#0x70 */
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    if (!page || (page & (page - 1))) return -1;
    uintptr_t base = (uintptr_t)entry & ~(page - 1);
    unsigned char *relay = MAP_FAILED; uint32_t branch;
    for (uintptr_t gap = 0x100000; gap < 0x7f00000; gap += 0x100000) {
        for (int sign = -1; sign <= 1; sign += 2) {
            uintptr_t hint = sign < 0 ? base - gap : base + gap;
            relay = mmap((void *)hint, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (relay == MAP_FAILED) continue;
            if (!mic_branch((uintptr_t)entry, (uintptr_t)relay, &branch)) goto allocated;
            munmap(relay, page); relay = MAP_FAILED;
        }
    }
    return -2;
allocated:
    mic_abs_jump(relay, (uintptr_t)replacement);
    memcpy(relay + 32, expected, 4);
    mic_abs_jump(relay + 36, (uintptr_t)entry + 4);
    __builtin___clear_cache((char *)relay, (char *)relay + 52);
    if (mprotect(relay, page, PROT_READ | PROT_EXEC)) { munmap(relay, page); return -3; }
    __atomic_store_n(original, relay + 32, __ATOMIC_RELEASE);
    if (mprotect((void *)base, page, PROT_READ | PROT_WRITE | PROT_EXEC)) {
        __atomic_store_n(original, NULL, __ATOMIC_RELEASE); munmap(relay, page); return -3;
    }
    uint32_t before = expected[0];
    if (!__atomic_compare_exchange_n((uint32_t *)entry, &before, branch, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        mprotect((void *)base, page, PROT_READ | PROT_EXEC);
        __atomic_store_n(original, NULL, __ATOMIC_RELEASE); munmap(relay, page); return -4;
    }
    __builtin___clear_cache(entry, (char *)entry + 4);
    /* Once reachable, retain trampoline for the lifetime of the process. */
    return mprotect((void *)base, page, PROT_READ | PROT_EXEC) ? 1 : 0;
}
#endif
