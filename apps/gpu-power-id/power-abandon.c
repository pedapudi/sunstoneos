/* SPDX-License-Identifier: MIT
 * The GPU power controller with a733_gpu_power_abandon (abandon.h), for the
 * wrapper-write variant only. */
#include "../../drivers/gpu/a733_gpu_power.c"
#include "abandon.h"

enum a733_gpu_power_result a733_gpu_power_abandon(struct a733_gpu_power *s, uint64_t deadline)
{
    if (!s || !s->ops.now || !s->row.domains || s->phase != A733_GPU_POWER_STARTING ||
        s->index != sizeof(on_steps) / sizeof(*on_steps) - 1) return A733_GPU_POWER_INVALID;
    /* The OFF steps start with CORE policy 0 and its readback, then poll
     * CORE status 0, close the GPU reset and gates, remove the domain-5 row
     * and return the delegated resources. */
    s->phase = A733_GPU_POWER_ON;
    return begin(s, deadline, 0);
}
