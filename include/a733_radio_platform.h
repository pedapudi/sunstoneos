#ifndef SUNSTONE_A733_RADIO_PLATFORM_H
#define SUNSTONE_A733_RADIO_PLATFORM_H
#include "a733_clock_leases.h"
#include "a733_pins.h"
#include "a733_rail_leases.h"
#include "a733_pmic.h"
#include "sdio_card.h"
#include <stdatomic.h>

/* The vendor wlan power path: VBAT is DCDC1 (3.3V, always on); PG is BLDO5 and
 * PM is CLDO1, both 1.8V; module VDDIO is one of those two. Admission refuses
 * any other map. If BLDO5 is off, the platform powers it on at 1.8V after PM1
 * is low, then reads every supply back before the 10ms interval to PM1 high.
 * DCDC1 and CLDO1 are never written. See docs/designs/radio-vendor-power-sequence.md. */
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
/* Platform startup stages, in order; READY is the last. Public so a result record can name the stage reached. */
enum a733_radio_platform_stage {
    RSTAGE_ACQUIRE, RSTAGE_RESERVE_RAILS, RSTAGE_WAIT_RAILS, RSTAGE_ADMIT, RSTAGE_RAILS, RSTAGE_BASELINE, RSTAGE_GIC_WAIT, RSTAGE_GUARD, RSTAGE_PINS, RSTAGE_POWER_ON, RSTAGE_POWER_WAIT, RSTAGE_VERIFY_RAILS, RSTAGE_SUPPLY_WAIT, RSTAGE_ENABLE_MODULE, RSTAGE_MODULE_SETTLE, RSTAGE_BUS_GATE, RSTAGE_RESET_WAIT, RSTAGE_RESET_RELEASE, RSTAGE_PREPARE, RSTAGE_PREPARING, RSTAGE_ENUMERATE, RSTAGE_ENUMERATING, RSTAGE_WIDE_READ, RSTAGE_WIDE_WRITE, RSTAGE_WIDEN, RSTAGE_WIDENING, RSTAGE_WIDE_VERIFY, RSTAGE_READY,
    /* Shutdown, in order: PM1 low, controller quiesce, module and bus clocks off, BLDO5 off, DOWN. */
    RSTAGE_SHUTDOWN_PM1, RSTAGE_SHUTDOWN_CONTROLLER, RSTAGE_SHUTDOWN_CONTROLLER_WAIT, RSTAGE_SHUTDOWN_CLOCKS,
    RSTAGE_SHUTDOWN_POWER, RSTAGE_SHUTDOWN_POWER_WAIT, RSTAGE_DOWN,
    /* Failure shutdown: after the power-on writes, a failure first drives PM1 low, stops the SDC1 clocks and
     * turns BLDO5 off, each read back where the hardware still answers, and only then retains. */
    RSTAGE_ABORT_PM1, RSTAGE_ABORT_CLOCKS, RSTAGE_ABORT_POWER, RSTAGE_ABORT_POWER_WAIT
};
enum a733_radio_platform_result { A733_RADIO_PENDING, A733_RADIO_BUSY,
    A733_RADIO_READY, A733_RADIO_RETAINED, A733_RADIO_INVALID, A733_RADIO_DOWN };
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
/* Optional installed-board authority to hold Bluetooth in reset after its
 * protocol owner releases the module. The inherited PM2 output and fitted
 * reset/LPO route require independent evidence. Legacy profiles omit it. */
struct a733_radio_bluetooth_stop {
    uint64_t authority,topology;
    uint8_t wiring_evidence[32];
    struct a733_pin_config expected;
    uint8_t inherited_level;
    uint32_t reserved;
};
_Static_assert(sizeof(struct a733_radio_bluetooth_stop)==56,"Bluetooth stop contract geometry");
/* A borrowed card lifetime is owned by this provider, including after fault.
 * A reset, power change, expiry or owner loss cannot mint a fresh generation.
 * Existing transport workers keep their complete object/buffer graph alive. */
