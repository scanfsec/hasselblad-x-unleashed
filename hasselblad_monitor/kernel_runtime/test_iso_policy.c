/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include "iso_policy.h"

int main(void)
{
    assert(EXDEV == 18);
    assert(uvc_iso_recoverable(1, 1, 1, -EXDEV, -EXDEV));
    assert(!uvc_iso_recoverable(0, 1, 1, -EXDEV, -EXDEV));
    assert(!uvc_iso_recoverable(1, 0, 1, -EXDEV, -EXDEV));
    assert(!uvc_iso_recoverable(1, 1, 0, -EXDEV, -EXDEV));
    assert(!uvc_iso_recoverable(1, 1, 1, 0, 0));
    assert(!uvc_iso_recoverable(1, 1, 1, -ESHUTDOWN, -ESHUTDOWN));
    assert(!uvc_iso_recoverable(1, 1, 1, -EIO, -EIO));
    assert(!uvc_iso_recoverable(1, 1, 1, -EXDEV, 0));
    puts("UVC ISO recovery: nine policy assertions passed");
    return 0;
}
