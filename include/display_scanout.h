#ifndef SUNSTONE_DISPLAY_SCANOUT_H
#define SUNSTONE_DISPLAY_SCANOUT_H
#include "a733_display_engine.h"
#include "a733_dsi_phy.h"

/* Scope bits name complete ownership domains, not single enable bits. */
#define DISPLAY_SCOPE_DE_ALL          (1u<<0)
#define DISPLAY_SCOPE_VO0_SHARED      (1u<<1)
#define DISPLAY_SCOPE_LCD1            (1u<<2)
#define DISPLAY_SCOPE_DSI1_PHY1_PLL1   (1u<<3)
#define DISPLAY_SCOPE_PANEL           (1u<<4)
#define DISPLAY_SCOPE_MEMORY_ROUTE    (1u<<5)
#define DISPLAY_SCOPE_FABRIC_CLOCKS   (1u<<6)
#define DISPLAY_SCOPE_RECOVERY        (1u<<7)
#define DISPLAY_REQUIRED_SCOPES       0xffu
struct display_frame {
    uintptr_t cpu_address;
    uint64_t device_address, mapped_bytes, frame_id;
    uint32_t pitch;
};
struct display_request {
    uint64_t owner, board_id, panel_id;
    struct a733_tcon_lcd_timing timing;
    struct display_frame frame;
};
struct display_resource_token {
    uint64_t id, generation, owner, board_id, panel_id, recovery_generation;
    uint32_t scopes, reference_hz, host_hz, hs_hz, ls_hz;
};
enum display_cache_state { DISPLAY_CACHE_UNOWNED, DISPLAY_CACHE_IMMUTABLE_DEVICE_READ };
struct display_frame_token {
    uint64_t id, generation, owner, frame_id, mapping_generation, visibility_generation;
    uintptr_t cpu_address;
    uint64_t device_address, mapped_bytes;
    uint32_t pitch;
    enum display_cache_state cache_state;
};
enum display_proof_kind { DISPLAY_PROOF_NONE, DISPLAY_PROOF_DE_DRAIN, DISPLAY_PROOF_OUTPUT_QUIET };
struct display_proof {
    uint64_t id, generation, resource_id, resource_generation, frame_id, frame_generation;
    uint64_t completed_us;
    enum display_proof_kind kind;
};
struct display_ownership {
    struct display_resource_token resources;
    struct display_frame_token frame;
    struct display_proof de_drain, output_quiet;
};
enum display_scanout_state { DISPLAY_SCANOUT_NEW, DISPLAY_SCANOUT_PENDING,
    DISPLAY_SCANOUT_READY, DISPLAY_SCANOUT_ACTIVE, DISPLAY_SCANOUT_STOPPED_RETAINED,
    DISPLAY_SCANOUT_FAULT_RETAINED };
enum display_phase {
    DISPLAY_ACQUIRE_RESOURCES, DISPLAY_ACQUIRE_FRAME, DISPLAY_PREPARE_PHY,
    DISPLAY_PREPARE_PANEL, DISPLAY_PREPARE_DSI, DISPLAY_PREPARE_LCD, DISPLAY_PREPARE_DE,
    DISPLAY_START_DE, DISPLAY_START_LCD, DISPLAY_START_DSI,
    DISPLAY_OBSERVE_DE, DISPLAY_OBSERVE_LCD, DISPLAY_OBSERVE_DSI,
    DISPLAY_STOP_DSI, DISPLAY_STOP_DE, DISPLAY_DRAIN_DE, DISPLAY_STOP_LCD,
    DISPLAY_QUIET_OUTPUT, DISPLAY_REPLAY_DE
};
enum display_scanout_action { DISPLAY_SCANOUT_START, DISPLAY_SCANOUT_OBSERVE, DISPLAY_SCANOUT_STOP };
enum display_scanout_error { DISPLAY_SCANOUT_OK, DISPLAY_SCANOUT_TIME,
    DISPLAY_SCANOUT_PROVIDER, DISPLAY_SCANOUT_IDENTITY, DISPLAY_SCANOUT_LEAF,
    DISPLAY_SCANOUT_SEQUENCE, DISPLAY_SCANOUT_REENTRY };