struct a733_radio_borrow { uint64_t generation,owner; };
struct a733_radio_platform {
    struct a733_radio_platform_config config;
    struct a733_radio_bluetooth_pins bluetooth;
    struct a733_radio_bluetooth_stop bluetooth_stop;
    struct a733_pin_lease_banked bluetooth_stop_lease;
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
    uint32_t selected_mhz[3],initial_module,initial_bus,running_module;
    unsigned initialized,stage,fault,held,retained,core_call,rail_count,rail_index,rail_retained;
    unsigned four_bit,card_pending,bus_interface,power_on,rail_verify,guard_done,first_run_guard;
    /* already_on: the first-run guard found BLDO5 on at 1.8V with PG at 1.8V (a reset left it on), so the two
     * power-on writes are skipped. aborting/abort_done/abort_flags: the failure shutdown (bit0 ran, bit1 PM1 low,
     * bit2 clocks off, bit3 BLDO5 off; all of bits 1 to 3 that applied read back matched sets bit4). */
    unsigned already_on,aborting,abort_done,abort_flags,abort_from;
    /* Where the first failure was decided: the source line of the fail call and the stage then. Zero while healthy. */
    unsigned fail_line,fail_stage;
    /* The first-run guard refused before any write: nothing is powered and nothing needs undoing, so the failure
     * releases the register lock instead of retaining it and the lifetime can end normally. fail_line then holds
     * 0x400 | PG bits (13:12 of GPIO_POW_VAL) << 4 | the unmet conditions (1 PG, 2 DCDC1, 4 BLDO5, 8 CLDO1). */
    unsigned refused;
    /* GPIO_POW_MOD_SEL, GPIO_POW_VAL, SGPIO_POW_MOD_SEL, SGPIO_POW_VAL as read
     * by the first-run guard; valid once guard_done is set. */
    uint32_t pad_power[4];
    uint64_t stop_deadline;
    struct sunxi_sdio_result stop_result;
    unsigned stop_requested,stop_phase,stop_completed,stop_unprepared,stop_bus,stop_powered,peer_stopped,stop_released,stop_from;
};
/* The failure left nothing powered, so no reset is needed to release anything: the first-run guard refused before
 * any write, or the failure shutdown finished with every readback matched (PM1 low, clocks off, BLDO5 off). Such a
 * failure keeps the board alive: it leaves the register lock and the leases as they are, sets no failure bit, and the
 * session ends with a normal return and its record. A watchdog reset loses DRAM, and with it the record. */
static inline int a733_radio_platform_clean(const struct a733_radio_platform *p)
{return p->refused||(p->abort_done&&(p->abort_flags&16u));}
/* Pure scope planning and init; no MMIO. Fresh private storage only. All inputs,
 * managers, provider objects and callback contexts remain disjoint and live. */
int a733_radio_platform_scopes(const struct a733_radio_platform_config *,a733_clock_mask_t *);
int a733_radio_platform_rails(const struct a733_radio_platform_config *,a733_rail_mask_t *);
int a733_radio_platform_init(struct a733_radio_platform *,const struct a733_radio_platform_config *,
    const struct a733_radio_platform_ops *,void *,struct a733_clock_leases *,
    struct a733_pins *,struct a733_rail_leases *,struct a733_pmic *);
int a733_radio_platform_bluetooth(struct a733_radio_platform *,const struct a733_radio_bluetooth_pins *);
int a733_radio_platform_bluetooth_stop(struct a733_radio_platform *,const struct a733_radio_bluetooth_stop *);
/* Owner-only permanent stop, including partial startup. The exact borrow is
 * required once issued; an empty borrow is valid only before issue. Poll drains
 * the one accepted host operation, then performs checked shutdown. All borrowed
 * storage remains pinned. Repeated admission cannot extend the first deadline. */
int a733_radio_platform_stop(struct a733_radio_platform *,const struct a733_radio_borrow *,uint64_t deadline);
int a733_radio_platform_stop_release(struct a733_radio_platform *,const struct a733_radio_borrow *);
enum a733_radio_platform_result a733_radio_platform_stop_held(struct a733_radio_platform *,const struct a733_radio_borrow *);
/* Request four-bit default speed before the first poll. Adds the shared PERI0
 * lease scope and its 400 MHz branch to every clock observation. After
 * enumeration of a full-speed card, the platform sets CCCR bus width to four
 * bits (read-after-write), switches SMHC1 to PERI0 400 MHz / 8 under the register
 * lock, verifies the controller, and rereads CCCR before READY: a 25 MHz card
 * clock. A low-speed card stays at one bit. Physically unqualified. */
int a733_radio_platform_four_bit(struct a733_radio_platform *);
/* Request the first-run guard before the first poll (kept out of the wire
 * profile's configuration). Before any PMIC write the platform also observes
 * DCDC8 (recorded, never required) and the pad power words, and proceeds only
 * if BLDO5 is off, PG reads unsupplied, CLDO1 is on at 1.8V and DCDC1 is on at
 * selector code 23. Otherwise it keeps every observation and retains with no
 * PMIC write. */
int a733_radio_platform_first_run_guard(struct a733_radio_platform *);
/* Shut the module down, with a readback for every step, so that the board can
 * return normally. Only from READY, by the downstream owner that holds the borrow
 * token and has stopped all card use: the host controller must be idle. Poll
 * then runs, in order, PM1 low (read back from DATA), the controller quiesce (card
 * clock stopped, interrupts masked, read back), the SDC1 module clock gate off and
 * the bus gate and reset asserted (read back, INTID 194 still masked), and, if this
 * run powered BLDO5 on, BLDO5 off with its enable byte read back. The result is
 * A733_RADIO_DOWN when every readback matched; any mismatch retains. After DOWN the
 * card and borrow are unusable. The deadline must lie after the current time and
 * within the lifetime deadline. Physical operation unqualified. */
int a733_radio_platform_shutdown(struct a733_radio_platform *,const struct a733_radio_borrow *,uint64_t deadline);
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
