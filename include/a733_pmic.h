#ifndef SUNSTONE_A733_PMIC_H
#define SUNSTONE_A733_PMIC_H
#include "a733_rail_leases.h"
#include "a733_pins.h"
#define A733_PMIC_REQUESTS 8u
enum a733_pmic_status {
    A733_PMIC_PENDING, A733_PMIC_READY, A733_PMIC_COMPLETE, A733_PMIC_BUSY,
    A733_PMIC_EXPIRED, A733_PMIC_INVALID, A733_PMIC_RETAINED
};
struct a733_pmic;
struct a733_pmic_lifetime { const struct a733_pmic *provider; uint64_t owner, identity; };
struct a733_pmic_request { struct a733_pmic_lifetime lifetime; uint64_t id, owner; };
struct a733_pmic_snapshot {
    enum a733_rail rail;
    uint8_t page_before, chip, revision, enable, selector, page_after;
    unsigned enabled, decoded;
    uint32_t programmed_uv;
};
/* Immutable privileged boot admission, authenticated before init. It excludes
 * every legacy reader, remote/secure owner, DMA/IRQ handler and autonomous PMU
 * writer. Device mappings, PL voltage/pullups and recovery must already be
 * established. While it has issued no selector write, the provider may use
 * supervised RAM recovery under docs/a733-smp-supervised.md, retaining every
 * lease until manual power removal.
 * An identity or register read does not prove these facts. This first profile
 * adopts an enabled, idle APBS1 bus: source0 detected DCXO or source4
 * PLL_REF, divider0. PLL_REF must decode to exactly24MHz from the detected DCXO.
 * The selected route, raw DCXO rate when used, and inherited TWI divisor stay
 * fixed for the lifetime; the exact baud rate must remain within 10–400 kHz.
 */
struct a733_pmic_config {
    uint64_t owner, identity, startup_deadline;
    struct a733_pin_config pins[2]; /* Exact inherited PL0/PL1 mux2 fields. */
};
struct a733_pmic_ops {
    int (*now)(void *,uint64_t *);
    int (*current)(void *); /* Exactly1 only for designated provider task. */
    int (*enter)(void *,uint64_t); /* Common PMIC mutex; 0 has no effects. */
    void (*leave)(void *);
    void (*retain)(void *);
    int (*read)(void *,uintptr_t,uint32_t *);
    int (*write)(void *,uintptr_t,uint32_t);
    int (*outside)(void *,const void *,size_t); /* Exclude adapter/lock/clock backing. */
};
struct a733_pmic_slot {
    struct a733_pmic_request request;
    struct a733_rail_borrow borrow;
    struct a733_pmic_snapshot snapshot;
    uint64_t deadline;
    unsigned state, write;
    uint8_t expected, target; /* Selector codes for a write request. */
};
struct a733_pmic {
    struct a733_pmic *self;
    struct a733_pmic_config config;
    struct a733_pmic_ops ops;
    void *cookie;
    struct a733_clock_leases *clocks;
    struct a733_rail_leases *rails;
    struct a733_pins *pins;
    struct a733_clock_token clock;
    struct a733_pin_lease_banked pin;
    struct a733_pmic_slot slots[A733_PMIC_REQUESTS];
    atomic_uint busy;
    uint64_t last_now, next_id, deadline;
    uint32_t divider, last_status, apbs1, apbs0, dcxo_hz;
    unsigned stage, held, retained, failed, active, phase, byte_index;
    uint8_t values[8]; /* Six snapshot bytes, written selector, readback. */
};
/* Zero private, disjoint storage once; all arguments/callback metadata and
 * shared managers have platform lifetime. No MMIO. All clocks use the same
 * monotonic microseconds. The actual native adapter performs direct I/O.
 * There is no close/reinit: controller, clock and pins remain platform-owned.
 */
int a733_pmic_init(struct a733_pmic *,const struct a733_pmic_config *,
    const struct a733_pmic_ops *,void *,struct a733_clock_leases *,
    struct a733_rail_leases *,struct a733_pins *);
