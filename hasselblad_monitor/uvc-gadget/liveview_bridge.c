#include "liveview_frame.h"
/* Called synchronously while the original consumer still owns its frame.
 * No pointer is retained, no camera buffer is acquired or released. */
int liveview_publish(void *shared, const uint8_t *source, uint32_t size,
                     uint32_t width, uint32_t height, uint32_t stride,
                     uint32_t uv_offset, uint32_t frame_id)
{
    struct liveview_header *header = shared;
    /* Recover an interrupted writer; completed frames must have an even seq. */
    uint32_t previous = header->sequence & ~1u;
    header->sequence = previous + 1;
    liveview_barrier();
    int result = liveview_convert(source, size, width, height, stride,
                                  uv_offset, (uint8_t *)shared + LV_OFFSET);
    if (result == 0) {
        header->frame_id = frame_id;
        header->source_width = width;
        header->source_height = height;
    }
    liveview_barrier();
    header->sequence = previous + 2;
    return result;
}