struct display_provider {
    int (*now_us)(void *, uint64_t *);
    /* Acquisition returns 1 complete, 0 pending, -1 fault. Tokens start zero.
     * Publish a COMPLETE nonzero token before the first reservation or physical
     * effect, and retain that exact identity on pending/error. A pending call
     * with a zero token has no effects. A granted token never expires or changes.
     * The one platform registry authenticates the owner, full request and scope;
     * values supplied by callers are not proof of ownership. No automatic release.
     * Resource acquisition establishes clocks/reset/power/cold handoff, inactive
     * PLL-clock selection and inherited locked PLL factors required by PHY init.
     */
    int (*acquire_resources)(void *, const struct display_request *, uint64_t,
                             struct display_resource_token *);
    int (*acquire_frame)(void *, const struct display_request *, const struct display_resource_token *,
                         uint64_t, struct display_frame_token *);
    /* Exact tokens, generations, mappings, visibility and phase are authenticated
     * by the real registry. Return exactly1 or failure, bounded and without side
     * effects. Called at each leaf's admission and each pending panel/drain step.
     * It must verify current independent evidence, including DE drain for LCD stop.
     */
    int (*authenticate)(void *, const struct display_request *, const struct display_ownership *,
                        enum display_phase, uint64_t);
    int (*read32)(void *, const struct display_resource_token *, uintptr_t, uint32_t *);
    int (*write32)(void *, const struct display_resource_token *, uintptr_t, uint32_t);
    int (*barrier)(void *, const struct display_resource_token *);
    /* Bounded asynchronous panel command controller: 1 complete, 0 pending,
     * -1 fault. Owns DSI1 command engine under the SAME lease. Use actual LP
     * command-controller calls and panel-specific power/reset/timing; complete
     * only with commands finished, engine idle and panel ready for video. PHY
     * READY alone is not panel readiness. No unspecified panel is admitted.
     */
    int (*prepare_panel)(void *, const struct display_request *, const struct display_ownership *,
                         const struct a733_dsi_phy *, uint64_t);
    /* Independent completion providers, 1 complete/0 pending/-1 fault. Do not
     * derive either proof solely from leaf state/enable/frame-end. Publish a
     * complete proof only on success. Proof identities bind both token generations
     * and a completion time after this phase began. Keep all leases and memory.
     * DE drain proves no fetches/prefetch or writeback/offline/RCQ transactions
     * remain and no new accesses can start. Output quiet proves external/panel
     * quiescence after LCD stop, with command engine exclusion still held.
     */
    int (*drain_de)(void *, const struct display_request *, const struct display_ownership *,
                    uint64_t, struct display_proof *);
    int (*quiet_output)(void *, const struct display_request *, const struct display_ownership *,
                        uint64_t, struct display_proof *);
    int (*rcq_submit)(void *,const struct display_resource_token *,uint64_t);
};
struct display_scanout {
    struct display_request request;
    struct display_provider provider;
    void *context;
    struct display_ownership ownership;
    /* Preserve an unexpected acquisition output alongside the original token. */
    struct display_resource_token observed_resources;
    struct display_frame_token observed_frame;
    struct a733_dsi_phy phy;
    struct a733_dsi_video dsi;
    struct a733_tcon_lcd lcd;
    struct a733_de de;
    uint64_t deadline, last_now, phase_after;
    enum display_scanout_state state;
    enum display_scanout_error error;
    enum display_phase phase, failed_phase;
    unsigned initialized, entered, begun, authenticated, panel_ready;
};
/* Zero storage, privileged single owner, immutable disjoint request/provider/
 * context backing. Copies inputs, validates timing/geometry, no callbacks.
 * cpu_address describes the same full padded mapping as device_address; the
 * provider must prove allocator ownership, translations, alias exclusion,
 * cache-line isolation, burst guard coverage and completed cache publication.
 * The token's exact mapping/visibility generations remain pinned for lifetime.
 * These provider bindings are required; this interface does not implement the
 * shared clock/pin/power/IOMMU/allocator managers or cold PLL provisioning.
 */
int display_scanout_init(struct display_scanout *, const struct display_request *,
                         const struct display_provider *, void *);
/* NEW->prepare; READY->START; ACTIVE->OBSERVE/STOP. Refusal invokes no callback.
 * Deadline covers the complete requested operation, not each leaf separately. */
int display_scanout_replay(struct display_scanout *,unsigned slot,uint64_t frame_bytes,uint64_t deadline);
int display_scanout_prepare(struct display_scanout *, uint64_t deadline);
int display_scanout_request(struct display_scanout *, enum display_scanout_action, uint64_t deadline);
/* One provider or leaf step per poll; timestamp callbacks are additional.
 * Terminal states are quiet. No retries, rollback, automatic stop, token release
 * or framebuffer retirement on failure. STOPPED_RETAINED keeps all resources:
 * verified PHY/PLL/panel shutdown, shared restoration and lease release are not
 * implemented. ACTIVE denotes composed controller start, not panel acceptance.
 */
enum display_scanout_state display_scanout_poll(struct display_scanout *);
#endif
