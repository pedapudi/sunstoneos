#ifndef SUNSTONE_PVR_GPU_CONTROL_H
#define SUNSTONE_PVR_GPU_CONTROL_H
#include "pvr_gpu_start.h"
#include "pvr_ccb.h"

enum pvr_gpu_control_phase { PVR_GPU_CONTROL_NEW, PVR_GPU_CONTROL_COMPAT,
    PVR_GPU_CONTROL_SUBMIT_HEALTH, PVR_GPU_CONTROL_WAIT_HEALTH,
    PVR_GPU_CONTROL_READY, PVR_GPU_CONTROL_FAILED, PVR_GPU_CONTROL_RETIRED };
enum pvr_gpu_control_result { PVR_GPU_CONTROL_OK, PVR_GPU_CONTROL_AGAIN,
    PVR_GPU_CONTROL_INVALID, PVR_GPU_CONTROL_FAULT };
enum pvr_gpu_control_error { PVR_GPU_CONTROL_NO_ERROR, PVR_GPU_CONTROL_IO,
    PVR_GPU_CONTROL_CLOCK, PVR_GPU_CONTROL_TIMEOUT, PVR_GPU_CONTROL_INCOMPATIBLE,
    PVR_GPU_CONTROL_QUEUE, PVR_GPU_CONTROL_RESTART, PVR_GPU_CONTROL_EVENT,
    PVR_GPU_CONTROL_HEALTH, PVR_GPU_CONTROL_EXTERNAL };

/* Callbacks return 1 on success, 0 on failure, and must be bounded and
 * non-reentrant. now uses the same monotonic clock as the queue, in microseconds.
 * read32/write32 perform ordered device accesses; writes complete before return.
 * compat acquires bytes from the 64-byte compatibility record at OS-init +40.
 * Once updated (record byte 56) is one, firmware must publish an immutable record
 * for this boot. Invalidate CPU caches as needed without overwriting firmware.
 * event receives a copied 88-byte known event. Validate its type-specific payload
 * and apply or retain its required action before returning success. It must not
 * call this controller or the queue recursively. NULL rejects every event.
 * Restart/page-fault events never reach this handler: they latch failure.
 */
struct pvr_gpu_control_ops {
    int (*now)(void *, uint64_t *);
    int (*read32)(void *, uint32_t offset, uint32_t *);
    int (*write32)(void *, uint32_t offset, uint32_t);
    int (*compat)(void *, uint32_t offset, void *, size_t);
    int (*event)(void *, const unsigned char command[PVR_CCB_COMMAND_BYTES]);
};
struct pvr_gpu_control {
    struct pvr_gpu_control_ops ops;
    struct pvr_ccb *queue;
    void *context;
    uint64_t deadline, last_now;
    struct pvr_ccb_token health_token;
    int (*executed)(void *, uint32_t *);
    uint32_t phase, error, health_done, health_status, executed_before, executed_after;
    unsigned char compatibility[64], last_event[PVR_CCB_COMMAND_BYTES];
};

/* Offline initialization. state must be zeroed; refusal preserves state and q.
 * Require BOOTED startup and PREPARED queue from the same resource instance.
 * Ownership/correspondence and physical identity are platform responsibilities.
 * Copy ops, retain q/context; state must be disjoint from all input objects.
 * A single privileged worker must serialize startup, this controller, and all
 * queue users. No application commands may be submitted before READY.
 * deadline covers compatibility publication and the initial health command.
 */
int pvr_gpu_control_init(struct pvr_gpu_control *, const struct pvr_gpu_start *,
                         struct pvr_ccb *, const struct pvr_gpu_control_ops *, void *, uint64_t deadline);
/* Before the first poll, judge the health check by the firmware's executed
 * kernel-command count, which executed acquires (OS data +568), as upstream
 * judges it (pvr_power.c, pvr_watchdog_kccb_stalled): the count advanced by
 * exactly one and the firmware's read offset passed the command. A return
 * word is recorded when the firmware wrote one and must then be 1; the
 * open firmware on the A733 wrote none (board-session-20261025-18). Without
 * this call the return word must be 1. Refused after the compatibility
 * record is accepted. */
int pvr_gpu_control_count_executed(struct pvr_gpu_control *, int (*executed)(void *, uint32_t *));
/* Bounded worker poll. event_budget is 1..31. At most two IRQ-status reads,
 * one acknowledgement, event_budget receives/handlers, and one return scan.
 * AGAIN requests further service; OK means READY with no observed backlog.
 * Poll on IRQ notifications and before pending deadlines even without an IRQ.
 * The interrupt top half only masks/notifies; this function is not an ISR.
 * READY proves revision/layout agreement and one health-command execution,
 * not graphics/compute execution, command admission, or GPU job completion.
 * After failure no callback runs; the queue also refuses further I/O. Retain
 * backing, compatibility, and event diagnostics until a verified device stop.
 * Known event handlers and physical IRQ/GIC integration remain platform work.
 */
enum pvr_gpu_control_result pvr_gpu_control_poll(struct pvr_gpu_control *, unsigned event_budget);
/* Close READY control/queue service after the owner's power-off handshake.
 * No I/O, device isolation, or buffer release. Polls then return INVALID. */
enum pvr_gpu_control_result pvr_gpu_control_retire(struct pvr_gpu_control *);
/* Retain an external shutdown/platform fault and stop future control/queue I/O.
 * No hardware access; valid after initialization, including RETIRED. */
enum pvr_gpu_control_result pvr_gpu_control_abort(struct pvr_gpu_control *);
#endif
