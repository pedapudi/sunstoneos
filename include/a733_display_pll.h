#ifndef SUNSTONE_A733_DISPLAY_PLL_H
#define SUNSTONE_A733_DISPLAY_PLL_H
#include "a733_clock_leases.h"
#include "a733_dsi_phy.h"
#include <stdint.h>

#define A733_DISPLAY_PLL_WORDS 7u
#define A733_DISPLAY_PLL_STEPS 14u
struct a733_display_pll_profile {
    uint64_t id, generation;
    uint32_t hs_hz, ls_hz;
    /* Full initial words at PHY1 offsets104,108,10c,124,130,134,138.
     * Their qualified provenance includes analog/test/reserved controls.
     * No hardware-reset profile is supplied by this implementation. */
    uint32_t words[A733_DISPLAY_PLL_WORDS];
};
struct a733_display_pll_reservation {
    struct a733_clock_token token;
    uint64_t handoff, inactive_epoch, reference_generation, rail_generation;
};
struct a733_display_pll_step { uint32_t index, mask, value; };
struct a733_display_pll_plan {
    struct a733_dsi_pll_plan initial, target;
    struct a733_display_pll_step steps[A733_DISPLAY_PLL_STEPS];
};
/* Pure, copied output on success only; input/output storage disjoint.
 * Requires canonical exact24MHz integer initial/target plans, disabled or fully
 * enabled initial PLL, and disabled modulation. Every source-ordered factor
 * transition must satisfy oscillator/output bounds. This arithmetic check
 * cannot qualify analog state or establish initial-profile provenance. */
int a733_display_pll_plan(const struct a733_display_pll_profile *,uint32_t hs_hz,
    uint32_t ls_hz,struct a733_display_pll_plan *);

struct a733_display_pll_config;
struct a733_display_pll_ops {
    int (*now_us)(void *,uint64_t *);
    int (*enter)(void *,uint64_t deadline); /* 1 held,0 busy,other uncertain. */
    void (*leave)(void *);
    void (*retain)(void *); /* Keep common mutex owned; prohibit further MMIO. */
    int (*authorize)(void *,const struct a733_display_pll_config *);
    int (*read32)(void *,uintptr_t,uint32_t *);
    int (*write32)(void *,uintptr_t,uint32_t);
    int (*barrier)(void *);
};
struct a733_display_pll_config {
    struct a733_display_pll_ops ops;
    void *context;
    struct a733_clock_leases *registry;
    struct a733_display_pll_reservation reservation;
    struct a733_display_pll_profile initial;
    uint32_t hs_hz,ls_hz;
    uint64_t deadline;
};
enum a733_display_pll_state { A733_DISPLAY_PLL_PENDING,
    A733_DISPLAY_PLL_READY_RETAINED,A733_DISPLAY_PLL_FAULT_RETAINED };
enum a733_display_pll_error { A733_DISPLAY_PLL_OK,A733_DISPLAY_PLL_TIME,
    A733_DISPLAY_PLL_IO,A733_DISPLAY_PLL_AUTHORITY,A733_DISPLAY_PLL_INITIAL,
    A733_DISPLAY_PLL_VERIFY,A733_DISPLAY_PLL_REENTRY };
struct a733_display_pll {
    struct a733_display_pll *self;
    void *successor; /* Single serialized consumer; never copied or released. */
    unsigned transferred;
    struct a733_display_pll_config config;
    struct a733_display_pll_plan plan;
    uint32_t expected[A733_DISPLAY_PLL_WORDS];
    uint64_t last_now,wait_until;
    enum a733_display_pll_state state;
    enum a733_display_pll_error error;
    unsigned initialized,entered,held,fault,stage,index,substep,locks,writes_attempted;
};
/* Zero private storage once, privileged serialized owner; no reinitialization.
 * Config, object, registry and callback/context storage stay private, live and
 * disjoint. Borrow the exact PREPARED media VO0 token; it must exclusively own
 * VO0 and DISPLL1, with OSC/PERI0/FABRIC shared stability. All relevant writers
 * use this registry and the SAME register mutex. Token id is the generation.
 * The initial-profile id/generation must resolve to qualified A733 provenance
 * for EVERY supplied word, not a caller assertion. No qualified reset profile
 * currently exists; production authorize MUST refuse until one is established.
 * authorize also authenticates the complete copied reservation, stable24MHz
 * DCXO, rails/VO power,150MHz host clock, readable Device mappings, reset release,
 * inactive TCON/DSI/LVDS/PHY consumers and remote writers, and independent
 * recovery. All consumers stay inactive until a later atomic ownership transfer.
 * Initial enables or idle bits cannot prove transaction drain or safe reset.
 * Callbacks are bounded/non-reentrant, return exactly1 except enter as specified,
 * and provide ordered Device MMIO. barrier completes prior writes. Routine
 * holds the common register mutex across yielded delays and lock observations.
 */
int a733_display_pll_init(struct a733_display_pll *,const struct a733_display_pll_config *);
/* Bounded source-faithful enable-before-retune; no synthesized analog defaults.
 * Pre/post monotonic deadlines, full initial/readback comparison, consecutive
 * lock samples. One hardware/admission operation per poll (time callbacks and
 * registry metadata checks additional). Registry BUSY yields without MMIO.
 * Success retains the exclusive token and releases only the register mutex.
 * Fault retains any still-held mutex and the registry; metadata BUSY can delay
 * terminal reporting. A late observation after completed mutex release still
 * faults the registry and retains the exclusive token.
 * No automatic rollback/disable/reset/release/retry or consumer publication.
 * READY_RETAINED proves observed register/lock state, not electrical qualification.
 */
enum a733_display_pll_state a733_display_pll_poll(struct a733_display_pll *);
/* Serialized, callback-free completion observation. No physical revalidation. */
int a733_display_pll_ready(const struct a733_display_pll *);
#endif
