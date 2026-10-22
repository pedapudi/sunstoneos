#ifndef SUNSTONE_PVR_GPU_STOP_H
#define SUNSTONE_PVR_GPU_STOP_H
#include "pvr_gpu_control.h"
#define PVR_GPU_STOP_MAX_STEPS 15u

enum pvr_gpu_stop_phase { PVR_GPU_STOP_NEW, PVR_GPU_STOP_CLEAR_IDLE,
    PVR_GPU_STOP_SEND_IDLE, PVR_GPU_STOP_WAIT_IDLE, PVR_GPU_STOP_CLEAR_OFF,
    PVR_GPU_STOP_SEND_OFF, PVR_GPU_STOP_WAIT_OFF, PVR_GPU_STOP_RETIRE,
    PVR_GPU_STOP_REGISTERS, PVR_GPU_STOP_ISOLATE, PVR_GPU_STOP_STOPPED, PVR_GPU_STOP_FAILED };
enum pvr_gpu_stop_result { PVR_GPU_STOP_OK, PVR_GPU_STOP_AGAIN,
    PVR_GPU_STOP_INVALID, PVR_GPU_STOP_FAULT };
enum pvr_gpu_stop_error { PVR_GPU_STOP_NO_ERROR, PVR_GPU_STOP_IO,
    PVR_GPU_STOP_CLOCK, PVR_GPU_STOP_TIMEOUT, PVR_GPU_STOP_CONTROL, PVR_GPU_STOP_COMMAND };

/* All callbacks return 1 on success, 0 on failure. They must be bounded and
 * non-reentrant. now uses the control worker's monotonic microsecond clock.
 * read/write perform one ordered 32/64-bit device access. Writes complete
 * before returning; readbacks fence preceding writes. sync_write publishes
 * one 32-bit power-sync word, including cache maintenance/barriers. sync_read
 * acquires that word without overwriting firmware writes.
 * isolate completes platform teardown after firmware shutdown, idle checks
 * and reset/readback. The native backend verifies fresh forced-idle/OFF state
 * alongside each ACK, then powers down before reclaim. These callbacks provide
 * healthy orderly stop, not recovery from failed or uncertain DMA. A standalone
 * reset or power-OFF observation cannot replace the preceding sequence.
 */
struct pvr_gpu_stop_ops {
    int (*now)(void *, uint64_t *);
    int (*read)(void *, uint32_t offset, uint32_t width, uint64_t *);
    int (*write)(void *, uint32_t offset, uint32_t width, uint64_t);
    int (*sync_write)(void *, uint32_t);
    int (*sync_read)(void *, uint32_t *);
    int (*isolate)(void *);
    /* Optional asynchronous completion. With this callback, isolate begins
     * teardown once and returns exactly 1 on admission. Each poll returns 0
     * while pending, exactly 1 after completion, or -1 on failure. The original
     * absolute deadline bounds both callbacks; no internal wait is permitted. */
    int (*isolate_poll)(void *);
};
struct pvr_gpu_stop_step { uint32_t kind, offset, width; uint64_t mask, value; };
struct pvr_gpu_stop {
    struct pvr_gpu_stop_ops ops;
    struct pvr_gpu_control *control;
    void *context;
    uint64_t deadline, last_now;
    uint32_t phase, error, index, count;
    uint32_t acknowledgement[2], command_status[2], isolation_observed, isolation_started;
    struct pvr_ccb_token tokens[2];
    struct pvr_gpu_stop_step steps[PVR_GPU_STOP_MAX_STEPS];
};

/* Offline init, state zeroed. Refusal preserves state and control. Require READY
 * control with an ACTIVE queue. Use the same pinned/validated firmware image
 * as startup. Decode the stop-relevant feature bits after structural validation.
 * Typed inputs need C alignment; state must be disjoint from every input.
 * Before init, the owner must close application admission, hold power/resources,
 * and serialize all control/queue access through the shutdown worker. Existing
 * event handlers must remain available during the firmware handshakes. New
 * application jobs must remain blocked until a separately initialized boot.
 * deadline covers both handshakes, idle polls, reset, and platform isolation.
 */
int pvr_gpu_stop_init(struct pvr_gpu_stop *, struct pvr_gpu_control *,
                      const void *image, size_t bytes, const struct pvr_gpu_stop_ops *,
                      void *context, uint64_t deadline);
/* Bounded poll. During a handshake, service at most four firmware events plus
 * one return scan, then read the power-sync word. During hardware shutdown,
 * perform one register step per poll. A power-sync or register poll re-reads
 * for at most PVR_GPU_SPIN_US and then returns AGAIN; every wait ends at the
 * deadline. Terminal states make no callbacks. Faults retain all buffers and
 * ownership; control and transport also latch failure. No reset/restart retries.
 * STOPPED means the complete protocol and isolation callback succeeded. It
 * does not establish successful completion of cancelled GPU jobs, output-cache
 * visibility, or physical qualification. The owner decides job disposition and
 * resource release only after the complete orderly shutdown contract.
 */
enum pvr_gpu_stop_result pvr_gpu_stop_poll(struct pvr_gpu_stop *);
#endif
