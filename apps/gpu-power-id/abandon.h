/* SPDX-License-Identifier: MIT
 * Diagnostic-only calls of the wrapper-write variant. power-abandon.c and
 * platform-abandon.c compile the shared controller and platform sources
 * with these calls added, in place of those sources, so the shared files and
 * every other image stay unchanged. */
#ifndef SUNSTONE_GPU_POWER_ID_ABANDON_H
#define SUNSTONE_GPU_POWER_ID_ABANDON_H
#include "a733_gpu_platform.h"
/* Turn an ON that waits at its last step, the CORE status poll, into the
 * OFF steps from their start, with a new deadline. Returns AGAIN when
 * accepted, FAULT if the clock read then fails, and INVALID with no change
 * in any other state. */
enum a733_gpu_power_result a733_gpu_power_abandon(struct a733_gpu_power *, uint64_t deadline);
/* From STARTING with the controller at its CORE status poll, begin OFF as
 * a733_gpu_platform_off_begin does from ON; continue with
 * a733_gpu_platform_off_poll. Returns 1 accepted and 0 refused. A refusal
 * in any other state changes nothing; a failed clock read fails the
 * platform as off_begin does. */
int a733_gpu_platform_abandon(struct a733_gpu_platform *, uint64_t absolute_deadline);
#endif
