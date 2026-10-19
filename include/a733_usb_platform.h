#ifndef SUNSTONE_A733_USB_PLATFORM_H
#define SUNSTONE_A733_USB_PLATFORM_H
#include <stdatomic.h>
#include <stdint.h>
#include "a733_clock_leases.h"

enum a733_usb_start_mode { A733_USB_ADOPT_REFERENCE=0, A733_USB_COLD_REFERENCE=1,
    A733_USB_ROM_DEVICE=2 };
/* USB0 device-only profile for the board's fixed-sink power connector.
 * The controller stays disconnected. Host, USB1, USB2 and role swaps require
 * their own controller/power bindings; this profile never drives VBUS. */
struct a733_usb_platform_config {
    uint64_t owner, handoff, deadline_us;
    uint32_t fabric[3], rate_hz[3]; /* AHB, NSI, MBUS masked profiles. */
    unsigned cold_reference; /* enum a733_usb_start_mode; retained ABI word. */
};
enum a733_usb_platform_result { A733_USB_PLATFORM_PENDING, A733_USB_PLATFORM_BUSY,
    A733_USB_PLATFORM_PREPARED, A733_USB_PLATFORM_RETAINED, A733_USB_PLATFORM_INVALID };
/* A BUSY callback guarantees that it issued no MMIO, including no read. */
#define A733_USB_PLATFORM_IO_BUSY 2
enum a733_usb_failure_kind {
    A733_USB_FAILURE_NONE, A733_USB_FAILURE_TIME, A733_USB_FAILURE_READ,
    A733_USB_FAILURE_WRITE, A733_USB_FAILURE_VALUE, A733_USB_FAILURE_CLOCK_ACQUIRE,
    A733_USB_FAILURE_CLOCK_TOKEN, A733_USB_FAILURE_AUTHORITY, A733_USB_FAILURE_LOCK,
    A733_USB_FAILURE_REFERENCE, A733_USB_FAILURE_PERI_PROFILE,
    A733_USB_FAILURE_PERI_RATE, A733_USB_FAILURE_REFERENCE_SELECT,
    A733_USB_FAILURE_DIRECT_OSC, A733_USB_FAILURE_WAIT,
    A733_USB_FAILURE_BUSY_AFTER_WRITE, A733_USB_FAILURE_STAGE
};
/* First failing condition, copied from existing operations without extra I/O.
 * wrote is the legacy attempted-write flag. Successful callbacks are counted
 * separately; neither counter establishes device completion or bus drain. */
struct a733_usb_failure {
    uintptr_t address;
    uint64_t last_us,sample_us,deadline_us;
    uint32_t mask,expected,observed;
    unsigned kind,stage,step,wrote,observed_valid;
    unsigned writes_attempted,writes_completed;
    int callback_result;
};
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
     * ROM_DEVICE additionally requires a supervised PIO ROM lifetime with no
     * prior undrained USB DMA, no concurrent ROM/firmware controller access,
     * and exclusive USB0 takeover authority. Active DMA is refused, never
     * stopped speculatively. The 150 ms detach interval is not bus-drain proof.
     * Cold reference also excludes all USB1/remote PHY consumers. Supplies
     * include VDD-USB, VCC33-USB and VCC33-18-USB; no voltage is programmed here.
     * 0 waits without mutation; other results fail. A native rail/connector
     * binding remains required: unconditional success is not authorization.
     * Authority remains held after PREPARED or RETAINED. */
    int (*authorize)(void *,const struct a733_usb_platform_config *,const struct a733_clock_token *);
    int (*enter)(void *,uint64_t); /* Common register mutex: 1 held, 0 busy. */
    void (*leave)(void *);
    void (*retain)(void *); /* Keep an uncertain held mutex; no subsequent MMIO. */
    /* 1 succeeds; IO_BUSY waits only if this poll has issued no successful
     * write. IO_BUSY after a write retains ownership rather than replaying a
     * partial reset/write sequence. Other values fault. These registers have
     * no read-to-clear semantics. A BUSY callback must issue no MMIO. */
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
    struct a733_usb_failure failure;
    uintptr_t last_address;
    uint64_t last_sample_us;
    uint32_t last_observed;
    unsigned last_observed_valid,io_busy,poll_writes,writes_attempted,writes_completed;
    int last_callback_result;
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
