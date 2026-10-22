#ifndef SUNSTONE_PVR_GPU_START_H
#define SUNSTONE_PVR_GPU_START_H
#include "pvr_mips.h"
#include "a733_gpu_spin.h"

#define PVR_GPU_START_MAX_STEPS 35u
enum pvr_gpu_start_phase { PVR_GPU_START_NEW, PVR_GPU_START_PREPARED,
    PVR_GPU_START_RUNNING, PVR_GPU_START_BOOTED, PVR_GPU_START_FAILED };
enum pvr_gpu_start_result { PVR_GPU_START_OK, PVR_GPU_START_AGAIN,
    PVR_GPU_START_INVALID, PVR_GPU_START_FAULT };
enum pvr_gpu_start_error { PVR_GPU_START_NO_ERROR, PVR_GPU_START_IO,
    PVR_GPU_START_TIMEOUT, PVR_GPU_START_CLOCK, PVR_GPU_START_STALE,
    PVR_GPU_START_BAD_FLAG, PVR_GPU_START_CANCELLED };

/* All callbacks return 1 on success, 0 on failure; they must be bounded,
 * serialized and non-reentrant. A failed write may have taken effect.
 * now returns monotonic microseconds without wrap during this instance.
 * read/write perform exactly one ordered 32- or 64-bit device transaction.
 * Writes complete before returning, so minimum delays start after completion.
 * Readbacks fence preceding writes, including writes to other registers.
 * started acquires one firmware_started byte at system-init offset 352, after
 * making firmware writes visible, and returns that byte as a uint32_t.
 * publish makes every firmware allocation and table visible to the device,
 * completing cache maintenance and barriers before returning. It must not
 * overwrite firmware data from a previous live instance.
 */
struct pvr_gpu_start_ops {
    int (*now)(void *, uint64_t *);
    int (*read)(void *, uint32_t offset, uint32_t width, uint64_t *);
    int (*write)(void *, uint32_t offset, uint32_t width, uint64_t);
    int (*started)(void *, uint32_t *);
    int (*publish)(void *);
};
struct pvr_gpu_start_step { uint32_t kind, offset, width; uint64_t value; };
struct pvr_gpu_start {
    struct pvr_gpu_start_ops ops;
    void *context;
    uint64_t deadline, last_now;
    uint32_t phase, error, index, count, slc_value;
    struct pvr_gpu_start_step steps[PVR_GPU_START_MAX_STEPS];
};

/* Zero state before init. Fields are private except phase/error/index for
 * diagnosis. Decode the A733 MIPS firmware feature stream and copy a validated
 * wrapper plan from pvr_mips_prepare(). No I/O. Refusal preserves state.
 * Inputs must be stable and disjoint from state. Typed objects require C
 * alignment. Caller owns RAM, allocation lifetimes and physical aliases.
 * Firmware hash, physical identity, power/clocks, independent watchdog recovery,
 * prepared queues/resources and stopped-device ownership must be established
 * before begin. This component does not check those platform prerequisites.
 */
int pvr_gpu_start_init(struct pvr_gpu_start *, const void *image, size_t bytes,
                       const struct pvr_mips_boot *, const struct pvr_gpu_start_ops *, void *);
/* Set an absolute deadline in the callback clock's microseconds. Reads only the
 * clock. A failed clock or expired deadline latches FAILED. Re-entry is refused.
 */
enum pvr_gpu_start_result pvr_gpu_start_begin(struct pvr_gpu_start *, uint64_t deadline);
/* One bounded step: at most one register access or publication, plus clock
 * reads. The clock may round down to whole microseconds. A delay step waits
 * on the clock inside the step until the clock passes the start sample by more
 * than 3, so at least 3 us elapse after the preceding operation completes. A
 * readiness check re-reads the shared byte for at most A733_GPU_SPIN_US and then
 * returns AGAIN. A wait whose clock stops advancing returns AGAIN without
 * advancing the step. Every wait ends at the deadline. Service until BOOTED or
 * FAILED.
 * BOOTED proves only an acquired firmware-started flag, not compatibility,
 * queue health, GPU workload completion, or physical qualification.
 * Failure latches the error/index and prevents further callback access. Keep
 * all backing and diagnostics until independently verified device shutdown.
 */
enum pvr_gpu_start_result pvr_gpu_start_poll(struct pvr_gpu_start *);
/* Latch cancellation without device access. This does not stop the GPU. */
enum pvr_gpu_start_result pvr_gpu_start_cancel(struct pvr_gpu_start *);
#endif
