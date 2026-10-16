// SPDX-License-Identifier: GPL-2.0-only
/* Runtime workaround for the exact Eagle2 ab316 kernel; no boot-file writes. */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/usb/composite.h>
#include <linux/vermagic.h>
#include "ssp_policy.h"

static bool armed;
module_param(armed, bool, 0444);
MODULE_PARM_DESC(armed, "Enable fallback after instruction/ABI verification; default false");
static atomic_t fallbacks = ATOMIC_INIT(0);
static atomic_t config_fallbacks = ATOMIC_INIT(0);
static unsigned long uvc_alt_address;
static struct usb_gadget *selftest_gadget;
static atomic_t selftest_visits = ATOMIC_INIT(0);

static int get_fallbacks(char *buffer, const struct kernel_param *parameter)
{
    return scnprintf(buffer, PAGE_SIZE, "%d\n", atomic_read(&fallbacks));
}
static const struct kernel_param_ops count_ops = { .get = get_fallbacks };
module_param_cb(fallbacks, &count_ops, NULL, 0444);

static int get_config_fallbacks(char *buffer, const struct kernel_param *parameter)
{
    return scnprintf(buffer, PAGE_SIZE, "%d\n", atomic_read(&config_fallbacks));
}
static const struct kernel_param_ops config_count_ops = { .get = get_config_fallbacks };
module_param_cb(config_fallbacks, &config_count_ops, NULL, 0444);

static int cursor_guard(struct kprobe *probe, struct pt_regs *regs)
{
    struct usb_gadget *g = (void *)regs->regs[0];
    struct usb_function *f = (void *)regs->regs[1];
    unsigned long ss, ssp;

    if (g == READ_ONCE(selftest_gadget))
        atomic_inc(&selftest_visits);

    /* At +0x50 x1 is still usb_function, x4 is the selected array cursor.
     * x6 indicates the SS companion descriptor path. Verified below.
     */
    if (regs->regs[4] || !g || !f ||
        (unsigned long)READ_ONCE(f->set_alt) != uvc_alt_address)
        return 0;
    ss = (unsigned long)READ_ONCE(f->ss_descriptors);
    ssp = (unsigned long)READ_ONCE(f->ssp_descriptors);
    if (uvc_ssp_use_ss(1, READ_ONCE(g->speed), regs->regs[6],
                       regs->regs[4], ssp, ss)) {
        regs->regs[4] = ss;
        if (atomic_inc_return(&fallbacks) == 1 && g != READ_ONCE(selftest_gadget))
            pr_info("uvc_ssp_guard: applied first UVC SSP-to-SS cursor fallback\n");
    }
    return 0;
}

static struct kprobe guard = {
    .symbol_name = "config_ep_by_speed_and_alt",
    .offset = 0x50,
    .flags = KPROBE_FLAG_DISABLED,
    .pre_handler = cursor_guard,
};

static int config_guard_handler(struct kprobe *probe, struct pt_regs *regs)
{
    struct usb_function *f = (void *)regs->regs[19];
    struct usb_configuration *c = (void *)regs->regs[20];
    struct usb_composite_dev *cdev;
    struct usb_gadget *g;
    unsigned long ss, ssp;

    /* At usb_add_function+0xe8 x0 is f->ssp_descriptors; the next
     * instructions only test it, then set the configuration's SSP flag.
     * The descriptor field itself is never changed or aliased.
     */
    if (regs->regs[0] || !f || !c || READ_ONCE(f->config) != c ||
        (unsigned long)READ_ONCE(f->set_alt) != uvc_alt_address)
        return 0;
    cdev = READ_ONCE(c->cdev);
    if (!cdev || !(g = READ_ONCE(cdev->gadget)))
        return 0;
    ss = (unsigned long)READ_ONCE(f->ss_descriptors);
    ssp = (unsigned long)READ_ONCE(f->ssp_descriptors);
    if (uvc_ssp_config_use_ss(1, READ_ONCE(g->max_speed), regs->regs[0],
                              ssp, ss, c->superspeed, c->superspeed_plus)) {
        regs->regs[0] = ss;
        atomic_inc(&config_fallbacks);
    }
    return 0;
}

