/* Admit the engine's configured bonded peers on incoming SPP too. Writing
 * bt_audio.cfg would also alter stock boot visibility, so leave it untouched.
 * All native audio/delete tables remain owned by their original writers. */
#ifndef MIC_BT_ADMISSION_H
#define MIC_BT_ADMISSION_H
#include <errno.h>
#include "bt_resolver.h"
#include "arm64_hook.h"
#include "../decoder/mic_admission.h"
static int (*mic_admission_original)(const uint8_t *);
static int (*mic_delete_original)(const uint8_t *, unsigned char);

static int mic_admission_check(const uint8_t *bd)
{
    /* Keep the original config refresh and decisions for unrelated peers.
     * Never hold the snapshot mutex across native code or file I/O. */
    int result = __atomic_load_n(&mic_admission_original, __ATOMIC_ACQUIRE)(bd);
    int saved = errno;
    int policy = mic_admission_policy(bd);
    /* A temporary user hold wins even when the stock config lists this peer.
     * Keep the original refresh, and leave its result intact for other peers.
     * Transport teardown still belongs to the engine's ordinary DG close. */
    if (policy == MIC_ADMISSION_HELD) result = 0;
    else if (!result && policy == MIC_ADMISSION_ALLOW) result = 1;
    errno = saved;
    return result;
}

static int mic_admission_delete(const uint8_t *bd, unsigned char server)
{
    int saved = errno;
    /* A user hold is temporary, not removal from the camera's master list.
     * Do not enqueue a delete-address request that would survive a subsequent
     * explicit connect. The engine closes held OPENs via the ordinary DG API.
     * Unknown/unpaired peers keep the entire original rejection policy. */
    int managed = server && mic_admission_policy(bd) != MIC_ADMISSION_NONE;
    errno = saved;
    if (managed) return 0;
    return __atomic_load_n(&mic_delete_original, __ATOMIC_ACQUIRE)(bd, server);
}

static int mic_admission_install(void *base, const struct mic_bt_abi *abi)
{
    static int attempted, result;
    if (attempted) return result;
    if (!base || !abi) return -1;
    attempted = 1; result = -1;
    int rc = mic_hook_install((uint8_t *)base + abi->offset[MIC_ABI_AUDIO_ADMISSION],
                             sig_AUDIO_ADMISSION, (void *)mic_admission_check,
                             (void **)&mic_admission_original);
    if (rc < 0) return result = rc;
    int warning = rc;
    rc = mic_hook_install((uint8_t *)base + abi->offset[MIC_ABI_AUDIO_DELETE],
                          sig_AUDIO_DELETE, (void *)mic_admission_delete,
                          (void **)&mic_delete_original);
    return result = rc < 0 ? rc : (warning || rc);
}
#endif
