/* Exercise protocol handling without a camera or USB controller. */
#define main uvc_application_main
#include "uvc-gadget.c"
#undef main
#include <assert.h>

int main(void)
{
    struct uvc_device dev = {0};
    struct usb_ctrlrequest request = {0};
    struct uvc_request_data response = {0};
    dev.control_intf = 3;
    dev.streaming_intf = 4;
    request.bRequestType = USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE;
    request.bRequest = UVC_GET_INFO;
    request.wValue = UVC_VS_PROBE_CONTROL << 8;
    request.wIndex = 4;
    uvc_events_process_class(&dev, &request, &response);
    assert(response.length == 1 && response.data[0] == 3);

    request.bRequest = UVC_GET_LEN;
    memset(&response, 0, sizeof response);
    uvc_events_process_class(&dev, &request, &response);
    assert(response.length == 2);
    assert(response.data[0] == sizeof(struct uvc_streaming_control));
    assert(response.data[1] == 0);

    request.wIndex = 1;
    response.length = -EL2HLT;
    uvc_events_process_class(&dev, &request, &response);
    assert(response.length == -EL2HLT);

    dev.control_intf = 0;
    dev.streaming_intf = 1;
    request.bRequest = UVC_GET_INFO;
    uvc_events_process_class(&dev, &request, &response);
    assert(response.length == 1 && response.data[0] == 3);
    assert(uvc_speed_from_index(0) == USB_SPEED_FULL);
    assert(uvc_speed_from_index(1) == USB_SPEED_HIGH);
    assert(uvc_speed_from_index(2) == USB_SPEED_SUPER);
    assert(uvc_speed_from_index(3) == USB_SPEED_UNKNOWN);
    dev.bulk = 0;
    dev.maxpkt = 1024;
    dev.mult = 2;
    uvc_fill_streaming_control(&dev, &dev.probe, 0, 0);
    assert(dev.probe.dwMaxPayloadTransferSize == 3072);
    assert(dev.probe.dwMaxVideoFrameSize == 640 * 480 * 2);
    dev.bulk = 1;
    uvc_fill_streaming_control(&dev, &dev.probe, 0, 0);
    /* Native vendor encoder emits header {2, EOH|EOF|FID}, then 614400
     * pixel bytes in one short bulk payload. A host read must hold both.
     */
    const unsigned char wire_header[] = {2, 0x82};
    assert(dev.probe.dwMaxPayloadTransferSize >=
           sizeof(wire_header) + dev.probe.dwMaxVideoFrameSize);
    assert(dev.probe.dwMaxPayloadTransferSize == 614402);
    assert(dev.probe.dwMaxVideoFrameSize == 614400);
    request.wIndex = 1;
    request.bRequest = UVC_GET_CUR;
    memset(&response, 0, sizeof response);
    uvc_events_process_class(&dev, &request, &response);
    const struct uvc_streaming_control *advertised = (const void *)response.data;
    assert(advertised->dwMaxPayloadTransferSize == 614402);
    assert(advertised->dwMaxVideoFrameSize == 614400);
    puts("UVC protocol/speed and bulk wire-boundary checks passed");
    return 0;
}