static struct kprobe config_guard = {
    .symbol_name = "usb_add_function",
    .offset = 0xe8,
    .flags = KPROBE_FLAG_DISABLED,
    .pre_handler = config_guard_handler,
};

static int fixture_set_alt(struct usb_function *f, unsigned interface, unsigned alt)
{
    return 0;
}

static void fixture_disable(struct usb_function *f)
{
}

/* Execute the real registration and configuration-count routines on
 * private objects. Nothing is registered with a UDC or gadget bus.
 */
static int __init configuration_selftest(unsigned (*count)(struct usb_composite_dev *, unsigned))
{
    struct config_fixture {
        struct usb_gadget g;
        struct usb_composite_dev cdev;
        struct usb_configuration c;
        struct usb_function f;
        struct usb_endpoint_descriptor endpoint;
        struct usb_descriptor_header *descriptors[2];
    } *t;
    int result = -EINVAL, kind, registered = 0, expected_count, expected_fix;

    t = kzalloc(sizeof(*t), GFP_KERNEL);
    if (!t)
        return -ENOMEM;
    INIT_LIST_HEAD(&t->cdev.configs);
    INIT_LIST_HEAD(&t->c.functions);
    INIT_LIST_HEAD(&t->f.list);
    list_add_tail(&t->c.list, &t->cdev.configs);
    t->cdev.gadget = &t->g;
    t->c.cdev = &t->cdev;
    t->c.label = "uvc-ssp-private-test";
    t->c.bConfigurationValue = 1;
    t->f.name = "uvc-ssp-private-test";
    t->f.disable = fixture_disable;
    t->endpoint.bLength = USB_DT_ENDPOINT_SIZE;
    t->endpoint.bDescriptorType = USB_DT_ENDPOINT;
    t->descriptors[0] = (void *)&t->endpoint;

    for (kind = 0; kind < 5; kind++) {
        t->g.max_speed = kind == 4 ? USB_SPEED_SUPER : USB_SPEED_SUPER_PLUS;
        t->g.speed = t->g.max_speed;
        t->c.fullspeed = t->c.highspeed = t->c.superspeed = t->c.superspeed_plus = 0;
        t->f.set_alt = kind == 2 ? fixture_set_alt : (void *)uvc_alt_address;
        t->f.fs_descriptors = t->descriptors;
        t->f.hs_descriptors = t->descriptors;
        t->f.ss_descriptors = kind == 3 ? NULL : t->descriptors;
        t->f.ssp_descriptors = kind == 0 ? t->descriptors : NULL;
        atomic_set(&config_fallbacks, 0);
        result = usb_add_function(&t->c, &t->f);
        if (result)
            goto out;
        registered = 1;
        expected_fix = kind == 1;
        expected_count = kind == 0 || kind == 1 || kind == 4;
        if (count(&t->cdev, USB_DT_DEVICE) != expected_count ||
            t->c.superspeed_plus != (kind == 0 || kind == 1) ||
            atomic_read(&config_fallbacks) != expected_fix ||
            t->f.ssp_descriptors != (kind == 0 ? t->descriptors : NULL) ||
            t->f.ss_descriptors != (kind == 3 ? NULL : t->descriptors)) {
            result = -EINVAL;
            goto out;
        }
        usb_remove_function(&t->c, &t->f);
        registered = 0;
    }
    result = 0;
out:
    if (registered)
        usb_remove_function(&t->c, &t->f);
    atomic_set(&config_fallbacks, 0);
    kfree(t);
    if (!result)
        pr_info("uvc_ssp_guard: real SSP configuration count selftest passed\n");
    return result;
}

/* Exercise the real kernel endpoint parser on private in-memory objects.
 * First prove the active probe executes with a valid SSP array; only then
 * test the missing-SSP case. No UDC, USB cable or real endpoint is touched.
 */
