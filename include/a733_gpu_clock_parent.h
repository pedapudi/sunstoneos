#ifndef SUNSTONE_A733_GPU_CLOCK_PARENT_H
#define SUNSTONE_A733_GPU_CLOCK_PARENT_H
#include "a733_clock_leases.h"
#include "a733_gpu_resources.h"

enum a733_gpu_clock_phase { A733_GPU_CLOCK_NEW, A733_GPU_CLOCK_COMMON,
    A733_GPU_CLOCK_LEGACY, A733_GPU_CLOCK_READY, A733_GPU_CLOCK_FAILED };
struct a733_gpu_clock_parent {
    struct a733_clock_leases *registry;
    struct a733_gpu_resource_manager *resources;
    struct a733_clock_token common;
    uint64_t owner, token, deadline, last_now;
    uint32_t client, phase;
};
/* Startup bridge between the existing GPU manager and common clock ownership.
 * Initialize before either manager is used concurrently, with no GPU leases.
 * Reserve one privileged GPU client ID for this object's platform lifetime.
 * Poll to READY before admitting GPU, audio or other clock clients. Registry
 * and GPU callbacks use the same monotonic microsecond clock. No init I/O.
 * READY holds shared PERI0/REF/OSC in the common registry and PARENT/OSC in the
 * GPU manager. The latter verifies inherited 24 MHz reference and 400 MHz
 * parent, and blocks bypass through the legacy change-guard API. No gate,
 * regulator, PLL or GPU-local writes occur. Existing GPU LOCAL leases can then
 * coexist, close and reopen while parent stability remains reserved.
 * No teardown or retune API: both tokens and this storage live until reboot.
 * Future retuning needs coordinated verified release, not token deletion.
 */
int a733_gpu_clock_parent_init(struct a733_gpu_clock_parent *,struct a733_clock_leases *,
    struct a733_gpu_resource_manager *,unsigned client,uint64_t owner,uint64_t deadline);
/* 1 ready, 0 pending, -1 retained failure. One bounded acquisition per poll;
 * no spin/retry loop, no automatic rollback. Terminal calls perform no I/O. */
int a733_gpu_clock_parent_poll(struct a733_gpu_clock_parent *);
#endif
