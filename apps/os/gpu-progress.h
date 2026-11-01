#ifndef SUNSTONE_OS_GPU_PROGRESS_H
#define SUNSTONE_OS_GPU_PROGRESS_H
/* Progress points of the GPU recognition caller (apps/os/gpu_hook.cc and
 * dispatch.c), so that a run which never returns can still name its last
 * step. sunstone_crumb() keeps a 16-bit stage and the low 16 bits of the
 * value. The breadcrumb header defines it; without that header the calls
 * compile to nothing. */
#include <stdint.h>
#if __has_include("sunstone_crumb.h")
#include "sunstone_crumb.h"
#endif
#ifndef SUNSTONE_CRUMB_DEFINED
#define SUNSTONE_CRUMB_DEFINED
static inline void sunstone_crumb(uint32_t stage, uint32_t value) { (void)stage; (void)value; }
#endif

/* Stages in the GPU group (0x04), after the breadcrumb header's own. */
#define OS_GPU_CRUMB_JOB_SUBMIT 0x0450u   /* job submitted; value: tensor ordinal */
#define OS_GPU_CRUMB_JOB_DONE 0x0451u     /* job record received; value: result << 12 | sequence */
#define OS_GPU_CRUMB_BISECT_LIMIT 0x0452u /* bisect limit reached; value: GPU jobs completed */
#define OS_GPU_CRUMB_FAILED 0x0453u       /* dispatch failure; value: 0x100 | failure state */
#endif
