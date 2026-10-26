/* Host shim of the performance-mode part of apps/os/npu.h. */
#ifndef SUNSTONE_OS_NPU_H
#define SUNSTONE_OS_NPU_H
int os_npu_perf_ready(void);
int os_npu_perf_stopped(void);
int os_npu_perf_retune(unsigned mhz,unsigned sequence);
unsigned os_npu_perf_answer(unsigned sequence);
#endif
