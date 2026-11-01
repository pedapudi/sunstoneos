#ifndef SUNSTONE_OS_GPU_RUNTIME_H
#define SUNSTONE_OS_GPU_RUNTIME_H
#include "a733_gpu_platform_native.h"
#define OS_GPU_PRIORITY 4u
#define OS_GPU_OPEN_US UINT64_C(1000000)
#define OS_GPU_MAX_JOB_US UINT64_C(1000000)
#define OS_GPU_REPLY_US (2 * OS_GPU_MAX_JOB_US)
#define OS_GPU_WAIT_TICKS 10u
int os_gpu_queue_quiesce(void);
#ifdef SUNSTONE_A733_OS_GPU_BISECT
#if !defined(SUNSTONE_A733_OS_CPU_RETURN) || OS_GPU_BISECT_STAGE < 1 || OS_GPU_BISECT_STAGE > 4 || \
    (OS_GPU_BISECT_STAGE == 3 && OS_GPU_BISECT_LIMIT < 1)
#error "GPU diagnostics require a returning image and a valid limit"
#endif
#define OS_GPU_RETURN_VERSION 3
#define OS_GPU_RETURN_VALUE ((uint64_t)OS_GPU_BISECT_STAGE << 32 | OS_GPU_BISECT_LIMIT)
#else
#define OS_GPU_RETURN_VERSION 1
#define OS_GPU_RETURN_VALUE A733_GPU_SUPPLY_UV
#endif
#endif
