#ifndef SUNSTONE_A733_GPU_PLL_WATCH_H
#define SUNSTONE_A733_GPU_PLL_WATCH_H
#include <stdint.h>
/* In an image built with SUNSTONE_ACCEL_PLL_WATCH, the native GPU platform (drivers/gpu/a733_gpu_platform_native.c) reads
 * PLL_NPU's control word (CCU 0x2a0) immediately before and after each PLL_GPU0 write, inside the platform register lock that
 * the write holds, and passes both words here with the write's clear and set masks. The OS records them beside the NPU ring
 * (apps/os/npu-queue.c, struct ss_npu_queue_pll_watch). Task context only; the function never blocks. */
#define A733_PLL_NPU_ADDRESS UINT32_C(0x020022a0)
void a733_gpu_pll_watch(uint32_t clear, uint32_t set, uint32_t npu_before, uint32_t npu_after);
#endif
