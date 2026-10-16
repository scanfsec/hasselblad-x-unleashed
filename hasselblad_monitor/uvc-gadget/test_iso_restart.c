#include <assert.h>

static int mock_ioctl(int fd, unsigned long request, void *argument);
#define EAGLE2_IOCTL_IMPL mock_ioctl
#define main gadget_main_for_test
#include "uvc-gadget.c"
#undef main

static unsigned long requests[32];
static unsigned int request_count, fail_at;
static int dq_error, dq_errno;
static struct v4l2_buffer queued[2];

static int mock_ioctl(int fd, unsigned long request, void *argument)
{
    (void)fd;
    assert(request_count < ARRAY_SIZE(requests));
    requests[request_count++] = request;
    if (request_count == fail_at) { errno = EIO; return -1; }
    if (request == VIDIOC_DQBUF) {
        if (dq_errno) { errno = dq_errno; return -1; }
        struct v4l2_buffer *buf = argument;
        buf->index = 0;
        buf->flags = dq_error ? V4L2_BUF_FLAG_ERROR : 0;
    } else if (request == VIDIOC_QBUF) {
        struct v4l2_buffer *buf = argument;
        assert(buf->index < 2);
        queued[buf->index] = *buf;
    } else {
        assert(request == VIDIOC_STREAMOFF || request == VIDIOC_STREAMON);
    }
    return 0;
}

static void reset(struct uvc_device *dev, struct buffer *mem, unsigned char *pixels)
{
    memset(dev, 0, sizeof(*dev));
    memset(mem, 0, sizeof(*mem) * 2);
    for (unsigned int i = 0; i < 2; ++i) {
        mem[i].start = pixels + i * 64;
        mem[i].length = 64;
    }
    dev->mem = mem; dev->nbufs = 2; dev->width = 8; dev->height = 4;
    dev->fcc = V4L2_PIX_FMT_YUYV; dev->io = IO_METHOD_MMAP;
    dev->run_standalone = 1; dev->is_streaming = 1;
    dev->iso_restart_enabled = 1;
    request_count = fail_at = 0; dq_error = dq_errno = 0;
    memset(queued, 0, sizeof(queued));
}

int main(void)
{
    struct uvc_device dev;
    struct buffer mem[2];
    unsigned char pixels[128];
    reset(&dev, mem, pixels); dq_error = 1;
    assert(uvc_video_process(&dev) == 0);
    assert(request_count == 5 && requests[0] == VIDIOC_DQBUF &&
           requests[1] == VIDIOC_STREAMOFF && requests[2] == VIDIOC_QBUF &&
           requests[3] == VIDIOC_QBUF && requests[4] == VIDIOC_STREAMON);
    assert(dev.is_streaming && dev.iso_restart_attempts == 1 && !dev.iso_restart_failed);
    assert(dev.qbuf_count == 2 && dev.dqbuf_count == 0);
    assert(queued[0].bytesused == 64 && queued[1].bytesused == 64);
    assert(!(queued[0].flags & V4L2_BUF_FLAG_ERROR));
    for (unsigned int at = 2; at <= 5; ++at) {
        reset(&dev, mem, pixels); dq_error = 1; fail_at = at;
        assert(uvc_video_process(&dev) < 0 && dev.iso_restart_failed);
        assert(request_count == at);
    }
    reset(&dev, mem, pixels); dq_errno = EIO;
    assert(uvc_video_process(&dev) == 0 && dev.iso_restart_attempts == 1);
    reset(&dev, mem, pixels); dq_errno = EAGAIN;
    assert(uvc_video_process(&dev) < 0 && !dev.iso_restart_attempts && request_count == 1);
    reset(&dev, mem, pixels);
    assert(uvc_video_process(&dev) == 0 && !dev.iso_restart_attempts && request_count == 2);
    for (unsigned int kind = 0; kind < 4; ++kind) {
        reset(&dev, mem, pixels);
        if (kind == 0) dev.iso_restart_enabled = 0;
        if (kind == 1) dev.bulk = 1;
        if (kind == 2) dev.uvc_shutdown_requested = 1;
        if (kind == 3) dev.io = IO_METHOD_USERPTR;
        assert(uvc_restart_iso(&dev) < 0 && request_count == 0);
    }
    reset(&dev, mem, pixels);
    for (unsigned int i = 0; i < 3; ++i) assert(uvc_restart_iso(&dev) == 0);
    unsigned int before = request_count;
    assert(uvc_restart_iso(&dev) < 0 && dev.iso_restart_failed && request_count == before);
    puts("ISO native restart: success, ioctl failures, host shutdown, modes and budget passed");
    return 0;
}
