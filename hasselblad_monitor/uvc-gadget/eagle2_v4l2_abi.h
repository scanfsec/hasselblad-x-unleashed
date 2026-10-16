#ifndef EAGLE2_V4L2_ABI_H
#define EAGLE2_V4L2_ABI_H
#include <string.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

/* Verified from Eagle2 kernel v4l2_ioctls at Image offset 0xbfda10.
 * __fill_v4l2_buffer at 0x768f10 proves an extra 8-byte timestamp field at
 * offset 40. timecode/sequence/memory/m/length move to 48/64/68/72/80.
 * Explicit opt-in keeps other Linux devices on their native ABI.
 */
static int eagle2_vendor_abi;

static unsigned long eagle2_ioctl_request(unsigned long request)
{
    unsigned int size;
    switch (request) {
    case VIDIOC_QUERYBUF:
    case VIDIOC_QBUF:
    case VIDIOC_DQBUF:
        size = 96;
        break;
    case VIDIOC_G_FMT:
    case VIDIOC_S_FMT:
        size = 344;
        break;
    default:
        return request;
    }
    return _IOC(_IOC_DIR(request), _IOC_TYPE(request), _IOC_NR(request), size);
}

#ifndef EAGLE2_IOCTL_IMPL
#define EAGLE2_IOCTL_IMPL ioctl
#endif

static int eagle2_ioctl(int fd, unsigned long request, void *argument)
{
    unsigned long translated = eagle2_vendor_abi ? eagle2_ioctl_request(request) : request;
    union {
        unsigned long long alignment;
        unsigned char bytes[344];
    } payload = {0};
    int result;
    if (translated == request)
        return EAGLE2_IOCTL_IMPL(fd, request, argument);
    if (_IOC_SIZE(translated) == 96) {
        _Static_assert(sizeof(struct v4l2_buffer) == 88, "requires 64-bit Linux buffer ABI");
        memcpy(payload.bytes, argument, 40);
        memcpy(payload.bytes + 48, (unsigned char *)argument + 40, 48);
    } else {
        memcpy(payload.bytes, argument, _IOC_SIZE(request));
    }
    result = EAGLE2_IOCTL_IMPL(fd, translated, payload.bytes);
    if (result >= 0) {
        if (_IOC_SIZE(translated) == 96) {
            memcpy(argument, payload.bytes, 40);
            memcpy((unsigned char *)argument + 40, payload.bytes + 48, 48);
        } else {
            memcpy(argument, payload.bytes, _IOC_SIZE(request));
        }
    }
    return result;
}

#endif
