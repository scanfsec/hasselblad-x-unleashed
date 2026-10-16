/* Exercise the actual Android backend with synthetic, reordered ISO URBs. */
#ifndef UVC_MONITOR_BACKEND_SOURCE
#define UVC_MONITOR_BACKEND_SOURCE "../vendor/UVCCamera/libuvccamera/src/main/jni/libusb/libusb/os/android_usbfs.c"
#endif
#include UVC_MONITOR_BACKEND_SOURCE

static int completions;
static enum libusb_transfer_status completion_status;
void usbi_log(struct libusb_context *ctx, enum libusb_log_level level,
              const char *function, const char *format, ...) {}
int usbi_handle_transfer_completion(struct usbi_transfer *transfer,
                                    enum libusb_transfer_status status)
{
    assert(((struct android_transfer_priv *)usbi_transfer_get_os_priv(transfer))->iso_urbs == NULL);
    completions++;
    completion_status = status;
    return 0;
}
int usbi_handle_transfer_cancellation(struct usbi_transfer *transfer)
{
    return usbi_handle_transfer_completion(transfer, LIBUSB_TRANSFER_CANCELLED);
}

static void run_case(const int order[3], int cancel, int removed)
{
    const int packets = 6;
    size_t size = sizeof(struct usbi_transfer) + sizeof(struct libusb_transfer)
        + packets * sizeof(struct libusb_iso_packet_descriptor) + sizeof(struct android_transfer_priv);
    struct usbi_transfer *internal = calloc(1, size);
    assert(internal);
    internal->num_iso_packets = packets;
    struct libusb_transfer *transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(internal);
    transfer->num_iso_packets = packets;
    struct android_transfer_priv *priv = usbi_transfer_get_os_priv(internal);
    priv->num_urbs = 3;
    priv->reap_action = cancel ? CANCELLED : NORMAL;
    priv->iso_urbs = calloc(3, sizeof(*priv->iso_urbs));
    struct usbfs_urb *urbs[3];
    for (int n = 0; n < 3; n++) {
        urbs[n] = priv->iso_urbs[n] = calloc(1, sizeof(struct usbfs_urb) + 2 * sizeof(struct usbfs_iso_packet_desc));
        assert(urbs[n]);
        urbs[n]->number_of_packets = 2;
        urbs[n]->status = removed && n == 2 ? -ESHUTDOWN : 0;
        for (int p = 0; p < 2; p++) urbs[n]->iso_frame_desc[p].actual_length = 100 + n * 2 + p;
    }
    pthread_mutex_init(&internal->lock, NULL);
    completions = 0;
    for (int n = 0; n < 3; n++) {
        int index = order[n];
        assert(handle_iso_completion(NULL, internal, urbs[index]) == 0);
        if (n < 2) {
            assert(completions == 0 && priv->iso_urbs != NULL);
            assert(priv->num_retired == n + 1);
        }
        assert(transfer->iso_packet_desc[index * 2].actual_length == 100 + index * 2);
        assert(transfer->iso_packet_desc[index * 2 + 1].actual_length == 101 + index * 2);
    }
    assert(completions == 1);
    assert(completion_status == (cancel ? LIBUSB_TRANSFER_CANCELLED :
                                removed ? LIBUSB_TRANSFER_NO_DEVICE : LIBUSB_TRANSFER_COMPLETED));
    pthread_mutex_destroy(&internal->lock);
    free(internal);
}
int main(void)
{
    const int orders[][3] = {{0,1,2}, {0,2,1}, {1,0,2}, {1,2,0}, {2,0,1}, {2,1,0}};
    for (int n = 0; n < 6; n++) {
        run_case(orders[n], 0, 0);
        run_case(orders[n], 0, 1);
        run_case(orders[n], 1, 0);
    }
    puts("ISO backend passed: 18 completion-order/ownership/packet-offset cases");
    return 0;
}