/* identity returns READY when the metadata copy succeeds, even during startup.
 * Only live reports provider readiness. Neither performs hardware discovery.
 * live takes the provider object's try-lock and reads the power-authority
 * state word of the bound rail ledger; it queries no other lock. A provider,
 * clock registry or rail ledger fault in that word retains the provider. */
enum a733_pmic_status a733_pmic_identity(struct a733_pmic *,struct a733_pmic_lifetime *);
enum a733_pmic_status a733_pmic_live(struct a733_pmic *,const struct a733_pmic_lifetime *);
/* One designated task advances one bounded state per call. No busy waits,
 * retries, clock/reset writes or bus-recovery pulses. The only PMIC data
 * write is an admitted selector write (a733_pmic_submit_write). An
 * accepted transaction retains the common I2C mutex across worker calls.
 * Startup verifies the S_TWI0 gate and reset and the idle engines once, and
 * sets A733_POWER_PROVIDER_READY. A request is admitted once against the
 * clock registry, its rail borrow, the unchanged clock route and divisor and
 * an idle bus; each later bus phase checks only the state word. The bus-idle
 * registers are read again after every STOP.
 * Failure retains any still-owned mutex, request ticket and all leases. A
 * client-detected fault is latched immediately; the provider task must poll
 * once more to mark its own mutex retained. No client unlocks another task.
 */
enum a733_pmic_status a733_pmic_poll(struct a733_pmic *);
/* Nonblocking client metadata. Supports DCDC1/2/3/4/5/8/9, BLDO1/2/5,
 * CLDO1/2/3/5, ELDO6 and DC1SW2. Snapshot
 * reads ff/0e/0f/enable/selector/ff at address36 under one mutex. A shared or
 * exclusive lease of the rail admits a snapshot. The ledger
 * borrow prevents parent release until reap. A snapshot remains meaningful
 * only under the same still-live parent rail lease. It proves programmed
 * values, never wiring, measured voltage or settling. Public fields read-only.
 * All buffers/tokens must be disjoint from the complete provider object graph.
 */
enum a733_pmic_status a733_pmic_submit(struct a733_pmic *,uint64_t owner,
    const struct a733_rail_token *,enum a733_rail,uint64_t deadline,struct a733_pmic_request *);
/* Selector write for the CPU rails DCDC3 and DCDC5 only, under an exclusive
 * lease of that rail. target must decode to 800-1050 mV, the range of the
 * pinned CPU operating points. Under one mutex the request reads the six
 * snapshot bytes, requires the output enabled and its selector code equal to
 * expected, writes target while preserving bit7, then reads the selector back.
 * A mismatch, a readback other than the written byte or any later fault
 * retains the request: the programmed voltage is then uncertain. The result
 * snapshot carries the readback selector. It proves a programmed value only;
 * the caller waits for the output to settle. The CPU clock driver is the only
 * permitted caller: it alone keeps the voltage covering the applied CPU-L,
 * CPU-B and DSU rates, which an exclusive rail lease does not ensure. */
enum a733_pmic_status a733_pmic_submit_write(struct a733_pmic *,uint64_t owner,
    const struct a733_rail_token *,enum a733_rail,uint8_t expected,uint8_t target,
    uint64_t deadline,struct a733_pmic_request *);
enum a733_pmic_status a733_pmic_result(struct a733_pmic *,const struct a733_pmic_request *,
    struct a733_pmic_snapshot *);
enum a733_pmic_status a733_pmic_reap(struct a733_pmic *,const struct a733_pmic_request *);
/* Pure source-backed decode. Unsupported/reserved selectors leave decoded=0
 * and retain raw bytes. In particular the DCDC8/9 selector>=102 branch is not
 * guessed from the bootloader's non-power-of-two ALIGN expression. */
int a733_pmic_decode(enum a733_rail,uint8_t enable,uint8_t selector,
    struct a733_pmic_snapshot *);
#endif