static int __init parser_selftest(void)
{
    struct fixture {
        struct usb_gadget g;
        struct usb_function f;
        struct usb_ep ep;
        struct usb_interface_descriptor interface;
        struct usb_endpoint_descriptor endpoint;
        struct usb_ss_ep_comp_descriptor companion;
        struct usb_descriptor_header *descriptors[4];
    } *t;
    int result = -EINVAL, kind, baseline;

    t = kzalloc(sizeof(*t), GFP_KERNEL);
    if (!t)
        return -ENOMEM;
    t->g.speed = USB_SPEED_SUPER_PLUS;
    t->g.max_speed = USB_SPEED_SUPER_PLUS;
    t->f.name = "uvc";
    t->f.set_alt = (void *)uvc_alt_address;
    t->interface.bLength = USB_DT_INTERFACE_SIZE;
    t->interface.bDescriptorType = USB_DT_INTERFACE;
    t->endpoint.bLength = USB_DT_ENDPOINT_SIZE;
    t->endpoint.bDescriptorType = USB_DT_ENDPOINT;
    t->endpoint.bEndpointAddress = USB_DIR_IN | 1;
    t->companion.bLength = USB_DT_SS_EP_COMP_SIZE;
    t->companion.bDescriptorType = USB_DT_SS_ENDPOINT_COMP;
    t->descriptors[0] = (void *)&t->interface;
    t->descriptors[1] = (void *)&t->endpoint;
    t->descriptors[2] = (void *)&t->companion;
    t->f.ss_descriptors = t->descriptors;
    WRITE_ONCE(selftest_gadget, &t->g);

    for (kind = 0; kind < 2; kind++) {
        memset(&t->ep, 0, sizeof(t->ep));
        t->ep.name = "ssp-guard-private-test";
        t->ep.address = USB_DIR_IN | 1;
        t->endpoint.bmAttributes = kind ? USB_ENDPOINT_XFER_INT : USB_ENDPOINT_XFER_BULK;
        t->endpoint.wMaxPacketSize = cpu_to_le16(kind ? 16 : 1024);
        t->f.ssp_descriptors = t->descriptors;
        baseline = atomic_read(&selftest_visits);
        if (config_ep_by_speed_and_alt(&t->g, &t->f, &t->ep, 0) ||
            atomic_read(&selftest_visits) != baseline + 1 ||
            t->ep.desc != &t->endpoint || t->ep.comp_desc != &t->companion)
            goto out;

        t->f.ssp_descriptors = NULL;
        baseline = atomic_read(&fallbacks);
        if (config_ep_by_speed_and_alt(&t->g, &t->f, &t->ep, 0) ||
            atomic_read(&fallbacks) != baseline + 1 ||
            t->f.ssp_descriptors || t->f.ss_descriptors != t->descriptors ||
            t->ep.desc != &t->endpoint || t->ep.comp_desc != &t->companion ||
            t->ep.maxpacket != (kind ? 16 : 1024) || t->ep.maxburst != 1)
            goto out;
    }
    result = 0;
out:
    WRITE_ONCE(selftest_gadget, NULL);
    atomic_set(&fallbacks, 0);
    kfree(t);
    if (!result)
        pr_info("uvc_ssp_guard: real SSP bulk/interrupt parser selftest passed\n");
    return result;
}

