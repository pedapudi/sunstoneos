#ifndef SUNSTONE_NPU_STARTUP_RETURN_H
#define SUNSTONE_NPU_STARTUP_RETURN_H
#include <stdint.h>
/* The startup client has three profiles: bootstrap only; bootstrap followed
 * by one inference job; and the NPU runtime client (runtime-client.c), which
 * runs one job of each of two catalogue networks through npu_job_run. Each
 * has its own receipt magic and monitor detail. */
#ifdef SUNSTONE_NPU_MODEL_SWITCH
#define NPU_STARTUP_RECEIPT_MAGIC UINT64_C(0x4e50555357433031)
#define NPU_STARTUP_READY UINT64_C(0x4e505557)
#define NPU_STARTUP_JOBS 3u
#define NPU_STARTUP_MAX_STATUS 1u
#define NPU_STARTUP_JOB_TEXT "3"
#elif defined(SUNSTONE_NPU_RUNTIME_RETURN)
#define NPU_STARTUP_RECEIPT_MAGIC UINT64_C(0x4e50555254523031)
#define NPU_STARTUP_READY UINT64_C(0x4e505552)
#define NPU_STARTUP_JOBS 2u
#define NPU_STARTUP_JOB_TEXT "2"
#define NPU_STARTUP_MAX_STATUS 1u /* 1: a copied output differs from its reference. */
#elif defined(SUNSTONE_NPU_SINGLE_INFERENCE)
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
