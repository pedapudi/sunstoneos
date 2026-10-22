#ifndef SUNSTONE_A733_GPU_POWER_H
#define SUNSTONE_A733_GPU_POWER_H
#include <stdint.h>
#include "a733_gpu_spin.h"

#define A733_GPU_INITIAL_HZ 400000000u
#define A733_GPU_INITIAL_UV 800000u

enum a733_gpu_power_bank { A733_GPU_CCU, A733_GPU_R_CCU, A733_GPU_PCK };
enum a733_gpu_power_phase { A733_GPU_POWER_NEW, A733_GPU_POWER_STARTING,
    A733_GPU_POWER_ON, A733_GPU_POWER_STOPPING, A733_GPU_POWER_OFF,
    A733_GPU_POWER_FAILED };
enum a733_gpu_power_result { A733_GPU_POWER_OK, A733_GPU_POWER_AGAIN,
    A733_GPU_POWER_INVALID, A733_GPU_POWER_FAULT };
enum a733_gpu_power_error { A733_GPU_POWER_NO_ERROR, A733_GPU_POWER_IO,
    A733_GPU_POWER_CLOCK, A733_GPU_POWER_TIMEOUT, A733_GPU_POWER_RESOURCES,
    A733_GPU_POWER_STATE, A733_GPU_POWER_CANCELLED };

struct a733_gpu_power_resources { uint32_t parent_hz, supply_uv; };
/* One privileged owner serializes this controller. All callbacks are bounded,
 * non-reentrant and return 1 for success. A failed callback may have side effects.
 * now uses monotonic microseconds without wrap during an operation; it may
 * round down to whole microseconds.
 * acquire owns the GPU-local clock, reset and GPU_TOP domain exclusively. It
 * also holds shared leases for stable pll-peri0-400m, DCDC4, dcxo, the PCK clock,
 * and GPU AHB/MBUS gates. Return the established parent rate and rail setting.
 * These resources remain stable until release; failures retain any partial lease.
 * Neither callback changes GPU-local clock/reset or GPU_TOP policy. The manager
 * must arbitrate shared clocks, rail changes and other devices' power requests.
 * read/write perform ordered 32-bit accesses to CCU 0x02002000, R_CCU 0x07010000
 * or PCK 0x07060000. update atomically applies (old & ~clear) | set under the
 * platform register lock, including any write key, preserving unrelated fields.
 * write/update complete before returning; read acquires preceding device writes.
 * release drops only this controller's shared-resource leases after power off.
 * It must preserve resources still used by other devices and the board
 * always-on DCDC4 rail.
 */
struct a733_gpu_power_ops {
    int (*now)(void *, uint64_t *);
    int (*acquire)(void *, struct a733_gpu_power_resources *);
    int (*release)(void *);
    int (*read)(void *, enum a733_gpu_power_bank, uint32_t offset, uint32_t *);
    int (*write)(void *, enum a733_gpu_power_bank, uint32_t offset, uint32_t);
    int (*update)(void *, enum a733_gpu_power_bank, uint32_t offset,
                  uint32_t clear, uint32_t set);
};
struct a733_gpu_power {
    struct a733_gpu_power_ops ops;
    void *context;
    uint64_t deadline, last_now;
    enum a733_gpu_power_phase phase;
    enum a733_gpu_power_error error;
    uint32_t index, last_value, lease_attempted, lease_held;
};
/* Zero storage before init. No I/O; refusal preserves state. Ops storage and
 * state must not overlap. Call only before first use. Callback context stays live.
 */
int a733_gpu_power_init(struct a733_gpu_power *, const struct a733_gpu_power_ops *, void *);
/* Start from NEW/OFF. Only a clock read occurs here. All physical use requires
 * qualified independent watchdog recovery and exclusive device ownership.
 * Initial GPU core/bus clocks must be disabled and external reset asserted;
 * a live inherited GPU is refused before GPU-local or PCK writes.
 */
enum a733_gpu_power_result a733_gpu_power_on(struct a733_gpu_power *, uint64_t deadline);
/* Start from ON after firmware shutdown, closed job admission, and interrupt
 * masking. The caller must retain all device backing. OFF reports clock/reset
 * and power-domain state only: it does not prove memory transactions drained.
 * This controller MUST NOT implement pvr_gpu_stop_ops.isolate by itself.
 */
enum a733_gpu_power_result a733_gpu_power_off(struct a733_gpu_power *, uint64_t deadline);
/* Runs the resource and register steps back to back, with clock reads. The
 * clock-settling delay waits on the clock inside the poll until the clock
 * passes the sample after the clock-gate write by more than one microsecond.
 * An unsatisfied PCK acknowledgement is re-read for at most
 * A733_GPU_SPIN_US from its first read and then returns AGAIN. A wait
 * whose clock stops advancing for A733_GPU_SPIN_STALLED_READS samples returns
 * AGAIN without advancing the step. Every wait ends at the absolute deadline.
 * Failure retains diagnostics and disables further I/O; no automatic rollback.
 * lease_held records completed acquire/release callbacks. Failed callbacks can
 * leave partial ownership; a late successful release leaves lease_held clear.
 * ON establishes the configured 400 MHz clock, not a frequency measurement.
 */
enum a733_gpu_power_result a733_gpu_power_poll(struct a733_gpu_power *);
enum a733_gpu_power_result a733_gpu_power_cancel(struct a733_gpu_power *);
#endif
