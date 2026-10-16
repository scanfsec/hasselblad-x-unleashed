/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UVC_ISO_POLICY_H
#define UVC_ISO_POLICY_H

/* A missed ISO service slot is recoverable; disconnect and other errors are not. */
static inline int uvc_iso_recoverable(int matching_callback, int has_context,
                                    int iso_in, int status, int loaded_status)
{
    return matching_callback && has_context && iso_in &&
           status == -18 && loaded_status == status;
}

#endif
