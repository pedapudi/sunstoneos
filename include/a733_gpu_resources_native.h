#ifndef SUNSTONE_A733_GPU_RESOURCES_NATIVE_H
#define SUNSTONE_A733_GPU_RESOURCES_NATIVE_H
#include "a733_gpu_resources.h"
#include "a733_i2c.h"

/* Platform locks must be shared with every clock-register and PMIC/I2C user.
 * enter functions use absolute monotonic microsecond deadlines and return 1
 * only while owning the lock. They must support task scheduling and SMP;
 * busy-waiting behind a preempted lock holder is not an acceptable binding.
 * Do not call back into this resource manager while holding either lock.
 */
struct a733_gpu_resource_locks {
    int (*register_enter)(void *, uint64_t deadline);
    void (*register_leave)(void *);
    int (*i2c_enter)(void *, uint64_t deadline);
    void (*i2c_leave)(void *);
    void *context;
};
struct a733_gpu_resource_native {
    struct a733_gpu_resource_ops operations;
    struct a733_gpu_resource_locks locks;
    struct a733_i2c bus;
    uint32_t initialized, i2c_held, rail_step;
};
/* AArch64 EL1 only, before use. Require the architectural counter at 24 MHz;
 * initialization accesses only CNTFRQ_EL0. Subsequent callbacks use Device-mapped
 * identity addresses and the existing inherited-bus I2C driver. Independent
 * watchdog recovery, valid mappings, and inherited I2C initialization are caller
 * prerequisites. I2C failures retain bus ownership and perform no later reads.
 * Pass &native.operations and &native to a733_gpu_resources_init().
 */
int a733_gpu_resources_native_init(struct a733_gpu_resource_native *,
                                   const struct a733_gpu_resource_locks *);
#endif
