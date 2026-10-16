#include <assert.h>
#include <dlfcn.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "libuvc/libuvc.h"
#include "iso_packet_size.h"

/* Offsets also accessed by the APK's unchanged arm64 libUVCCamera. */
_Static_assert(sizeof(uvc_frame_t) == 88, "frame ABI");
_Static_assert(offsetof(uvc_frame_t, actual_bytes) == 16, "actual bytes ABI");
_Static_assert(offsetof(uvc_frame_t, width) == 24, "width ABI");
_Static_assert(offsetof(uvc_frame_t, step) == 40, "stride ABI");

int main(void)
{
    const unsigned char extra[] = {6, LIBUSB_DT_SS_ENDPOINT_COMPANION, 0, 2, 0, 12};
    struct libusb_endpoint_descriptor ep;
    memset(&ep, 0, sizeof(ep));
    ep.bmAttributes = LIBUSB_TRANSFER_TYPE_ISOCHRONOUS;
    ep.wMaxPacketSize = 1024;
    ep.extra = extra; ep.extra_length = sizeof(extra);
    assert(uvc_monitor_iso_packet_size(NULL, &ep, LIBUSB_SPEED_SUPER) == 3072);
    assert(uvc_monitor_iso_packet_size(NULL, &ep, LIBUSB_SPEED_UNKNOWN) == 3072);
    assert(uvc_monitor_iso_packet_size(NULL, &ep, LIBUSB_SPEED_HIGH) == 3072);
    ep.extra = NULL; ep.extra_length = 0;
    ep.wMaxPacketSize = 0x1400;
    assert(uvc_monitor_iso_packet_size(NULL, &ep, LIBUSB_SPEED_HIGH) == 3072);

    uvc_frame_t *source = uvc_allocate_frame(64);
    uvc_frame_t *target = uvc_allocate_frame(48);
    assert(source && target);
    source->width = 8; source->height = 4;
    source->frame_format = UVC_FRAME_FORMAT_YUYV;
    source->step = 16; source->actual_bytes = 64;
    unsigned char *p = source->data;
    for (int i = 0; i < 64; i += 4) {
        p[i] = 80; p[i+1] = 100; p[i+2] = 120; p[i+3] = 150;
    }
    assert(uvc_yuyv2iyuv420SP(source, target) == UVC_SUCCESS);
    assert(target->width == 8 && target->height == 4 && target->data_bytes == 48);
    assert(((unsigned char *)target->data)[0] == 80);
    assert(((unsigned char *)target->data)[32] == 150); /* V */
    assert(((unsigned char *)target->data)[33] == 100); /* U */
    const unsigned char colors[][3] = {{81, 90, 240}, {41, 240, 110}}; /* Y,U,V */
    for (int color = 0; color < 2; color++) {
        for (int i = 0; i < 64; i += 4) {
            p[i] = p[i+2] = colors[color][0];
            p[i+1] = colors[color][1]; p[i+3] = colors[color][2];
        }
        assert(uvc_yuyv2iyuv420SP(source, target) == UVC_SUCCESS);
        const unsigned char *nv21 = target->data;
        assert(nv21[32] == colors[color][2] && nv21[33] == colors[color][1]);
        const float yy = 1.164f * (nv21[0] - 16);
        const float red = yy + 1.596f * (nv21[32] - 128);
        const float blue = yy + 2.018f * (nv21[33] - 128);
        if (color == 0) assert(red > 240 && blue < 10);
        else assert(blue > 240 && red < 10);
        assert(uvc_yuyv2yuv420SP(source, target) == UVC_SUCCESS);
        assert(nv21[32] == colors[color][1] && nv21[33] == colors[color][2]);
    }
    uvc_free_frame(source); uvc_free_frame(target);

    void *camera = dlopen("libUVCCamera.so", RTLD_NOW);
    if (!camera) { fprintf(stderr, "libUVCCamera load failed: %s\n", dlerror()); return 1; }
    assert(dlsym(camera, "JNI_OnLoad"));
    dlclose(camera);
    puts("Android native smoke passed: SS budgets, frame ABI, real VU/NV21 red/blue patches, camera JNI linking");
    return 0;
}
