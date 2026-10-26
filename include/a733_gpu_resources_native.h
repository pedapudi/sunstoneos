#ifndef SUNSTONE_A733_GPU_RESOURCES_NATIVE_H
#define SUNSTONE_A733_GPU_RESOURCES_NATIVE_H
#include "a733_gpu_resources.h"
#include "a733_i2c.h"
#include "a733_pmic_consumer.h"

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
    struct a733_pmic *pmic;
    struct a733_gpu_resource_manager *pmic_manager;
    struct a733_pmic_consumer *pmic_view;
    /* Established only by the direct native binding or shared-resource
     * constructor. Authenticate the actual wrapper tuple before binding. */
    struct a733_gpu_resource_manager *manager;
    struct a733_gpu_resource_ops manager_operations;
    void *manager_context;
    unsigned pmic_client;
    uint32_t initialized, i2c_held, rail_step;
};
/* AArch64 EL1 only, before use. Require the architectural counter at 24 MHz;
 * initialization accesses only CNTFRQ_EL0. Subsequent callbacks use Device-mapped
 * identity addresses and the existing inherited-bus I2C driver. Independent
 * watchdog recovery, valid mappings, and inherited I2C initialization are caller
 * prerequisites for isolated legacy I2C users. PCK-only acquisition with the
 * shared read-only PMIC provider may use the supervised RAM recovery policy in
 * docs/a733-smp-supervised.md; it retains ownership until manual power removal.
 * This PCK-only exception does not admit GPU/DMA work or prove prior drain.
 * The integrated OS's supervised recorded GPU caller has a separate concrete
 * entry/recovery contract in docs/designs/gpu-recognition-wait.md; it binds
 * GPU ownership and mappings and retains resources until whole-board power
 * removal. It does not extend the PCK-only exception to other callers.
 * I2C failures retain bus ownership and perform no later reads.
 * Pass &native.operations and &native to a733_gpu_resources_init().
 */
int a733_gpu_resources_native_init(struct a733_gpu_resource_native *,
                                   const struct a733_gpu_resource_locks *);
/* Before concurrency or any acquisition, select shared-provider mode for the
 * one manager using this adapter (directly or through the NPU shared wrapper).
 * This irreversibly disables the legacy reader even while no view is bound.
 * The default initializer is for isolated inherited-bus users only; it cannot
 * coexist with a shared provider. No PMIC/controller work occurs here. */
int a733_gpu_resources_native_use_pmic(struct a733_gpu_resource_native *,
    struct a733_gpu_resource_manager *,struct a733_pmic *);
int a733_gpu_resources_native_bind_view(struct a733_gpu_resource_native *,struct a733_pmic_consumer *,unsigned client);
#endif
