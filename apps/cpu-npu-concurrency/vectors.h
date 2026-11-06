#ifndef CPU_NPU_VECTORS_H
#define CPU_NPU_VECTORS_H
#include <stddef.h>
#include <stdint.h>
/* Only the reference output is linked: the graph and the input arrive in input regions (main.c). */
extern const uint8_t cpu_npu_reference[];
extern const size_t cpu_npu_reference_bytes;
#endif
