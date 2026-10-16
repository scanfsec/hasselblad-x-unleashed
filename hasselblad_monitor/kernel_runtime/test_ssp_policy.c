/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include "ssp_policy.h"

int main(void)
{
    assert(uvc_ssp_use_ss(1, 6, 1, 0, 0, 0x1234));
    assert(!uvc_ssp_use_ss(0, 6, 1, 0, 0, 0x1234));
    assert(!uvc_ssp_use_ss(1, 5, 1, 0, 0, 0x1234));
    assert(!uvc_ssp_use_ss(1, 3, 0, 0, 0, 0x1234));
    assert(!uvc_ssp_use_ss(1, 6, 0, 0, 0, 0x1234));
    assert(!uvc_ssp_use_ss(1, 6, 1, 0x4567, 0, 0x1234));
    assert(!uvc_ssp_use_ss(1, 6, 1, 0, 0x4567, 0x1234));
    assert(!uvc_ssp_use_ss(1, 6, 1, 0, 0, 0));
    assert(!uvc_ssp_use_ss(1, 0, 1, 0, 0, 0x1234));
    assert(uvc_ssp_config_use_ss(1, 6, 0, 0, 0x1234, 1, 0));
    assert(!uvc_ssp_config_use_ss(0, 6, 0, 0, 0x1234, 1, 0));
    assert(!uvc_ssp_config_use_ss(1, 5, 0, 0, 0x1234, 1, 0));
    assert(!uvc_ssp_config_use_ss(1, 0, 0, 0, 0x1234, 1, 0));
    assert(!uvc_ssp_config_use_ss(1, 6, 0x4567, 0, 0x1234, 1, 0));
    assert(!uvc_ssp_config_use_ss(1, 6, 0, 0x4567, 0x1234, 1, 0));
    assert(!uvc_ssp_config_use_ss(1, 6, 0, 0, 0, 1, 0));
    assert(!uvc_ssp_config_use_ss(1, 6, 0, 0, 0x1234, 0, 0));
    assert(!uvc_ssp_config_use_ss(1, 6, 0, 0, 0x1234, 1, 1));
    puts("SSP fallback: eighteen endpoint/configuration policy checks passed");
    return 0;
}
