/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UVC_SSP_POLICY_H
#define UVC_SSP_POLICY_H

/* No descriptor ownership changes: replace only the current register cursor. */
static inline int uvc_ssp_use_ss(int is_uvc, int speed, int companion,
                                 unsigned long cursor, unsigned long ssp,
                                 unsigned long ss)
{
    return is_uvc && speed == 6 && companion == 1 && !cursor && !ssp && ss;
}

/* A configuration with a guarded SS-only UVC function is also usable at
 * SSP. At the verified usb_add_function branch, a nonzero register lets
 * the kernel set its normal superspeed_plus capability flag.
 */
static inline int uvc_ssp_config_use_ss(int is_uvc, int max_speed,
                                      unsigned long cursor,
                                      unsigned long ssp, unsigned long ss,
                                      int has_ss, int has_ssp)
{
    return is_uvc && max_speed == 6 && !cursor && !ssp && ss &&
           has_ss && !has_ssp;
}

#endif
