#ifndef SUNSTONE_NPU_CRUMB_H
#define SUNSTONE_NPU_CRUMB_H
#include <stdint.h>
/* Progress points for locating a hang. The watchdog/breadcrumb branch defines
 * sunstone_crumb and SUNSTONE_CRUMB_DEFINED; until then the calls compile away.
 * Stage values are NPU_CRUMB_*; value is the stage's ticket, count or reason. */
#ifndef SUNSTONE_CRUMB_DEFINED
#define SUNSTONE_CRUMB_DEFINED 1
static inline void sunstone_crumb(uint32_t stage,uint32_t value){(void)stage;(void)value;}
#endif
#define NPU_CRUMB(n) (UINT32_C(0x4e500000)|(uint32_t)(n))
enum {
    NPU_CRUMB_WAIT_STARTUP=NPU_CRUMB(1),NPU_CRUMB_OPEN=NPU_CRUMB(2),NPU_CRUMB_BOOTSTRAP_READY=NPU_CRUMB(3),
    NPU_CRUMB_SUBMIT=NPU_CRUMB(4),NPU_CRUMB_JOB_DONE=NPU_CRUMB(5),NPU_CRUMB_OUTPUT_COPIED=NPU_CRUMB(6),
    NPU_CRUMB_REAPED=NPU_CRUMB(7),NPU_CRUMB_CLOSE=NPU_CRUMB(8),NPU_CRUMB_LOCAL_STOP=NPU_CRUMB(9),
    NPU_CRUMB_QUIESCENT=NPU_CRUMB(10),NPU_CRUMB_EXIT=NPU_CRUMB(11),NPU_CRUMB_RETAINED=NPU_CRUMB(0xff)
};
#endif
