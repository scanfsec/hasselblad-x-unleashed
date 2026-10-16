// SPDX-License-Identifier: GPL-2.0-only
/* Experimental, exact Eagle2 ab316 runtime workaround. No boot or USB setup. */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/usb/composite.h>
#include <linux/vermagic.h>
#include "iso_policy.h"

static bool armed;
module_param(armed, bool, 0444);
static atomic_t recoveries = ATOMIC_INIT(0);
static atomic_t visits = ATOMIC_INIT(0);
static unsigned long completion_address;
static void *fixture_video;

static int get_recoveries(char *buffer, const struct kernel_param *parameter)
{
    return scnprintf(buffer, PAGE_SIZE, "%d\n", atomic_read(&recoveries));
}
static const struct kernel_param_ops count_ops = { .get = get_recoveries };
module_param_cb(recoveries, &count_ops, NULL, 0444);

static int completion_guard(struct kprobe *probe, struct pt_regs *regs)
{
    struct usb_ep *ep = (void *)regs->regs[0];
    struct usb_request *req = (void *)regs->regs[1];
    const struct usb_endpoint_descriptor *desc;
    int iso_in;

    if (!ep || !req)
        return 0;
    if (READ_ONCE(req->context) == READ_ONCE(fixture_video))
        atomic_inc(&visits);
    desc = READ_ONCE(ep->desc);
    iso_in = desc && usb_endpoint_xfer_isoc(desc) && usb_endpoint_dir_in(desc);
    if (uvc_iso_recoverable((unsigned long)READ_ONCE(req->complete) == completion_address,
                            !!READ_ONCE(req->context), iso_in,
                            READ_ONCE(req->status), (int)regs->regs[3])) {
        /* Only the already-loaded status register changes. The request status,
         * payload and ownership remain intact. Normal callback processing can
         * recycle/requeue the request instead of cancelling every video buffer.
         * The lost USB payload is not claimed to have been delivered.
         */
        regs->regs[3] = 0;
        atomic_inc(&recoveries);
    }
    return 0;
}

static struct kprobe guard = {
    .flags = KPROBE_FLAG_DISABLED,
    .pre_handler = completion_guard,
};

/* Vendor video object offsets verified from completion, queue_head and
 * queue_cancel instructions. Empty private queue means no USB operation.
 */
static int __init completion_selftest(int recover)
{
    struct fixture {
        u8 video[0x700] __aligned(8);
        struct usb_request req;
        struct usb_ep ep;
        struct usb_endpoint_descriptor desc;
    } *t;
    struct list_head *free_list, *irq_list;
    u32 *flags, *used;
    int result = -EINVAL, kind;

    t = kzalloc(sizeof(*t), GFP_KERNEL);
    if (!t)
        return -ENOMEM;
    free_list = (void *)(t->video + 0x98);
    irq_list = (void *)(t->video + 0x6c8);
    flags = (void *)(t->video + 0x688);
    used = (void *)(t->video + 0x690);
    t->ep.desc = &t->desc;
    t->desc.bLength = USB_DT_ENDPOINT_SIZE;
    t->desc.bDescriptorType = USB_DT_ENDPOINT;
    t->desc.bEndpointAddress = USB_DIR_IN | 1;
    t->desc.bmAttributes = USB_ENDPOINT_XFER_ISOC;
    t->req.context = t->video;
    t->req.complete = (void *)completion_address;
    WRITE_ONCE(fixture_video, t->video);

    for (kind = 0; kind < (recover ? 2 : 1); kind++) {
        int previous = atomic_read(&recoveries);
        int seen = atomic_read(&visits);
        INIT_LIST_HEAD(free_list);
        INIT_LIST_HEAD(irq_list);
        INIT_LIST_HEAD(&t->req.list);
        spin_lock_init((spinlock_t *)(t->video + 0xa8));
        spin_lock_init((spinlock_t *)(t->video + 0x698));
        *flags = 0;
        *used = 7;
        t->req.status = kind ? -EXDEV : 0;
        t->req.complete(&t->ep, &t->req);
        if (free_list->next != &t->req.list || free_list->prev != &t->req.list ||
            t->req.list.next != free_list || t->req.list.prev != free_list ||
            *used != 7 || *flags != 4 ||
            t->req.status != (kind ? -EXDEV : 0) || t->req.context != t->video ||
            atomic_read(&recoveries) != previous + kind ||
            (recover && atomic_read(&visits) != seen + 1))
            goto out;
    }
    result = 0;
out:
    WRITE_ONCE(fixture_video, NULL);
    atomic_set(&recoveries, 0);
    atomic_set(&visits, 0);
    kfree(t);
    if (!result)
        pr_info("uvc_iso_recover: real private completion selftest passed (recover=%d)\n", recover);
    return result;
}

static int __init uvc_iso_recover_init(void)
{
    const u32 completion[] = { 0xa9bc7bfd, 0x910003fd, 0xa90153f3, 0xf9001bf7,
        0xaa0103f7, 0xa9025bf5, 0xb9405423, 0xf9401c33, 0x3101b07f,
        0x9103c274, 0x54000780, 0x34000483 };
    const u32 head[] = { 0x91176002, 0xaa0003e1, 0xf942ec00, 0xeb00005f };
    u32 actual[ARRAY_SIZE(completion)], head_actual[ARRAY_SIZE(head)];
    unsigned long base;
    int result;

    BUILD_BUG_ON(offsetof(struct usb_request, context) != 56);
    BUILD_BUG_ON(offsetof(struct usb_request, status) != 84);
    BUILD_BUG_ON(offsetof(struct usb_request, list) != 64);
    BUILD_BUG_ON(sizeof(spinlock_t) > 0x30);
    if (strcmp(UTS_RELEASE, "5.4.123-rt59-g5b4774899a19-ab316"))
        return -EINVAL;
    /* The kernel also has a different host-driver uvc_video_complete symbol.
     * Resolve the gadget copy by its exact Image offset from an exported anchor.
     */
    base = (unsigned long)config_ep_by_speed_and_alt - 0x6ed83c;
    completion_address = base + 0x70e884;
    result = probe_kernel_read(actual, (void *)completion_address, sizeof(actual));
    if (!result)
        result = probe_kernel_read(head_actual, (void *)(base + 0x70dec0), sizeof(head_actual));
    if (result || memcmp(actual, completion, sizeof(completion)) ||
        memcmp(head_actual, head, sizeof(head)))
        return -EINVAL;
    guard.addr = (void *)(completion_address + 0x1c);
    result = register_kprobe(&guard);
    if (result)
        return result;
    result = completion_selftest(0);
    if (!result && armed)
        result = enable_kprobe(&guard);
    if (!result && armed)
        result = completion_selftest(1);
    if (result) {
        unregister_kprobe(&guard);
        return result;
    }
    pr_info("uvc_iso_recover: exact ab316 completion verified, armed=%d\n", armed);
    return 0;
}

static void __exit uvc_iso_recover_exit(void)
{
    unregister_kprobe(&guard);
}
module_init(uvc_iso_recover_init);
module_exit(uvc_iso_recover_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Exact Eagle2 ab316 UVC ISO missed-slot recovery; temporary");
MODULE_INFO(vermagic, VERMAGIC_STRING);
