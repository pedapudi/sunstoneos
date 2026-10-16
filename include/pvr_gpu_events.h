#ifndef SUNSTONE_PVR_GPU_EVENTS_H
#define SUNSTONE_PVR_GPU_EVENTS_H
#include "pvr_ccb.h"

#define PVR_GPU_EVENTS_RESOURCES 64u
#define PVR_GPU_EVENTS_RECONSTRUCT 18u
enum pvr_gpu_event_kind {
    PVR_GPU_EVENT_BACK = 101, PVR_GPU_EVENT_UNBACK, PVR_GPU_EVENT_GROW,
    PVR_GPU_EVENT_RECONSTRUCT, PVR_GPU_EVENT_RESET, PVR_GPU_EVENT_DEBUG,
    PVR_GPU_EVENT_STATS, PVR_GPU_EVENT_CLOCK
};
struct pvr_gpu_event {
    enum pvr_gpu_event_kind kind;
    uint32_t flags; /* Uninterpreted firmware compatibility flags; not padding. */
    union {
        uint32_t id; /* BACK, UNBACK, GROW. Zero is a valid firmware ID. */
        struct { uint32_t count, recovery_counter, ids[PVR_GPU_EVENTS_RECONSTRUCT]; } reconstruct;
        struct {
            uint32_t context, reason, engine, job, flags;
            uint64_t page_catalog, fault_address;
        } reset;
        struct { uint32_t statistic, process; int32_t adjustment; } stats;
        uint32_t clock_hz;
    } data;
};
enum pvr_gpu_event_decode_result {
    PVR_GPU_EVENT_DECODE_OK, PVR_GPU_EVENT_DECODE_INVALID,
    PVR_GPU_EVENT_DECODE_TYPE, PVR_GPU_EVENT_DECODE_PAYLOAD
};
/* Exact 88-byte command; no aligned input requirement. Padding and inactive
 * union members are ignored. Reset flags and enum ranges are validated; outer
 * compatibility flags are retained because the pinned ABI defines no mask.
 * Output is changed only on success and must not overlap the command.
 */
enum pvr_gpu_event_decode_result pvr_gpu_event_decode(const void *, size_t,
                                                      struct pvr_gpu_event *);

enum pvr_gpu_event_resource_kind { PVR_GPU_RESOURCE_ZS = 1,
    PVR_GPU_RESOURCE_FREELIST, PVR_GPU_RESOURCE_CONTEXT, PVR_GPU_RESOURCE_PROCESS };
struct pvr_gpu_event_resource {
    enum pvr_gpu_event_resource_kind kind;
    uint32_t id;
    uint64_t owner, token; /* Nonzero platform-issued identities; never FW pointers. */
};
/* The catalogue is copied at initialization. Its objects remain pinned until
 * verified device isolation; IDs and tokens cannot be reused within this epoch.
 * Each handler receives the entire validated event and its resolved resources.
 * Reconstruction preserves the request's ID order, including duplicate IDs.
 * RESET with ALL_CTXS resolves every context; its context field is ignored.
 * DEBUG/CLOCK have zero targets. No firmware-supplied pointer is dereferenced.
 * The target-pointer array is valid only during the callback. Resource records
 * remain pinned in this dispatcher, but the event is overwritten by subsequent
 * dispatch. A deferred action must copy the event and resolved target identities
 * into its own storage before returning.
 *
 * A handler returns 1 only after applying the required operation or retaining
 * a complete action in an implemented, bounded resource service. Responses and
 * referenced resources must remain owned until completion. Returning success
 * solely because a request was logged is invalid for resource operations.
 * Calls must be bounded and non-reentrant. Do not call the GPU queue/control
 * recursively: response submission belongs to the worker after control polling.
 * NULL means unsupported. Failure may have side effects and retains all objects.
 */
typedef int (*pvr_gpu_event_handler)(void *, const struct pvr_gpu_event *,
    const struct pvr_gpu_event_resource *const *targets, unsigned count);
struct pvr_gpu_event_handlers {
    pvr_gpu_event_handler back, unback, grow, reconstruct, reset, debug, stats, clock;
};
enum pvr_gpu_events_phase { PVR_GPU_EVENTS_NEW, PVR_GPU_EVENTS_READY,
    PVR_GPU_EVENTS_DISPATCHING, PVR_GPU_EVENTS_FAILED };
enum pvr_gpu_events_error { PVR_GPU_EVENTS_NO_ERROR, PVR_GPU_EVENTS_INVALID,
    PVR_GPU_EVENTS_EPOCH, PVR_GPU_EVENTS_DECODE, PVR_GPU_EVENTS_OWNERSHIP,
    PVR_GPU_EVENTS_UNSUPPORTED, PVR_GPU_EVENTS_ACTION, PVR_GPU_EVENTS_REENTRY };
struct pvr_gpu_events {
    uint64_t epoch;
    struct pvr_gpu_event_handlers handlers;
    void *context;
    unsigned count, phase, error, decode_error;
    struct pvr_gpu_event_resource resources[PVR_GPU_EVENTS_RESOURCES];
    struct pvr_gpu_event last_event;
    size_t last_bytes;
    unsigned char last_raw[PVR_CCB_COMMAND_BYTES];
};
/* One serialized privileged owner. Zero state once; init performs no callbacks
 * or resource writes and preserves state on refusal. Caller verifies catalogue
 * ownership and pins each backing object. An empty catalogue permits only
 * device-wide notifications and zero-resource requests with real handlers.
 */
int pvr_gpu_events_init(struct pvr_gpu_events *, uint64_t epoch,
    const struct pvr_gpu_event_resource *, unsigned count,
    const struct pvr_gpu_event_handlers *, void *context);
/* Resolve every resource before calling one handler. Errors latch FAILED;
 * subsequent calls preserve diagnostics and invoke no handlers. Retained input
 * is at most 88 bytes, with last_bytes recording the supplied length. The input
 * must be disjoint from state. Epoch must equal the current control queue's.
 */
int pvr_gpu_events_dispatch(struct pvr_gpu_events *, uint64_t epoch, const void *, size_t);
#endif
