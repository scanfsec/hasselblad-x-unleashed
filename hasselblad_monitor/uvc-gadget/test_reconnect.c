/* Exercise the actual gadget event handlers; only kernel syscalls are mocked. */
#include <assert.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdlib.h>
#include "eagle2_v4l2_abi.h"

static int mock_ioctl(int fd, unsigned long request, void *arg);
static void *mock_mmap(void *p, size_t n, int prot, int flags, int fd, off_t off);
static int mock_munmap(void *p, size_t n);
#define eagle2_ioctl mock_ioctl
#define mmap mock_mmap
#define munmap mock_munmap
#define main gadget_main
#include "uvc-gadget.c"
#undef main

static unsigned event_type, subscriptions;
static int pools, mapped, stops, starts, fail_stop;

static int mock_ioctl(int fd, unsigned long request, void *arg)
{
    (void)fd;
    if (request == VIDIOC_DQEVENT) {
        memset(arg, 0, sizeof(struct v4l2_event));
        ((struct v4l2_event *)arg)->type = event_type;
    } else if (request == VIDIOC_SUBSCRIBE_EVENT) {
        unsigned type = ((struct v4l2_event_subscription *)arg)->type;
        if (type == UVC_EVENT_CONNECT) subscriptions |= 1;
        if (type == UVC_EVENT_DISCONNECT) subscriptions |= 2;
    } else if (request == VIDIOC_REQBUFS) {
        struct v4l2_requestbuffers *r = arg;
        if (r->count) { assert(!pools); pools++; r->count = 2; }
        else { assert(mapped == 0); pools = 0; }
    } else if (request == VIDIOC_QUERYBUF) {
        ((struct v4l2_buffer *)arg)->length = 8;
    } else if (request == VIDIOC_STREAMON) {
        assert(pools == 1 && mapped == 2); starts++;
    } else if (request == VIDIOC_STREAMOFF) {
        stops++;
        if (fail_stop) { errno = EIO; return -1; }
    }
    return 0;
}

static void *mock_mmap(void *p, size_t n, int prot, int flags, int fd, off_t off)
{
    (void)p; (void)prot; (void)flags; (void)fd; (void)off;
    mapped++;
    return calloc(1, n);
}

static int mock_munmap(void *p, size_t n)
{
    (void)n;
    assert(p && mapped > 0); mapped--; free(p); return 0;
}

static void event(struct uvc_device *dev, unsigned type)
{
    event_type = type;
    uvc_events_process(dev);
}

int main(void)
{
    struct uvc_device dev = {0};
    dev.io = IO_METHOD_MMAP;
    dev.run_standalone = 1;
    dev.nbufs = 2;
    dev.width = dev.height = 2;
    dev.fcc = V4L2_PIX_FMT_YUYV;
    uvc_events_init(&dev);
    assert(subscriptions == 3 && !dev.iso_restart_enabled);
    for (int i = 0; i < 20; i++) {
        event(&dev, UVC_EVENT_CONNECT);
        event(&dev, UVC_EVENT_STREAMON);
        assert(dev.is_streaming && dev.mem && mapped == 2);
        int previous = starts;
        event(&dev, UVC_EVENT_STREAMON);
        assert(starts == previous);
        event(&dev, UVC_EVENT_DISCONNECT);
        assert(!dev.is_streaming && !dev.mem && !pools && !mapped);
        assert(!dev.first_buffer_queued && dev.qbuf_count == 0);
        previous = stops;
        event(&dev, UVC_EVENT_STREAMOFF);
        event(&dev, UVC_EVENT_DISCONNECT);
        assert(stops == previous); /* duplicate notifications are harmless */
    }
    event(&dev, UVC_EVENT_STREAMON);
    fail_stop = 1;
    event(&dev, UVC_EVENT_DISCONNECT);
    assert(dev.mem && mapped == 2 && pools == 1); /* don't unmap active DMA */
    assert(uvc_video_process(&dev) == 0); /* don't requeue after a failed stop */
    fail_stop = 0;
    event(&dev, UVC_EVENT_STREAMOFF);
    assert(!dev.mem && mapped == 0 && pools == 0);
    puts("PASS: 20 reconnect cycles, duplicate events, failed STREAMOFF ownership");
    return 0;
}
