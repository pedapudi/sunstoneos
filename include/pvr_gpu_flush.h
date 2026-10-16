#ifndef SUNSTONE_PVR_GPU_FLUSH_H
#define SUNSTONE_PVR_GPU_FLUSH_H
#include "pvr_gpu_control.h"

enum pvr_gpu_flush_level { PVR_GPU_FLUSH_LEAF, PVR_GPU_FLUSH_DIRECTORY,
    PVR_GPU_FLUSH_CATALOGUE };
enum pvr_gpu_flush_phase { PVR_GPU_FLUSH_NEW, PVR_GPU_FLUSH_PUBLISH,
    PVR_GPU_FLUSH_SUBMIT, PVR_GPU_FLUSH_WAIT, PVR_GPU_FLUSH_COMPLETE,
    PVR_GPU_FLUSH_FAILED };
enum pvr_gpu_flush_result { PVR_GPU_FLUSH_OK, PVR_GPU_FLUSH_AGAIN,
    PVR_GPU_FLUSH_INVALID, PVR_GPU_FLUSH_FAULT };
enum pvr_gpu_flush_error { PVR_GPU_FLUSH_NO_ERROR, PVR_GPU_FLUSH_IO,
    PVR_GPU_FLUSH_CLOCK, PVR_GPU_FLUSH_TIMEOUT, PVR_GPU_FLUSH_CONTROL,
    PVR_GPU_FLUSH_COMMAND };

/* Bounded, non-reentrant callbacks, returning 1 on success and 0 on failure.
 * now shares the controller's monotonic microsecond clock. publish makes every
 * modified table entry in this operation visible to GPU table walkers before
 * subsequent command publication. It must include appropriate CPU cache
 * maintenance and barriers. Hold the table/data allocations and power leases.
 * A failed publication may have changed device-visible memory: retain it.
 */
struct pvr_gpu_flush_ops {
    int (*now)(void *, uint64_t *);
    int (*publish)(void *);
};
struct pvr_gpu_flush {
    struct pvr_gpu_flush_ops ops;
    struct pvr_gpu_control *control;
    struct pvr_ccb *queue;
    void *context;
    uint64_t epoch, deadline, last_now;
    struct pvr_ccb_token token;
    uint32_t phase, error, status;
    unsigned char command[PVR_CCB_COMMAND_BYTES];
};

/* Initialize one mapping-change batch without I/O; refusal preserves state.
 * State must be zeroed, C-aligned, and disjoint from all input objects. The
 * control must be READY on the same active queue throughout the operation.
 * Use the highest changed translation level: directory includes leaf caches;
 * catalogue includes both lower levels. Every level also invalidates TLBs and
 * requests a completion interrupt. The existing firmware MMU-sync allocation
 * must remain mapped at the address supplied by pvr_fw_state_layout().
 *
 * One privileged worker serializes this operation, control/queue calls, table
 * changes, and job admission. Before changing tables, prevent access by jobs
 * using the affected mappings and retain their backing until completion here.
 * Close new job admission until COMPLETE. Freeze the changed tables from init
 * through completion; unrelated firmware event servicing remains available.
 * Completion is an MMU-cache command result, not GPU job completion, SLC data
 * cache maintenance, device isolation, or authorization to free active memory.
 */
int pvr_gpu_flush_init(struct pvr_gpu_flush *, struct pvr_gpu_control *,
                       const struct pvr_gpu_flush_ops *, void *context,
                       enum pvr_gpu_flush_level, uint64_t deadline);

/* One bounded step. While waiting, service at most four firmware events and
 * one command-return scan. A full queue is serviced without resubmitting any
 * published command. Absolute deadline covers publication and completion.
 * Failure aborts control/queue I/O and retains ownership; no reset or retries.
 * Terminal calls make no callbacks. After COMPLETE the owner may discard this
 * operation record; FAILED requires independently verified device isolation.
 */
enum pvr_gpu_flush_result pvr_gpu_flush_poll(struct pvr_gpu_flush *);
#endif
