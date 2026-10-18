#ifndef SUNSTONE_A733_NPU_PLATFORM_H
#define SUNSTONE_A733_NPU_PLATFORM_H
#include "a733_clock_leases.h"

#define A733_NPU_INITIAL_HZ UINT32_C(492000000)
#define A733_NPU_INITIAL_UV UINT32_C(800000)
enum a733_npu_platform_result { A733_NPU_PLATFORM_READY, A733_NPU_PLATFORM_BUSY,
    A733_NPU_PLATFORM_PENDING, A733_NPU_PLATFORM_RETAINED };
/* Private startup record. fabric contains AHB/NSI/MBUS selector, divisor and
 * enable fields; rate_hz gives their exact integer rates. Initial NPU rate is
 * fixed at 492 MHz. handoff must authenticate exclusive NPU/controller/domain
 * ownership, no inherited work or CPU/DMA shared-SRAM users, masked GIC97,
 * independent watchdog recovery, a supported operating-point profile, and
 * stable rail/fabric authority. A numeric identity is not physical proof.
 * Configuration and callback metadata are copied before state is modified. */
struct a733_npu_platform_config {
    uint32_t fabric[3], rate_hz[3];
    uint64_t owner, handoff, deadline_us;
};
struct a733_npu_platform_resources {
    struct a733_clock_token clock;
    uint64_t rail, pck;
    uint32_t supply_uv;
};
struct a733_npu_platform_operations {
    /* Authenticate the complete config against private startup authority.
     * Grant supplied shared scopes and exclusive NPU in ONE common registry.
     * Also reserve stable, already enabled and settled DCDC2 at 800 mV and the
     * enabled PCK clock. All PMIC/PCK/fabric writers must honor those leases.
     * READY confirms every lease and observation. BUSY has no effects and
     * leaves output unchanged. PENDING/RETAINED publish every admitted token,
     * including after partial failure. Repeated calls may add missing tokens
     * but never replace them. Never enter while holding the register mutex.
     * Reservations survive both READY and failure for platform lifetime. */
    enum a733_npu_platform_result (*acquire)(void *,
        const struct a733_npu_platform_config *, a733_clock_mask_t shared,
        struct a733_npu_platform_resources *);
    /* One common mutex serializes every register writer. enter returns 1 when
     * held, 0 for effect-free BUSY, -1 for uncertainty. leave returns exactly
     * 1 after unlocking. All callbacks are bounded and non-reentrant.
     * now_us shares one monotonic lower bound with lease admission and lock
     * operations. Deadlines are absolute microseconds; controller request
     * timeout durations are a separate interface.
     * read/write return exactly 1 after ordered 32-bit Device access. */
    int (*enter)(void *, uint64_t deadline_us);
    int (*leave)(void *);
    int (*now_us)(void *, uint64_t *);
    int (*read)(void *, uintptr_t address, uint32_t *);
    int (*write)(void *, uintptr_t address, uint32_t);
};
struct a733_npu_platform {
    struct a733_npu_platform_config config;
    struct a733_npu_platform_operations ops;
    struct a733_npu_platform_resources resources, observed;
    void *cookie;
    uint64_t last_now, settle_until;
    a733_clock_mask_t shared;
    uint32_t parent_hz, divisor, selected_mhz[3];
    unsigned initialized, stage, step, failed, wrote, adopt_on;
};
/* Pure validation; refusal preserves output. Unsupported fabric parents and
 * fractional rates are refused. Parent leases are separate from FABRIC. */
int a733_npu_platform_scopes(const struct a733_npu_platform_config *, a733_clock_mask_t *);
/* Zero private storage once, initialize before use, serialize all polls.
 * Callbacks/context and admitted resources remain live for platform lifetime. */
int a733_npu_platform_init(struct a733_npu_platform *,
    const struct a733_npu_platform_config *, const struct a733_npu_platform_operations *, void *);
/* Select before acquisition, under the same authenticated private ownership.
 * Require quiescent ON (command and status 8), the existing gated/reset local
 * baseline and abandoned shared SRAM. Preserve SRAM reset/mapping and all PCK
 * registers; no OFF transition occurs. Caller and callbacks remain immutable.
 * This does not establish quiescence or SRAM abandonment from observed bits. */
int a733_npu_platform_adopt_on(struct a733_npu_platform *);
/* Default local startup: module/bus off, core/AXI/AHB resets asserted, SRAM
 * reset deasserted, CM_NPU mode disabled and domain OFF. Existing critical
 * AHB/MBUS and PCK clock gates must already be enabled. Explicit adopt_on
 * replaces only the OFF-domain requirement and omits the power transition.
 * Inherited PLL_NPU must divide to 492 MHz; this API never retunes a shared PLL,
 * changes voltage, acknowledges NPU events, or submits device commands.
 * Each poll performs bounded work. Failure stops all subsequent callbacks;
 * resources and observed retain identities even after a malformed callback.
 * READY proves configured startup state only. It does not authorize the NPU
 * service or prove command semantics, execution, bus drain, or safe teardown.
 * There is no release/reset/retry API; external recovery must prove quiescence.
 */
enum a733_npu_platform_result a733_npu_platform_poll(struct a733_npu_platform *);
int a733_npu_platform_ready(const struct a733_npu_platform *);
#endif
