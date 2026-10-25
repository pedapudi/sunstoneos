/* SPDX-License-Identifier: MIT
 * The GPU platform with a733_gpu_platform_abandon (abandon.h), for the
 * wrapper-write variant only. */
#include "../../drivers/gpu/a733_gpu_platform.c"
#include "abandon.h"

int a733_gpu_platform_abandon(struct a733_gpu_platform *p, uint64_t deadline)
{
    if (!p || !p->initialized || p->phase != A733_GPU_PLATFORM_STARTING ||
        p->power.phase != A733_GPU_POWER_STARTING) return 0;
    const uint64_t previous = p->deadline;
    p->deadline = deadline;
    if (!clock_ok(p)) return 0;
    const enum a733_gpu_power_result result = a733_gpu_power_abandon(&p->power, deadline);
    if (result == A733_GPU_POWER_INVALID) { p->deadline = previous; return 0; }
    if (result != A733_GPU_POWER_AGAIN) { fail(p); return 0; }
    p->phase = A733_GPU_PLATFORM_STOPPING;
    return 1;
}
