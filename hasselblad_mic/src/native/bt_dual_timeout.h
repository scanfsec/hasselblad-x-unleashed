/* Action 5 closes only the timed-out TX. The audited Hasselblad timer also
 * re-arms the OTHER TX's clock state. Skip just that post-close handoff;
 * retain both native retry loops, close/error handling and later maintenance.
 * Discover the whole code pattern, not a firmware version or absolute VA.
 * The executable on disk is never modified. A process restart removes this.
 */
#ifndef MIC_BT_DUAL_TIMEOUT_H
#define MIC_BT_DUAL_TIMEOUT_H
#include "bt_resolver.h"
#include "arm64_hook.h"
#define MIC_TIMEOUT_SKIP_WORD (0x230 / 4)
#define MIC_TIMEOUT_NEXT_WORD (0x414 / 4)
static int mic_timeout_patch(void *base, const struct mic_bt_abi *abi)
{
    if (!base || !abi || !abi->offset[MIC_ABI_AUDIO_TIMER]) return -1;
    uint32_t *timer = (uint32_t *)((uintptr_t)base + abi->offset[MIC_ABI_AUDIO_TIMER]);
    uint32_t branch;
    if (mic_branch((uintptr_t)(timer + MIC_TIMEOUT_SKIP_WORD),
                   (uintptr_t)(timer + MIC_TIMEOUT_NEXT_WORD), &branch)) return -1;
    /* Validate memory as well as the file, including the skipped region and
     * its continuation. A previously installed identical patch is idempotent. */
    for (unsigned i = 0; i < sizeof sig_AUDIO_TIMER / sizeof *sig_AUDIO_TIMER; i++) {
        uint32_t word = __atomic_load_n(timer + i, __ATOMIC_ACQUIRE);
        if (i == MIC_TIMEOUT_SKIP_WORD && word == branch) continue;
        uint32_t mask = mic_code_mask(sig_AUDIO_TIMER[i]);
        if ((word & mask) != (sig_AUDIO_TIMER[i] & mask)) return -2;
    }
    uint32_t *entry = timer + MIC_TIMEOUT_SKIP_WORD;
    uint32_t expected = sig_AUDIO_TIMER[MIC_TIMEOUT_SKIP_WORD];
    if (__atomic_load_n(entry, __ATOMIC_ACQUIRE) == branch) return 0;
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    if (!page || (page & (page - 1))) return -3;
    uintptr_t start = (uintptr_t)entry & ~(page - 1);
    if (mprotect((void *)start, page, PROT_READ | PROT_WRITE | PROT_EXEC)) return -3;
    if (!__atomic_compare_exchange_n(entry, &expected, branch, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        mprotect((void *)start, page, PROT_READ | PROT_EXEC);
        return -4;
    }
    __builtin___clear_cache((char *)entry, (char *)entry + 4);
    return mprotect((void *)start, page, PROT_READ | PROT_EXEC) ? 1 : 0;
}
#endif
