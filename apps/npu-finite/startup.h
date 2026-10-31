#ifndef SUNSTONE_NPU_STARTUP_RETURN_H
#define SUNSTONE_NPU_STARTUP_RETURN_H
#include <stdint.h>
#define NPU_STARTUP_RECEIPT_MAGIC UINT64_C(0x4e50555354415231)
#define NPU_STARTUP_READY UINT64_C(0x4e505553)
/* EL1 client only. Existing quiescence and successful CLOSE remain required. */
int npu_startup_return_ready(void);
#endif
