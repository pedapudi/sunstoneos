#ifndef SUNSTONE_A733_NPU_PLL_H
#define SUNSTONE_A733_NPU_PLL_H
#include "a733_clock_leases.h"

#define A733_NPU_PLL_HZ UINT32_C(492000000)
#define A733_NPU_PLL_SNAPSHOTS 13u
/* Exact full CCU snapshots, in this order. No consumer register is written. */
enum a733_npu_pll_snapshot {
    A733_NPU_PLL_AHB, A733_NPU_PLL_NSI, A733_NPU_PLL_MBUS,
    A733_NPU_PLL_VE_ENC, A733_NPU_PLL_VE_DEC, A733_NPU_PLL_VE_BUS,
    A733_NPU_PLL_NPU_MODULE, A733_NPU_PLL_NPU_BUS,
    A733_NPU_PLL_DRAM, A733_NPU_PLL_DRAM_BUS,
    A733_NPU_PLL_CM_VE_DEC, A733_NPU_PLL_CM_VE_ENC, A733_NPU_PLL_CM_NPU
};
enum a733_npu_pll_mode { A733_NPU_PLL_COLD_ENABLE=1, A733_NPU_PLL_ENABLED_RETUNE };
enum a733_npu_pll_result { A733_NPU_PLL_READY, A733_NPU_PLL_BUSY,
    A733_NPU_PLL_PENDING, A733_NPU_PLL_RETAINED };
struct a733_npu_pll_config {
    uint64_t owner, handoff, deadline_us;
    enum a733_npu_pll_mode mode;
    uint32_t consumers[A733_NPU_PLL_SNAPSHOTS];
    uint32_t initial_control; /* LOCK required for retune admission; ignored only
                              * when comparing writable control fields. */
};
struct a733_npu_pll_resources {
    struct a733_clock_token clock;
    uint64_t whole_platform;
};
struct a733_npu_pll_operations {
    /* Authenticate the entire copied config against independent PRIVATE boot
     * authority before granting anything. Grant the requested common scopes
     * and a whole-platform reservation that excludes every PLL_NPU consumer,
     * secure/remote MCU writer and autonomous selector/gate writer. READY must
     * establish stable actual selected fabric parents, not just token numbers.
     * COLD_ENABLE requires pre-DRAM execution, stack, data and all callbacks in
     * owned non-NPU SRAM, safe CPU clocks, no initialized/live RAM and no other
     * active PLL consumers vulnerable to the documented enable disturbance.
     * ENABLED_RETUNE requires known independent DRAM/PHY sources and exclusion
     * of every NPU-parent route; unrelated running RAM must stay untouched.
     * A numeric handoff or observed idle register is not that authority.
     * BUSY has no effects. PENDING/RETAINED publish all partially acquired
     * tokens; subsequent calls may add but never replace an admitted token.
     * Callback metadata, all storage and reservations have platform lifetime. */
    enum a733_npu_pll_result (*acquire)(void *, const struct a733_npu_pll_config *,
        uint32_t shared, uint32_t exclusive, struct a733_npu_pll_resources *);
    /* One common register mutex covers all writers, including remote writers
     * through the authenticated handoff. enter: 1 held, 0 effect-free BUSY,
     * other values uncertain. leave: exactly 1 confirms release. */
    int (*enter)(void *, uint64_t deadline_us);
    int (*leave)(void *);
    /* Called once on terminal uncertainty, after failed is published. Retain
     * both token records, poison shared authority and NEVER perform MMIO or
     * unlock. lock_state is 1 definitely held, 2 uncertain, 0 already released
     * or never acquired. This callback must retain ownership even if registry
     * poisoning is temporarily BUSY; complete that metadata work separately. */
    void (*retain)(void *, const struct a733_npu_pll_resources *,
        const struct a733_npu_pll_resources *, unsigned lock_state);
    /* Bounded, non-reentrant callbacks. One monotonic microsecond provider
     * and lower bound must be shared with admission/mutex operations. */
    int (*now_us)(void *, uint64_t *);
    int (*read)(void *, uintptr_t, uint32_t *);
    int (*write)(void *, uintptr_t, uint32_t);
};
struct a733_npu_pll {
    struct a733_npu_pll_config config;
    struct a733_npu_pll_operations ops;
    struct a733_npu_pll_resources resources, observed;
    void *cookie;
    const struct a733_npu_pll *self;
    struct a733_clock_leases *registry;
    void *successor; /* Exact private startup object; permanent after binding. */
    unsigned transferred;
    uint64_t last_now, next_sample, settle_until;
    uint32_t shared, expected_control;
    unsigned initialized, stage, step, samples, consecutive, failed, wrote, lock_state;
};
/* Pure validation. Outputs are unchanged on refusal. Initial supported fabric
 * parents are SYS24 and PERI0; DDR/DE/NPU-selected fabric is refused. */
int a733_npu_pll_scopes(const struct a733_npu_pll_config *, uint32_t *shared, uint32_t *exclusive);
/* Zero once; metadata copied before mutating p. Serialize every poll. */
int a733_npu_pll_init(struct a733_npu_pll *, const struct a733_npu_pll_config *,
    const struct a733_npu_pll_operations *, void *);
/* Optional concrete registry provenance for startup adoption. Bind before the
 * first poll, while private and serialized. The acquisition provider must use
 * this exact registry and its monotonic counter. Binding does not create boot
 * authority or prove that callbacks use the counter correctly. Object address,
 * callbacks, config and registry are immutable thereafter; never copy/move it.
 * The bound object and whole-platform reservation outlive every successor. */
int a733_npu_pll_bind_registry(struct a733_npu_pll *,struct a733_clock_leases *);
/* Serialized read-only observation; completion is published only after final
 * readback, mutex release and deadline checks. Not a cross-core atomic API. */
int a733_npu_pll_ready(const struct a733_npu_pll *);
/* READY retains all reservations. An explicitly bound NPU startup successor
 * may atomically convert PLL ownership to shared and adopt the live token;
 * transferred then records that change. PLL polling alone never releases or
 * authorizes a service. Concrete cold boot authority remains REQUIRED.
 * No callback occurs after terminal failure or completed preparation. */
enum a733_npu_pll_result a733_npu_pll_poll(struct a733_npu_pll *);
/* Native allowlisted Device access only; not a resource/boot binding. */
int a733_npu_pll_native_read(void *, uintptr_t, uint32_t *);
int a733_npu_pll_native_write(void *, uintptr_t, uint32_t);
#endif
