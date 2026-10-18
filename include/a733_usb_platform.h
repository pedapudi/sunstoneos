#ifndef SUNSTONE_A733_USB_PLATFORM_H
#define SUNSTONE_A733_USB_PLATFORM_H
#include <stdatomic.h>
#include <stdint.h>
#include "a733_clock_leases.h"

/* USB0 device-only cold profile for the board's fixed-sink power connector.
 * The controller stays disconnected. Host, USB1, USB2 and role swaps require
 * their own controller/power bindings; this profile never drives VBUS. */
struct a733_usb_platform_config {
    uint64_t owner, handoff, deadline_us;
    uint32_t fabric[3], rate_hz[3]; /* AHB, NSI, MBUS masked profiles. */
    unsigned cold_reference; /* 1 initializes USB01_REF; 0 adopts a 24 MHz reference. */
};
enum a733_usb_platform_result { A733_USB_PLATFORM_PENDING, A733_USB_PLATFORM_BUSY,
    A733_USB_PLATFORM_PREPARED, A733_USB_PLATFORM_RETAINED, A733_USB_PLATFORM_INVALID };
struct a733_usb_platform_ops {
    int (*now_us)(void *,uint64_t *);
    /* 1 authenticates the exact owner/handoff/token, persistent rail and sink
     * session ownership, 24 MHz selected PHY reference and Device mappings. USB0 device,
     * EHCI and OHCI GIC INTIDs188/189/190 must be disabled with no active or
     * pending interrupt and no ISR executing or retaining controller access.
     * The same authority establishes
     * independent recovery, external DMA exclusion/drain and USB0 reset safety.
     * A supervised RAM-only USB0 PIO image may instead bind an explicit manual
     * whole-board power-removal plan: retain every resource on uncertainty,
     * never enable USB0 DMA, and never reuse this lifetime before confirmed
     * removal of every board power source. This supplies no autonomous cutoff
     * or live DMA-drain guarantee. Initial external DMA exclusion/drain and
     * reset safety remain required. Host/source ports need separate authority.
     * Cold reference also excludes all USB1/remote PHY consumers. Supplies
     * include VDD-USB, VCC33-USB and VCC33-18-USB; no voltage is programmed here.
     * 0 waits without mutation; other results fail. A native rail/connector
     * binding remains required: unconditional success is not authorization.
     * Authority remains held after PREPARED or RETAINED. */
    int (*authorize)(void *,const struct a733_usb_platform_config *,const struct a733_clock_token *);
    int (*enter)(void *,uint64_t); /* Common register mutex: 1 held, 0 busy. */
    void (*leave)(void *);
    void (*retain)(void *); /* Keep an uncertain held mutex; no subsequent MMIO. */
    int (*read)(void *,uintptr_t,uint32_t *);
    int (*write)(void *,uintptr_t,uint32_t);
};
struct a733_usb_platform {
    struct a733_usb_platform_config config;
    struct a733_usb_platform_ops ops;
    struct a733_clock_leases *registry;
    struct a733_clock_token token;
    void *cookie;
    atomic_uint busy;
    uintptr_t self;
    uint64_t last_now, wait_until;
    a733_clock_mask_t shared, exclusive;
    uint32_t reference; /* Admitted gate/selector; retained through verification. */
    unsigned parent_mhz[3], stage, step, initialized, fault, held, wrote;
};
/* Zero once. Objects/config/ops/registry are private, disjoint and live until
 * disposal. Callbacks are bounded and non-reentrant, return exactly 1 on
 * success and use monotonic microseconds. No polling loop or scheduler call.
 * All clock/PHY writers must use this one registry and common register mutex.
 * Upstream fabric gates and supplies are admitted, not initialized here.
 * No USB consumer may start until PREPARED; it must adopt p->token, which can
 * change during atomic reference publication. The original token is stale.
 * Moving/copying an initialized object is invalid. Completed polls authenticate
 * live registry ownership; they do not repeat hardware observations or extend
 * the initialization deadline. No release API: quiescence and disposal need
 * a later controller binding. */
int a733_usb_platform_init(struct a733_usb_platform *,const struct a733_usb_platform_config *,
    const struct a733_usb_platform_ops *,void *,struct a733_clock_leases *);
enum a733_usb_platform_result a733_usb_platform_poll(struct a733_usb_platform *);
#endif
