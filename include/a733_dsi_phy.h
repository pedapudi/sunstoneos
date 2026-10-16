#ifndef SUNSTONE_A733_DSI_PHY_H
#define SUNSTONE_A733_DSI_PHY_H
#include <stdint.h>

struct a733_dsi_pll_plan {
    uint32_t hs_hz, ls_hz, vco_hz, factors;
};
/* Exact integer plans for the published clock path with a stable 24 MHz
 * reference. Refuses rounding, field truncation, modulation and rates outside
 * the manual's VCO/output limits. Output is unchanged on refusal. A successful
 * plan does not establish a qualified lane rate or program any clock. */
int a733_dsi_pll_plan(uint32_t hs_hz, uint32_t ls_hz, struct a733_dsi_pll_plan *);

struct a733_dsi_phy_config;
struct a733_dsi_phy_ops {
    int (*admit)(void *, const struct a733_dsi_phy_config *);
    int (*read32)(void *, uintptr_t, uint32_t *);
    int (*write32)(void *, uintptr_t, uint32_t);
    int (*barrier)(void *);
    int (*now_us)(void *, uint64_t *);
};
struct a733_dsi_phy_config {
    struct a733_dsi_phy_ops ops;
    void *context;
    uint64_t lease, deadline;
    uint32_t hs_hz, ls_hz;
    unsigned controller, lanes;
};
enum a733_dsi_phy_state { A733_DSI_PHY_PENDING, A733_DSI_PHY_READY,
    A733_DSI_PHY_RETAINED };
enum a733_dsi_phy_error { A733_DSI_PHY_OK, A733_DSI_PHY_TIME,
    A733_DSI_PHY_IO, A733_DSI_PHY_AUTHORITY, A733_DSI_PHY_INITIAL_STATE,
    A733_DSI_PHY_VERIFY, A733_DSI_PHY_REENTRY };
struct a733_dsi_phy {
    struct a733_dsi_phy_config config;
    struct a733_dsi_pll_plan pll;
    uint64_t last_now, wait_until;
    uint32_t previous, expected;
    enum a733_dsi_phy_state state;
    enum a733_dsi_phy_error error;
    unsigned initialized, entered, phase, index, substep, locks, writes_attempted;
};
/* Privileged serialized owner, zero unused storage, no reinitialization.
 * Copies configuration. All callback/context storage remains private, disjoint
 * and live. Callbacks return exactly 1, are bounded and non-reentrant, and
 * provide ordered Device memory access. The barrier completes prior writes.
 * admit authenticates combined host/PHY/PLL ownership for the full lifetime:
 * stable VO power/rails, 24 MHz PLL reference, 150 MHz controller clock, pins,
 * shared gate/reset release, and exclusion of TCON/video/LVDS/remote writers.
 * The host and PHY must be disabled; an already enabled, locked PLL with the
 * requested exact factors and no modulation must have been handed over.
 * This component NEVER activates, resets or retunes that PLL. Cold PLL startup
 * needs a separately verified sequence. Reset is shared with the host/PHY.
 * The platform provider is not implemented here. Independent watchdog recovery
 * is required before physical use. No panel power or GPIO sequence is implied.
 */
int a733_dsi_phy_init(struct a733_dsi_phy *, const struct a733_dsi_phy_config *);
/* At most one MMIO/barrier/admission operation per poll. Uses absolute monotonic
 * microsecond deadlines, filtered inherited PLL lock, source analog delays and
 * masked writes with readback. Every fault retains all resources; terminal
 * polls are quiet. READY proves register preparation, not electrical signaling.
 * There is no close, rollback, reset, release or retry operation. */
enum a733_dsi_phy_state a733_dsi_phy_poll(struct a733_dsi_phy *);
/* Pure composition predicate for the DSI command authority provider. It does
 * not authenticate a live platform lease by itself. No callbacks or I/O. */
int a733_dsi_phy_ready(const struct a733_dsi_phy *, unsigned controller,
                       unsigned lanes, uint64_t lease);
#endif
