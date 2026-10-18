#ifndef SUNSTONE_A733_RADIO_PLATFORM_H
#define SUNSTONE_A733_RADIO_PLATFORM_H
#include "a733_clock_leases.h"
#include "a733_pins.h"
#include "a733_rail_leases.h"
#include "a733_pmic.h"
#include "sdio_card.h"
#include <stdatomic.h>

/* Rail identity describes authenticated wiring, not a selectable voltage.
 * The provider observes enabled 3.3V VBAT and 1.8V I/O profiles read-only. */
enum a733_radio_rail { A733_RADIO_DCDC1, A733_RADIO_DCDC8,
    A733_RADIO_BLDO5, A733_RADIO_CLDO1 };
struct a733_radio_topology {
    uint64_t identity;
    enum a733_radio_rail vbat,pg,pm,vddio;
};
struct a733_radio_platform_config {
    uint64_t owner,handoff,deadline_us,lifetime_deadline_us;
    struct a733_radio_topology topology;
    /* AHB, NSI and MBUS selector/divider snapshots and resulting rates. */
    uint32_t fabric[3],fabric_hz[3];
    /* PG0..5, PM0 and PM1, respectively. Expected input/disabled only. */
    struct a733_pin_config expected[8],desired[8];
    /* Fixed3.3V supply: supported OCR is a nonempty subset of bits20/21. */
    struct sdio_card_config card;
};
enum a733_radio_platform_result { A733_RADIO_PENDING, A733_RADIO_BUSY,
    A733_RADIO_READY, A733_RADIO_RETAINED, A733_RADIO_INVALID };
/* Bounded, non-reentrant callbacks. Physical admission authenticates the exact
 * handoff, board topology and scope token; excludes console/storage/card/DMA,
 * remote firmware, GIC handlers and all independent module reset/power writers.
 * It proves source-supported quiescence, Device mappings, retained pin-latch
 * authority, stable inherited PM2/LPO behavior and independent recovery.
 * Supervised RAM-only PIO operation may use confirmed whole-board power removal
 * as terminal recovery while retaining card, pins, clocks, buffers and all
 * borrowers. No retry/reuse or live drain follows uncertainty. This does not
 * relax any attached CE DMA/entropy or other powered-accessory requirement.
 * It does not replace actual clock/pin/rail observations and ownership. */
struct a733_radio_platform_ops {
    int (*now_us)(void *,uint64_t *);
    int (*current)(void *);
    int (*admit)(void *,const struct a733_radio_platform_config *,const struct a733_clock_token *);
    int (*enter)(void *,uint64_t);
    void (*leave)(void *);
    void (*retain)(void *);
    int (*read)(void *,uintptr_t,uint32_t *);
    int (*write)(void *,uintptr_t,uint32_t);
    int (*barrier)(void *);
};
/* Optional single-owner Bluetooth wake pins. The existing physical admission
 * callback must authenticate this exact immutable extension too. PM2 remains
 * inherited and unchanged; its reset/LPO topology is an external prerequisite. */
struct a733_radio_bluetooth_pins {
    uint64_t authority;
    struct a733_pin_config expected[2],desired[2]; /* PM3 output-high, PM4 input. */
};
/* A borrowed card lifetime is owned by this provider, including after fault.
 * A reset, power change, expiry or owner loss cannot mint a fresh generation.
 * Existing transport workers keep their complete object/buffer graph alive. */
struct a733_radio_borrow { uint64_t generation,owner; };
struct a733_radio_platform {
    struct a733_radio_platform_config config;
    struct a733_radio_bluetooth_pins bluetooth;
    struct a733_radio_platform_ops ops;
    struct a733_clock_leases *clocks;
    struct a733_pins *pins;
    struct a733_rail_leases *rails;
    struct a733_rail_token rail;
    struct a733_pmic *pmic;
    struct a733_pmic_lifetime pmic_lifetime;
    struct a733_pmic_request pmic_request;
    struct a733_pmic_snapshot snapshots[4];
    enum a733_rail rail_sequence[4];
    struct a733_clock_token clock;
    struct a733_pin_lease_banked pin_lease;
    struct sunxi_sdio host;
    struct sdio_card card;
    struct a733_radio_borrow borrower;
    void *cookie;
    atomic_uint busy;
    a733_clock_mask_t shared;
    a733_rail_mask_t rail_scopes;
    uint64_t deadline,last_now,wait_since;
    uint32_t selected_mhz[3],initial_module,initial_bus;
    unsigned initialized,stage,fault,held,retained,core_call,rail_count,rail_index,rail_retained;
};
/* Pure scope planning and init; no MMIO. Fresh private storage only. All inputs,
 * managers, provider objects and callback contexts remain disjoint and live. */
int a733_radio_platform_scopes(const struct a733_radio_platform_config *,a733_clock_mask_t *);
int a733_radio_platform_rails(const struct a733_radio_platform_config *,a733_rail_mask_t *);
int a733_radio_platform_init(struct a733_radio_platform *,const struct a733_radio_platform_config *,
    const struct a733_radio_platform_ops *,void *,struct a733_clock_leases *,
    struct a733_pins *,struct a733_rail_leases *,struct a733_pmic *);
int a733_radio_platform_bluetooth(struct a733_radio_platform *,const struct a733_radio_bluetooth_pins *);
/* Sample PM4 host-wake through the actual shared pin owner. Its level does
 * not establish controller readiness or acknowledge an interrupt. */
enum a733_radio_platform_result a733_radio_platform_bluetooth_wake(struct a733_radio_platform *,
    const struct a733_radio_borrow *,uint64_t deadline,unsigned *level);
enum a733_radio_platform_result a733_radio_platform_poll(struct a733_radio_platform *);
/* Actual owner only. Retains resource metadata and stops the host without MMIO. */
enum a733_radio_platform_result a733_radio_platform_retain(struct a733_radio_platform *);
/* Exactly one downstream owner borrows the real enumerated card. This does not
 * select firmware or establish Wi-Fi/Bluetooth readiness. Call live before
 * every downstream operation; its deadline bounds callbacks through the host.
 * A fresh operation deadline is accepted only when the host is READY (prior
 * completion consumed); active work cannot extend its deadline. The fixed
 * lifetime_deadline_us never extends.
 * No reset/prepare/direct card enumeration is allowed through borrowed objects.
 * No release/disposal API is offered until protocol consumers have real shutdown.
 * The complete module/card lifetime is retained through external recovery. */
int a733_radio_platform_borrow(struct a733_radio_platform *,uint64_t owner,
    struct a733_radio_borrow *,struct sdio_card **);
enum a733_radio_platform_result a733_radio_platform_live(struct a733_radio_platform *,
    const struct a733_radio_borrow *,uint64_t deadline);
/* Poll PM0 through the actual owned GPIO input. Output remains unchanged on
 * refusal; no interrupt acknowledgement, debounce or wake event is inferred. */
enum a733_radio_platform_result a733_radio_platform_wake(struct a733_radio_platform *,
    const struct a733_radio_borrow *,uint64_t deadline,unsigned *level);
#endif
