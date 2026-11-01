#ifndef SUNSTONE_NPU_STARTUP_RETURN_H
#define SUNSTONE_NPU_STARTUP_RETURN_H
#include <stdint.h>
/* The startup client has two profiles: bootstrap only, or bootstrap followed
 * by one inference job. Each has its own receipt magic and monitor detail. */
#ifdef SUNSTONE_NPU_SINGLE_INFERENCE
#define NPU_STARTUP_RECEIPT_MAGIC UINT64_C(0x4e5055494e463031)
#define NPU_STARTUP_READY UINT64_C(0x4e505549)
#define NPU_STARTUP_JOBS 1u
#define NPU_STARTUP_MAX_STATUS 1u /* 1: the copied output differs from the reference. */
#else
#define NPU_STARTUP_RECEIPT_MAGIC UINT64_C(0x4e50555354415231)
#define NPU_STARTUP_READY UINT64_C(0x4e505553)
#define NPU_STARTUP_JOBS 0u
#define NPU_STARTUP_MAX_STATUS 0u
#endif
/* EL1 client only. Existing quiescence and successful CLOSE remain required. */
int npu_startup_return_ready(void);
#endif
