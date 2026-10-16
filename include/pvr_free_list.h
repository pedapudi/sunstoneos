#ifndef SUNSTONE_PVR_FREE_LIST_H
#define SUNSTONE_PVR_FREE_LIST_H
#include "pvr_gpu_events.h"

#define PVR_FREE_LISTS 8u
#define PVR_FREE_LIST_REQUESTS 8u
#define PVR_FREE_LIST_MAX_PAGES 16384u
#define PVR_FREE_LIST_CHUNK 64u
/* One admitted allocation lease. Metadata belongs to the manager. A successful
 * allocation grants a private writable 4 KiB CPU mapping and a distinct 36-bit
 * physical page. Backing must exclude every firmware/table/manager allocation.
 * The platform retains ownership by (epoch, resource token, slot) even if an
 * uncertain allocator result cannot supply a complete descriptor.
 */
struct pvr_free_list_page { void *cpu; uint64_t physical, lease; };
struct pvr_free_list_config {
    struct pvr_gpu_event_resource resource;
    uint64_t gpu_address;
    uint32_t firmware_address, initial_pages, maximum_pages, grow_pages, threshold;
    struct pvr_free_list_page *pages; /* maximum_pages entries, zero before init */
};
enum pvr_free_list_memory { PVR_FREE_LIST_TABLE, PVR_FREE_LIST_FIRMWARE };
/* Operations other than allocate return 1 only on completed success; every
 * other return is an uncertain failure. No callback may reenter the manager. */
struct pvr_free_list_ops {
    /* 1 grants the descriptor, 0 refuses without effects, -1 is uncertain.
     * Store the descriptor before any further callback, including on failure. */
    int (*allocate)(void *, uint64_t epoch, uint64_t owner, uint64_t token,
                    unsigned slot, struct pvr_free_list_page *);
    int (*publish_page)(void *, const struct pvr_free_list_page *);
    /* Shared-memory reads/writes, never MMIO. Firmware words use aligned atomic
     * little-endian transactions. write copies data before return. publish makes
     * preceding host writes visible without overwriting firmware-owned bytes. */
    int (*read)(void *, uint64_t token, enum pvr_free_list_memory, uint32_t offset, void *, size_t);
    int (*write)(void *, uint64_t token, enum pvr_free_list_memory, uint32_t offset, const void *, size_t);
    int (*publish)(void *, uint64_t token, enum pvr_free_list_memory, uint32_t offset, size_t);
    /* Rebuild/reset all render-target records linked to this free list, including
     * HWR state and clearing HAS_LAST_GEOM. Return 1 only when completed. This
     * callback is required even if the platform proves there are no linked targets. */
    int (*recover_targets)(void *, uint64_t token);
};
struct pvr_free_list_state {
    struct pvr_free_list_config config;
    uint32_t acquired, active, current, ready;
};
struct pvr_free_list_request {
    struct pvr_gpu_event event;
    uint64_t deadline;
    unsigned count, lists[PVR_GPU_EVENTS_RECONSTRUCT];
};
enum pvr_free_list_phase { PVR_FREE_LIST_NEW, PVR_FREE_LIST_BUILDING,
    PVR_FREE_LIST_IDLE, PVR_FREE_LIST_WORKING, PVR_FREE_LIST_FAILED };
enum pvr_free_list_error { PVR_FREE_LIST_NO_ERROR, PVR_FREE_LIST_INVALID,
    PVR_FREE_LIST_CLOCK, PVR_FREE_LIST_TIMEOUT, PVR_FREE_LIST_ALLOCATION,
    PVR_FREE_LIST_PUBLICATION, PVR_FREE_LIST_MEMORY_ERROR, PVR_FREE_LIST_RECOVERY,
    PVR_FREE_LIST_QUEUE_ERROR, PVR_FREE_LIST_REENTRY };
enum pvr_free_list_result { PVR_FREE_LIST_OK, PVR_FREE_LIST_AGAIN, PVR_FREE_LIST_FAULT };
struct pvr_free_lists {
    struct pvr_free_list_ops ops;
    void *context;
    struct pvr_ccb *queue;
    uint64_t epoch, timeout, last_now, deadline;
    unsigned phase, error, busy, count, stage, list, cursor, goal, old_active;
    unsigned request_head, request_count, request_index, recovery_step;
    uint32_t firmware_current;
    struct pvr_free_list_state lists[PVR_FREE_LISTS];
    struct pvr_free_list_request requests[PVR_FREE_LIST_REQUESTS];
    struct pvr_ccb_token response_token;
    unsigned char response[PVR_CCB_COMMAND_BYTES];
};
/* Zero state once. Init performs no callbacks/writes to supplied metadata and
 * preserves state on refusal. The platform owns protected GPU mappings for each
 * maximum-sized table and 64-byte firmware record; no GPU may use them until
 * initial polling returns OK. Page metadata must be private, disjoint and zero.
 * Resource IDs/tokens, mappings, metadata and ops stay pinned for this epoch.
 * Only one privileged worker calls the manager, event dispatcher and CCB.
 * minimum_pages comes from verified device features and must be nonzero.
 */
int pvr_free_lists_init(struct pvr_free_lists *, const struct pvr_free_list_config *, unsigned count,
    unsigned minimum_pages, const struct pvr_free_list_ops *, void *, struct pvr_ccb *,
    uint64_t now, uint64_t timeout_us);
/* Event callback for GROW/RECONSTRUCT only. Retains a complete copied action;
 * invokes no allocator, memory, recovery, or CCB callback. Full queue, unsupported
 * event, or mismatched ownership returns 0; the control worker must latch failure.
 * The manager refuses further work if the CCB is subsequently failed/retired.
 */
int pvr_free_lists_event(void *, const struct pvr_gpu_event *,
    const struct pvr_gpu_event_resource *const *, unsigned count);
/* One bounded page allocation/zero/publication, table chunk, record operation,
 * recovery callback, or CCB response step. All callbacks must be bounded and
 * non-reentrant. Poll after control polling so command-return scans are reused.
 * Initial setup accepts PREPARED/ACTIVE queue; responses require ACTIVE. The
 * same monotonic microsecond clock supplies now to control and this manager.
 * A definite allocation refusal during growth returns delta_pages=0 and retains
 * any previously acquired spare pages. Uncertain failures retain every lease,
 * current request and response diagnostics, and prohibit further callbacks.
 * No release/restart API: reclamation requires verified device isolation plus
 * platform disposal of all leases and unresolved allocation attempts.
 */
enum pvr_free_list_result pvr_free_lists_poll(struct pvr_free_lists *, uint64_t now);
#endif
