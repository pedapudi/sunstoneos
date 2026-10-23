/* Host model of the USC intrinsics that shaders/pvr_mimus_linear.cl uses.
 * checks/gpu-mimus-layers.py compiles the kernel source as C with this header
 * after replacing each OpenCL vector literal "(uint2)(" with "mk2(". Loads and
 * stores go through checks/gpu-mimus-layers.cc, which translates GPU
 * addresses with the session's permissions and records faults. */
#ifndef GPU_MIMUS_LAYERS_SHIM_H
#define GPU_MIMUS_LAYERS_SHIM_H
#include <stdbool.h>
#include <stdint.h>
typedef uint32_t uint;
typedef struct { uint x, y; } uint2;
typedef struct { uint x, y, z, w; } uint4;
static inline uint2 mk2(uint x, uint y) { uint2 v = { x, y }; return v; }
static inline uint2 gpu_add64(uint lo, uint hi, uint off)
{
    uint64_t v = ((uint64_t)hi << 32 | lo) + off;
    return mk2((uint)v, (uint)(v >> 32));
}
extern uint gpu_vtxin[4], gpu_group[2], gpu_local[2];
uint gpu_ld1(uint2 address);
uint4 gpu_ld4(uint2 address);
void gpu_st(int fence, uint2 address, const uint *words, unsigned count);
#define KERNEL2D(x, y) static void
#define nir_load_vtxin_pco(n, i) gpu_vtxin[i]
#define nir_dma_ld_pco(n, a) gpu_ld##n(a)
#define nir_uadd64_32(lo, hi, off) gpu_add64(lo, hi, off)
#define nir_dma_st_pco(f, a, ...) \
    gpu_st((f), (a), (const uint[]){ __VA_ARGS__ }, sizeof((const uint[]){ __VA_ARGS__ }) / sizeof(uint))
#define get_group_id(i) gpu_group[i]
#define get_local_id(i) gpu_local[i]
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif
