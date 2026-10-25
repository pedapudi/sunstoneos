#ifndef SUNSTONE_NPU_CONVOLUTION_LAYOUT_H
#define SUNSTONE_NPU_CONVOLUTION_LAYOUT_H
#include "npu_convolution.h"

/* Fixed buffer layout of the selected build profile, defined once. Each slot
 * gives its offset inside the single reserved parent allocation (CPU and
 * physical), its size, NPU address and memory role. Descriptors, weights and
 * the shader are CONSTANT: assembled once, cleaned by the bootstrap publication
 * and never written by the CPU afterwards. A zero-sized slot is
 * unused by the profile and must stay zero in the configuration. The private
 * NPU table arena follows the payload at NPU_CONVOLUTION_ARENA_OFFSET.
 * The reserved constructor builds its configuration from this table, and
 * preparation checks the configuration and the assembly plan against it.
 * The values reproduce the recorded layouts in docs/npu-convolution.md,
 * docs/npu-relative-shift.md and docs/npu-complete-nn.md. */
struct npu_convolution_slot {
    size_t offset, bytes;
    uint32_t device;
    enum a733_npu_buffer_role role;
};
#define NPU_CONVOLUTION_TABLE_BYTES 20480u
#define NPU_CONVOLUTION_UNUSED NPU_CONVOLUTION_BUFFERS
#if defined(SUNSTONE_NPU_NN_POOL_GRAPH)
#define NPU_CONVOLUTION_ARENA_OFFSET 0x311000u
static const struct npu_convolution_slot npu_convolution_layout[NPU_CONVOLUTION_BUFFERS]={
    [NPU_CONVOLUTION_NN]={0,24576,0xe0000000,A733_NPU_BUFFER_CONSTANT},
    [NPU_CONVOLUTION_COEFF]={0x6000,892928,0xe0007000,A733_NPU_BUFFER_CONSTANT},
    [NPU_CONVOLUTION_COMMAND]={0x1eb000,524288,0xe01ee000,A733_NPU_BUFFER_COMMAND},
    [NPU_CONVOLUTION_INPUT]={0x26b000,151552,0xe026f000,A733_NPU_BUFFER_READ},
    [NPU_CONVOLUTION_OUTPUT]={0x290000,4096,0xe0295000,A733_NPU_BUFFER_WRITE},
    [NPU_CONVOLUTION_BOOTSTRAP]={0x291000,524288,0xe0297000,A733_NPU_BUFFER_BOOTSTRAP},
    /* The finite image clears the pool once and leaves it to the device; the
     * retained runtime clears it for every job (npu_convolution_send). */
#if defined(SUNSTONE_NPU_FINITE_RETURN)
    [NPU_CONVOLUTION_POOL]={0xe0000,1093632,0xe00e2000,A733_NPU_BUFFER_SCRATCH},
#else
    [NPU_CONVOLUTION_POOL]={0xe0000,1093632,0xe00e2000,A733_NPU_BUFFER_WRITE},
#endif
    [NPU_CONVOLUTION_PPU]={0,0,0,A733_NPU_BUFFER_READ}};
/* Assembly plan allocation order (enum nbg_allocation_id) to buffer slot. */
static const unsigned npu_convolution_allocation[NBG_ALLOCATION_COUNT]={
    NPU_CONVOLUTION_NN,NPU_CONVOLUTION_COEFF,NPU_CONVOLUTION_POOL,
    NPU_CONVOLUTION_COMMAND,NPU_CONVOLUTION_INPUT,NPU_CONVOLUTION_OUTPUT};
#elif defined(SUNSTONE_NPU_LARGE_GRAPH)
#define NPU_CONVOLUTION_ARENA_OFFSET 0x2c3000u
static const struct npu_convolution_slot npu_convolution_layout[NPU_CONVOLUTION_BUFFERS]={
    [NPU_CONVOLUTION_NN]={0,4096,0xe0000000,A733_NPU_BUFFER_CONSTANT},
    [NPU_CONVOLUTION_COEFF]={0,0,0,A733_NPU_BUFFER_READ},
    [NPU_CONVOLUTION_COMMAND]={0x102000,524288,0xe0105000,A733_NPU_BUFFER_COMMAND},
    [NPU_CONVOLUTION_INPUT]={0x182000,524288,0xe0186000,A733_NPU_BUFFER_READ},
    [NPU_CONVOLUTION_OUTPUT]={0x202000,266240,0xe0207000,A733_NPU_BUFFER_WRITE},
    [NPU_CONVOLUTION_BOOTSTRAP]={0x243000,524288,0xe0249000,A733_NPU_BUFFER_BOOTSTRAP},
    [NPU_CONVOLUTION_POOL]={0x2000,1048576,0xe0004000,A733_NPU_BUFFER_WRITE},
    [NPU_CONVOLUTION_PPU]={0x1000,4096,0xe0002000,A733_NPU_BUFFER_CONSTANT}};
/* SH/NT plan order (NBG_SH_NT_*) places the PPU shader in the second entry. */
static const unsigned npu_convolution_allocation[NBG_ALLOCATION_COUNT]={
    NPU_CONVOLUTION_NN,NPU_CONVOLUTION_PPU,NPU_CONVOLUTION_POOL,
    NPU_CONVOLUTION_COMMAND,NPU_CONVOLUTION_INPUT,NPU_CONVOLUTION_OUTPUT};
#else
#define NPU_CONVOLUTION_ARENA_OFFSET 0x104000u
static const struct npu_convolution_slot npu_convolution_layout[NPU_CONVOLUTION_BUFFERS]={
    [NPU_CONVOLUTION_NN]={0,4096,0xe0000000,A733_NPU_BUFFER_CONSTANT},
    [NPU_CONVOLUTION_COEFF]={0x1000,4096,0xe0002000,A733_NPU_BUFFER_CONSTANT},
    [NPU_CONVOLUTION_COMMAND]={0x2000,524288,0xe0004000,A733_NPU_BUFFER_COMMAND},
    [NPU_CONVOLUTION_INPUT]={0x82000,4096,0xe0085000,A733_NPU_BUFFER_READ},
    [NPU_CONVOLUTION_OUTPUT]={0x83000,4096,0xe0087000,A733_NPU_BUFFER_WRITE},
    [NPU_CONVOLUTION_BOOTSTRAP]={0x84000,524288,0xe0089000,A733_NPU_BUFFER_BOOTSTRAP}};
/* The baseline profile has no working pool allocation. */
static const unsigned npu_convolution_allocation[NBG_ALLOCATION_COUNT]={
    NPU_CONVOLUTION_NN,NPU_CONVOLUTION_COEFF,NPU_CONVOLUTION_UNUSED,
    NPU_CONVOLUTION_COMMAND,NPU_CONVOLUTION_INPUT,NPU_CONVOLUTION_OUTPUT};
#endif
#define NPU_CONVOLUTION_RESERVED_BYTES (NPU_CONVOLUTION_ARENA_OFFSET+NPU_CONVOLUTION_TABLE_BYTES)
#endif
