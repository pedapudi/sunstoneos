#ifndef SUNSTONE_A733_DISPLAY_HANDOFF_H
#define SUNSTONE_A733_DISPLAY_HANDOFF_H
#include "a733_display_pll.h"
#include "a733_media_clock.h"
#include "display_scanout.h"

struct a733_display_handoff_config {
    struct a733_media_clock *media;
    struct a733_display_pll *pll;
    struct display_request request;
    uint64_t recovery_generation;
    void *context;
    /* Effect-free, bounded current admission: 1 ready,0 pending,other fault.
     * Authenticate the complete request, initial analog-profile provenance,
     * inactive epoch, stable supplies/reference and recovery generation. Keep
     * every output consumer inactive until publication. The common token and
     * PCK lease are independently authenticated by this binding. */
    int (*authorize_handoff)(void *,const struct a733_display_handoff_config *,
                             const struct a733_clock_token *,uint64_t);
    /* acquire_resources MUST be NULL: native media/PLL ownership supplies it.
     * Remaining hooks retain their display_provider contracts, including real
     * frame/mapping/cache admission, panel sequencing and independent drain.
     * MMIO hooks use the SAME common register mutex as media and PLL setup,
     * preserve unrelated fields, and retain the mutex after uncertain effects.
     * These hooks never acquire/release/replace the supplied clock ownership.
     * now_us, registry.now and both producer clocks must observe ONE coherent
     * monotonic provider/history, including the publication sample. Independent
     * counters or offsets with matching units are insufficient. Wrappers may
     * differ, but every returned timestamp belongs to that same history. */
    struct display_provider platform;
};
struct a733_display_handoff {
    struct a733_display_handoff *self;
    struct a733_display_handoff_config config;
    struct a733_media_clock_config media_config;
    struct a733_media_clock_ops media_ops;
    void *media_cookie;
    struct a733_clock_leases *registry;
    struct a733_gpu_resource_manager *pck_manager;
    struct a733_gpu_resource_lease pck;
    unsigned pck_client;
    struct a733_display_pll_config pll_config;
    struct a733_display_pll_plan pll_plan;
    uint32_t pll_expected[A733_DISPLAY_PLL_WORDS];
    a733_clock_mask_t media_shared,media_exclusive;
    struct a733_clock_token original,clock;
    struct display_resource_token resources;
    struct display_scanout scanout;
    uint64_t last_now;
    unsigned initialized,entered,transferred,failed,retained;
};
/* Zero private storage once. Sources must have completed before binding.
 * Pure validation; no callbacks or
 * hardware. Claims each source for exactly one successor on success. Every
 * object, provider context and source remains private and alive. One designated
 * task serializes source completion, binding and all consumer operations. No
 * object may be copied or moved. Public fields, including embedded scanout, are
 * read-only; use these entry points instead of calling the embedded controller.
 * Source callbacks/configuration stay immutable. Media token/shared/exclusive
 * are refreshed immediately after publication; PLL config retains its historical
 * initialization token and must never be used as live consumer authority.
 * This binding supplies no qualified initial analog profile or panel defaults. */
int a733_display_handoff_init(struct a733_display_handoff *,const struct a733_display_handoff_config *);
int a733_display_handoff_prepare(struct a733_display_handoff *,uint64_t deadline);
int a733_display_handoff_request(struct a733_display_handoff *,enum display_scanout_action,uint64_t deadline);
/* One scanout step, with native source/token/PCK checks first. BUSY leaves the
 * operation pending. Atomic DISPLL1 publication retains VO0 and all other scopes;
 * the fresh token is persistent before any later callback. Failure retains the
 * currently live token; registry BUSY requires metadata-only polls until terminal
 * FAULT_RETAINED. No device callback runs after a fault. Frame/panel/drain owners
 * remain held. No release, rollback, retry or automatic shutdown is provided. */
enum display_scanout_state a733_display_handoff_poll(struct a733_display_handoff *);
#endif
