/* Host shim of the performance-mode part of apps/os/npu.h. */
#ifndef SUNSTONE_OS_NPU_H
#define SUNSTONE_OS_NPU_H
#include <stdint.h>
int os_npu_perf_ready(void);
int os_npu_perf_stopped(void);
int os_npu_perf_retune(unsigned mhz,unsigned sequence,uint64_t deadline);
unsigned os_npu_perf_answer(unsigned sequence);
#endif
