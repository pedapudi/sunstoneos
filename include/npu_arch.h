#ifndef SUNSTONE_NPU_ARCH_H
#define SUNSTONE_NPU_ARCH_H
#include <stddef.h>
#include <stdint.h>
/* The few AArch64 system instructions that the NPU runtime and service use.
 * On the board they are the instructions; the host check shim defines the
 * four functions on another architecture, so that the real runtime and
 * service units compile there. */
#ifdef __aarch64__
static inline uint64_t npu_arch_current_el(void)
{uint64_t v;__asm__ volatile("mrs %0,CurrentEL":"=r"(v));return v;}
static inline uint64_t npu_arch_stack_select(void)
{uint64_t v;__asm__ volatile("mrs %0,spsel":"=r"(v));return v;}
static inline int npu_arch_zva_block_is_64(void)
{uint64_t v;__asm__ volatile("mrs %0, dczid_el0":"=r"(v));return (v&0x1f)==4;}
static inline void npu_arch_zero_line(uintptr_t at)
{__asm__ volatile("dc zva, %0"::"r"(at):"memory");}
#else
uint64_t npu_arch_current_el(void);
uint64_t npu_arch_stack_select(void);
int npu_arch_zva_block_is_64(void);
void npu_arch_zero_line(uintptr_t at);
#endif
#endif
