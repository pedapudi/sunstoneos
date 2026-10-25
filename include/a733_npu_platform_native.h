#ifndef SUNSTONE_A733_NPU_PLATFORM_NATIVE_H
#define SUNSTONE_A733_NPU_PLATFORM_NATIVE_H
#include <stdint.h>
/* Bounded, ordered EL1 MMIO callbacks of the NPU startup sequencer. The caller
 * holds the register lock for any write. Readable: the reference and parent
 * clocks, the PLL_NPU consumers, the PLL_NPU words, the domain-4 policy and
 * status, and the shared SRAM routing word (SYSCTRL 0x03000008). Writable: the
 * NPU module and bus words, PLL_NPU control and gate control, and the routing
 * bit, each under the field rules in the source. PCK600 is read-only. */
int a733_npu_platform_native_read(void *, uintptr_t, uint32_t *);
int a733_npu_platform_native_write(void *, uintptr_t, uint32_t);
#endif
