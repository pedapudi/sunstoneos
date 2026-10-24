#ifndef SUNSTONE_A733_GPU_PLATFORM_H
#define SUNSTONE_A733_GPU_PLATFORM_H
#include "a733_gpu_resources.h"
#include "a733_pmic_consumer.h"
struct a733_gpu_resource_native;

/* Bounded, ordered operations; all mutations use the common register lock.
 * The clock is the same monotonic microsecond clock used by the manager.
 * Callbacks return 1 on success. A failure may have effects. */
struct a733_gpu_platform_ops {
    int (*now)(void *, uint64_t *);
    int (*read)(void *, enum a733_gpu_power_bank, uint32_t, uint32_t *);
    int (*update)(void *, enum a733_gpu_power_bank, uint32_t, uint32_t, uint32_t, uint64_t);
};
enum a733_gpu_platform_phase { A733_GPU_PLATFORM_NEW, A733_GPU_PLATFORM_ADMIT,
    A733_GPU_PLATFORM_STARTING, A733_GPU_PLATFORM_ON, A733_GPU_PLATFORM_STOPPING,
    A733_GPU_PLATFORM_RELEASE, A733_GPU_PLATFORM_OFF, A733_GPU_PLATFORM_FAILED };
struct a733_gpu_platform {
    struct a733_gpu_power power;
    struct a733_gpu_resource_manager *manager;
    struct a733_gpu_platform_ops ops;
    void *context;
    struct a733_gpu_power_resources ready;
    uint64_t token, deadline, last_now;
    unsigned client, initialized, delegated;
    enum a733_gpu_platform_phase phase;
    enum a733_gpu_resource_result resource_result;
    struct a733_pmic_consumer pmic;
    struct a733_gpu_resource_native *pmic_native;
    unsigned pmic_observed;
};
/* Zero storage, initialize once, then serialize all calls through one privileged
 * task. Register this client exclusively; manager/context outlive the platform.
 * No I/O. Ops and manager storage must be disjoint from platform storage. */
int a733_gpu_platform_init(struct a733_gpu_platform *, struct a733_gpu_resource_manager *,
    unsigned client, const struct a733_gpu_platform_ops *, void *);
/* Optional pre-concurrency binding. The adapter must actually back this global
 * manager; its private identity, provider and view stay immutable. A completed
 * DCDC4 observation and rail lease remain held through OFF and later starts. */
int a733_gpu_platform_bind_pmic(struct a733_gpu_platform *,struct a733_gpu_resource_native *,
    struct a733_pmic *,const struct a733_pmic_lifetime *,uint64_t owner);
/* Direct service callback signatures. Begin returns 1 accepted, 0 refused.
 * Poll returns 0 pending, 1 ready, -1 fault. BUSY admission stays pending with
 * no controller acquire call. Faults latch and preserve any resource token.
 * Shared-provider binding polls actual asynchronous rail admission first. The
 * isolated legacy mode uses a synchronous I2C snapshot that can overrun the
 * deadline. Late success is rejected in both modes. Schedule polls without
 * spinning; retain the task after faults. */
int a733_gpu_platform_begin(void *, uint64_t absolute_deadline);
int a733_gpu_platform_poll(void *);
/* Separate deadline, ON only. Require completed firmware shutdown, closed job
 * admission, masked interrupts, and retained device backing. OFF establishes
 * clock/reset/domain state and released resource accounting. It does not prove
 * GPU transaction drain or permit backing reuse; it is not a memory-isolation
 * implementation. During release contention, the platform still owns token. */
int a733_gpu_platform_off_begin(struct a733_gpu_platform *, uint64_t absolute_deadline);
int a733_gpu_platform_off_poll(struct a733_gpu_platform *);
#endif
