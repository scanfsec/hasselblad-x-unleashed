#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "iso_packet_size.h"

static struct libusb_ss_endpoint_companion_descriptor descriptor;
static unsigned int parsed, freed;
static int parse_error;

int libusb_get_ss_endpoint_companion_descriptor(libusb_context *ctx,
        const struct libusb_endpoint_descriptor *ep,
        struct libusb_ss_endpoint_companion_descriptor **result)
{
    (void)ctx; (void)ep;
    parsed++;
    if (parse_error) return parse_error;
    *result = &descriptor;
    return 0;
}
void libusb_free_ss_endpoint_companion_descriptor(
        struct libusb_ss_endpoint_companion_descriptor *comp)
{
    assert(comp == &descriptor);
    freed++;
}
int main(void)
{
    struct libusb_endpoint_descriptor ep;
    memset(&ep, 0, sizeof(ep));
    ep.bmAttributes = LIBUSB_TRANSFER_TYPE_ISOCHRONOUS;
    ep.wMaxPacketSize = 0x1400;
    parse_error = LIBUSB_ERROR_NOT_FOUND;
    assert(uvc_monitor_iso_packet_size(NULL, &ep, LIBUSB_SPEED_HIGH) == 3072);
    ep.wMaxPacketSize = 1023;
    assert(uvc_monitor_iso_packet_size(NULL, &ep, LIBUSB_SPEED_FULL) == 1023);
    assert(parsed == 2 && freed == 0);
    parse_error = 0;
    ep.wMaxPacketSize = 1024;
    descriptor.bLength = 6;
    descriptor.bDescriptorType = LIBUSB_DT_SS_ENDPOINT_COMPANION;
    descriptor.bmAttributes = 2;
    descriptor.wBytesPerInterval = 3072;
    assert(uvc_monitor_iso_packet_size(NULL, &ep, LIBUSB_SPEED_SUPER) == 3072);
    assert(parsed == 3 && freed == 1);
    assert(uvc_monitor_iso_packet_size(NULL, &ep, LIBUSB_SPEED_UNKNOWN) == 3072);
    assert(uvc_monitor_iso_packet_size(NULL, &ep, LIBUSB_SPEED_HIGH) == 3072);
    descriptor.bMaxBurst = 1;
    descriptor.wBytesPerInterval = 6144;
    assert(uvc_monitor_iso_packet_size(NULL, &ep, LIBUSB_SPEED_SUPER) == 6144);
    descriptor.wBytesPerInterval = 0;
    assert(uvc_monitor_iso_packet_size(NULL, &ep, LIBUSB_SPEED_SUPER) == 0);
    assert(uvc_monitor_iso_packet_size(NULL, &ep, LIBUSB_SPEED_UNKNOWN) == 0);
    descriptor.wBytesPerInterval = 6145;
    assert(uvc_monitor_iso_packet_size(NULL, &ep, LIBUSB_SPEED_SUPER) == 0);
    descriptor.wBytesPerInterval = 3072;
    descriptor.bmAttributes = 3;
    assert(uvc_monitor_iso_packet_size(NULL, &ep, LIBUSB_SPEED_SUPER) == 0);
    descriptor.bmAttributes = 2;
    descriptor.bMaxBurst = 16;
    assert(uvc_monitor_iso_packet_size(NULL, &ep, LIBUSB_SPEED_SUPER) == 0);
    descriptor.bMaxBurst = 0;
    descriptor.bLength = 5;
    assert(uvc_monitor_iso_packet_size(NULL, &ep, LIBUSB_SPEED_SUPER) == 0);
    descriptor.bLength = 6;
    parse_error = LIBUSB_ERROR_NOT_FOUND;
    const unsigned int before = freed;
    assert(uvc_monitor_iso_packet_size(NULL, &ep, LIBUSB_SPEED_SUPER) == 0);
    assert(freed == before);
    assert(uvc_monitor_iso_packet_size(NULL, &ep, LIBUSB_SPEED_UNKNOWN) == 1024);
    parse_error = LIBUSB_ERROR_NO_MEM;
    assert(uvc_monitor_iso_packet_size(NULL, &ep, LIBUSB_SPEED_UNKNOWN) == 0);
    ep.bmAttributes = LIBUSB_TRANSFER_TYPE_BULK;
    const unsigned int calls = parsed;
    assert(uvc_monitor_iso_packet_size(NULL, &ep, LIBUSB_SPEED_SUPER) == 0);
    assert(parsed == calls);
    puts("ISO packet budget: HS/FS, SS multiplier/burst, malformed/missing descriptors passed");
    return 0;
}
