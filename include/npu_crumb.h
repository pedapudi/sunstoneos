#ifndef SUNSTONE_NPU_JOB_CRUMB_H
#define SUNSTONE_NPU_JOB_CRUMB_H
#include <stdint.h>
/* Progress points of the NPU job helper (npu_job.h), for locating a hang. The
 * progress-record header sunstone_crumb.h defines sunstone_crumb and
 * SUNSTONE_CRUMB_DEFINED; without it the calls compile to nothing. The record
 * keeps 16 bits of each stage and of each value; NPU stages use group 0x06,
 * and the helper's stages are 0x0620 to 0x062f. value is the stage's slot,
 * model, ticket, byte count or reason. */
#if defined(__has_include)
#if __has_include("sunstone_crumb.h")
#include "sunstone_crumb.h"
#endif
#endif
#ifndef SUNSTONE_CRUMB_DEFINED
#define SUNSTONE_CRUMB_DEFINED 1
static inline void sunstone_crumb(uint32_t stage, uint32_t value) { (void)stage; (void)value; }
#endif
#define NPU_JOB_CRUMB(n) (UINT32_C(0x0600) | (uint32_t)(n))
enum {
    NPU_JOB_CRUMB_OPEN = NPU_JOB_CRUMB(0x20), NPU_JOB_CRUMB_LOAD = NPU_JOB_CRUMB(0x21),
    NPU_JOB_CRUMB_SUBMIT = NPU_JOB_CRUMB(0x22), NPU_JOB_CRUMB_DONE = NPU_JOB_CRUMB(0x23),
    NPU_JOB_CRUMB_COPIED = NPU_JOB_CRUMB(0x24), NPU_JOB_CRUMB_CLOSE = NPU_JOB_CRUMB(0x25),
    NPU_JOB_CRUMB_REFUSED = NPU_JOB_CRUMB(0x2e), NPU_JOB_CRUMB_FAILED = NPU_JOB_CRUMB(0x2f)
};
#endif
