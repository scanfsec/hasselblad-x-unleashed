/* Link against the actual rebuilt JNI library; fail before any USB access. */
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include "utilbase.h"
#include "UVCPreview.h"

static int mode;
static uvc_frame_desc_t descriptor;
static jint attach(JavaVM *, JNIEnv **env, void *) { *env = NULL; return JNI_OK; }
static jint detach(JavaVM *) { return JNI_OK; }

extern "C" uvc_error_t uvc_get_stream_ctrl_format_size_fps(uvc_device_handle_t *,
    uvc_stream_ctrl_t *, enum uvc_frame_format, int, int, int, int)
{
    return mode == 0 ? UVC_ERROR_NO_DEVICE : UVC_SUCCESS;
}
extern "C" uvc_error_t uvc_get_frame_desc(uvc_device_handle_t *,
    uvc_stream_ctrl_t *, uvc_frame_desc_t **out)
{
    descriptor.wWidth = 640; descriptor.wHeight = 480;
    *out = &descriptor; return UVC_SUCCESS;
}
extern "C" uvc_error_t uvc_start_streaming_bandwidth(uvc_device_handle_t *,
    uvc_stream_ctrl_t *, uvc_frame_callback_t *, void *, float, uint8_t)
{
    return mode == 2 ? UVC_SUCCESS : UVC_ERROR_IO;
}
extern "C" void uvc_stop_streaming(uvc_device_handle_t *) {}
extern "C" void ANativeWindow_acquire(ANativeWindow *) {}
extern "C" void ANativeWindow_release(ANativeWindow *) {}
extern "C" int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *, int32_t, int32_t, int32_t) { return 0; }
extern "C" int32_t ANativeWindow_lock(ANativeWindow *, ANativeWindow_Buffer *, ARect *) { return -1; }

int main()
{
    JNIInvokeInterface table = {};
    table.AttachCurrentThread = attach;
    table.DetachCurrentThread = detach;
    JavaVM vm = {&table};
    setVM(&vm);
    for (mode = 0; mode < 3; mode++) {
        UVCPreview preview(NULL);
        assert(preview.stopPreview() == 0);
        for (int cycle = 0; cycle < 100; cycle++) {
            preview.setPreviewDisplay(reinterpret_cast<ANativeWindow *>(1));
            assert(preview.startPreview() == 0);
            usleep(1000);
            assert(preview.stopPreview() == 0);
            assert(preview.stopPreview() == 0);
        }
    }
    puts("PASS: 300 negotiation failure, stream-start failure, and active-thread stop/reuse cycles");
    return 0;
}
