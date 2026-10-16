#ifndef UVC_MONITOR_ISO_PACKET_SIZE_H
#define UVC_MONITOR_ISO_PACKET_SIZE_H

#include <stddef.h>
#include "libusb.h"

/* USB 3 moves the per-interval budget out of wMaxPacketSize. */
static size_t uvc_monitor_iso_packet_size(libusb_context *ctx,
        const struct libusb_endpoint_descriptor *ep, int speed)
{
    if ((ep->bmAttributes & 3) != LIBUSB_TRANSFER_TYPE_ISOCHRONOUS)
        return 0;
    const size_t packet = ep->wMaxPacketSize & 0x07ff;
    struct libusb_ss_endpoint_companion_descriptor *comp = NULL;
    /* Android's wrapped USB fd may have UNKNOWN speed when sysfs is inaccessible.
     * The endpoint's actual companion is authoritative even in that case. */
    const int result = libusb_get_ss_endpoint_companion_descriptor(ctx, ep, &comp);
    if (result == LIBUSB_ERROR_NOT_FOUND && speed < LIBUSB_SPEED_SUPER)
        return packet * (((ep->wMaxPacketSize >> 11) & 3) + 1);
    if (result != LIBUSB_SUCCESS)
        return 0;
    if (!comp)
        return 0;
    const size_t limit = packet * ((size_t)comp->bMaxBurst + 1) *
        ((comp->bmAttributes & 3) + 1);
    const size_t bytes = comp->wBytesPerInterval;
    const int valid = comp->bLength >= 6 && comp->bDescriptorType == LIBUSB_DT_SS_ENDPOINT_COMPANION &&
        comp->bMaxBurst <= 15 && (comp->bmAttributes & 3) <= 2 && bytes && bytes <= limit;
    libusb_free_ss_endpoint_companion_descriptor(comp);
    return valid ? bytes : 0;
}
#endif
