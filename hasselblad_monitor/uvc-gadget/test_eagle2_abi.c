#include <assert.h>
#include <linux/videodev2.h>
#include <string.h>
#include <stdint.h>
static unsigned long received_request;
static int mock_error;
static int mock_ioctl(int fd, unsigned long request, void *argument)
{
    (void)fd;
    received_request = request;
    if (request == 0xc060560fUL) {
        unsigned char *bytes = argument;
        assert(bytes[0] == 7);
        for (unsigned int i = 40; i < 48; ++i)
            assert(bytes[i] == 0);
        uint32_t memory, length;
        memcpy(&memory, bytes + 68, sizeof memory);
        memcpy(&length, bytes + 80, sizeof length);
        assert(memory == V4L2_MEMORY_MMAP && length == 614400);
        uint32_t sequence = 123, output_length = 618496;
        memcpy(bytes + 64, &sequence, sizeof sequence);
        memcpy(bytes + 80, &output_length, sizeof output_length);
        bytes[0] = 8;
    }
    return mock_error;
}
#define EAGLE2_IOCTL_IMPL mock_ioctl
#include "eagle2_v4l2_abi.h"

int main(void)
{
    struct v4l2_buffer buffer = {0};
    assert(eagle2_ioctl_request(VIDIOC_QUERYBUF) == 0xc0605609UL);
    assert(eagle2_ioctl_request(VIDIOC_QBUF) == 0xc060560fUL);
    assert(eagle2_ioctl_request(VIDIOC_DQBUF) == 0xc0605611UL);
    assert(eagle2_ioctl_request(VIDIOC_G_FMT) == 0xc1585604UL);
    assert(eagle2_ioctl_request(VIDIOC_S_FMT) == 0xc1585605UL);
    assert(eagle2_ioctl_request(VIDIOC_REQBUFS) == VIDIOC_REQBUFS);
    buffer.index = 7;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.length = 614400;
    eagle2_ioctl(0, VIDIOC_QBUF, &buffer);
    assert(received_request == VIDIOC_QBUF && buffer.index == 7);
    eagle2_vendor_abi = 1;
    eagle2_ioctl(0, VIDIOC_QBUF, &buffer);
    assert(received_request == 0xc060560fUL && buffer.index == 8);
    assert(buffer.sequence == 123 && buffer.length == 618496);
    buffer.index = 7;
    buffer.length = 614400;
    mock_error = -1;
    assert(eagle2_ioctl(0, VIDIOC_QBUF, &buffer) == -1 && buffer.index == 7);
    assert(buffer.length == 614400);
    return 0;
}