static int __init uvc_ssp_guard_init(void)
{
    struct kprobe alt = {
        .symbol_name = "uvc_function_set_alt",
        .flags = KPROBE_FLAG_DISABLED,
    };
    struct kprobe count_probe = {
        .symbol_name = "count_configs",
        .flags = KPROBE_FLAG_DISABLED,
    };
    /* Exact instruction signatures from SHA256 bcc5ae24...fc4 vendor Image. */
    const u32 expected[] = {
        0x71000c9f, 0x54000880, 0xf9400824, 0x52800006, 0xf9400081,
        0xb5000081,
    };
    const u32 config_expected[] = {
        0xf9401660, 0xb4000080, 0x39416680, 0x321d0000, 0x39016680,
        0x34000055,
    };
    const u32 config_registers[] = { 0xaa0003f4, 0xaa0103f3 };
    const u32 count_expected[] = { 0x39416424, 0x340000e3, 0x37180104 };
    const u32 count_start[] = { 0xf9400002, 0xb9405c43, 0x7100087f, 0x540001c9 };
    u32 actual[ARRAY_SIZE(expected)], alt_first, ssp_load;
    u32 config_actual[ARRAY_SIZE(config_expected)], config_regs[ARRAY_SIZE(config_registers)];
    u32 count_actual[ARRAY_SIZE(count_expected)], count_prologue[ARRAY_SIZE(count_start)];
    int result;

    BUILD_BUG_ON(offsetof(struct usb_function, ss_descriptors) != 32);
    BUILD_BUG_ON(offsetof(struct usb_function, ssp_descriptors) != 40);
    BUILD_BUG_ON(offsetof(struct usb_gadget, speed) != 88);
    BUILD_BUG_ON(offsetof(struct usb_gadget, max_speed) != 92);
    BUILD_BUG_ON(offsetof(struct usb_configuration, next_interface_id) != 88);
    BUILD_BUG_ON(USB_SPEED_SUPER_PLUS != 6);
    if (strcmp(UTS_RELEASE, "5.4.123-rt59-g5b4774899a19-ab316"))
        return -EINVAL;

    result = register_kprobe(&alt);
    if (result)
        return result;
    uvc_alt_address = (unsigned long)alt.addr;
    result = probe_kernel_read(&alt_first, alt.addr, sizeof(alt_first));
    unregister_kprobe(&alt);
    if (result || alt_first != 0xa9b37bfd)
        return -EINVAL;

    result = register_kprobe(&guard);
    if (result)
        return result;
    result = probe_kernel_read(actual, (char *)guard.addr - 16, sizeof(actual));
    if (!result)
        result = probe_kernel_read(&ssp_load, (char *)guard.addr + 0x70,
                                   sizeof(ssp_load));
    if (result || memcmp(actual, expected, sizeof(expected)) ||
        ssp_load != 0xf9401424) {
        pr_err("uvc_ssp_guard: instruction mismatch; fallback remains disabled\n");
        unregister_kprobe(&guard);
        return -EINVAL;
    }
    result = register_kprobe(&config_guard);
    if (result)
        goto out_guard;
    result = probe_kernel_read(config_actual, (char *)config_guard.addr - 4,
                               sizeof(config_actual));
    if (!result)
        result = probe_kernel_read(config_regs, (char *)config_guard.addr - 0xd4,
                                   sizeof(config_regs));
    if (result || memcmp(config_actual, config_expected, sizeof(config_expected)) ||
        memcmp(config_regs, config_registers, sizeof(config_registers))) {
        pr_err("uvc_ssp_guard: configuration instruction mismatch\n");
        result = -EINVAL;
        goto out_config;
    }
    result = register_kprobe(&count_probe);
    if (result)
        goto out_config;
    result = probe_kernel_read(count_actual, (char *)count_probe.addr + 0x68,
                               sizeof(count_actual));
    if (!result)
        result = probe_kernel_read(count_prologue, count_probe.addr, sizeof(count_prologue));
    if (result || memcmp(count_actual, count_expected, sizeof(count_expected)) ||
        memcmp(count_prologue, count_start, sizeof(count_start))) {
        pr_err("uvc_ssp_guard: configuration count instruction mismatch\n");
        result = -EINVAL;
        goto out_count;
    }
    if (armed) {
        result = enable_kprobe(&guard);
        if (!result)
            result = enable_kprobe(&config_guard);
        if (!result)
            result = parser_selftest();
        if (!result)
            result = configuration_selftest((void *)count_probe.addr);
        if (result)
            goto out_count;
    }
    unregister_kprobe(&count_probe);
    pr_info("uvc_ssp_guard: verified ab316 ABI/instructions, armed=%d\n", armed);
    return 0;
out_count:
    unregister_kprobe(&count_probe);
out_config:
    unregister_kprobe(&config_guard);
out_guard:
    unregister_kprobe(&guard);
    return result;
}

static void __exit uvc_ssp_guard_exit(void)
{
    unregister_kprobe(&config_guard);
    unregister_kprobe(&guard);
    pr_info("uvc_ssp_guard: unloaded, fallbacks=%d, config_fallbacks=%d\n",
            atomic_read(&fallbacks), atomic_read(&config_fallbacks));
}
module_init(uvc_ssp_guard_init);
module_exit(uvc_ssp_guard_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Version-checked Eagle2 UVC SSP endpoint/configuration fallback");
MODULE_INFO(vermagic, VERMAGIC_STRING);

/* Normal module metadata using the exact vendor struct module layout.
 * CONFIG_MODVERSIONS is disabled; no donor module or binary rewriting.
 */
__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
    .name = KBUILD_MODNAME,
    .init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
    .exit = cleanup_module,
#endif
};
