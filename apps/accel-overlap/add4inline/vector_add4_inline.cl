/* SPDX-License-Identifier: MIT
 * The four-elements-per-lane add (../add4/vector_add4.cl) for the inline
 * argument ABI (GPU_QUEUE_ABI_GRID_INLINE, include/gpu_queue.h), in which
 * the PDS writes the argument block into unified registers. The job's block
 * is binding 1's address (registers 0-1), then count (register 4) and mode
 * (register 5); registers 2-3 hold binding zero, the output.
 * - Mode 0, arguments once per task: binding 1 is the input and no lane
 *   loads its arguments.
 * - Mode 1, arguments per lane: binding 1 is a 12-byte record of the input's
 *   64-bit address and count, which every lane loads, as the add4 kernel
 *   loads its argument block; the register count is unused.
 * The mode is the same for every lane of a job. Input contains count unsigned
 * words in each of two consecutive vectors; count is a multiple of four. One
 * work item adds four consecutive words with one 4-word load per vector and
 * writes the sums with two 2-word stores. */
#include "pco/usclib/libcl.h"
KERNEL(32)
cs_queue_add4_inline(void)
{
    uint2 input=(uint2)(nir_load_vtxin_pco(1,0),nir_load_vtxin_pco(1,1));
    uint2 output=(uint2)(nir_load_vtxin_pco(1,2),nir_load_vtxin_pco(1,3));
    uint count=nir_load_vtxin_pco(1,4);
    if(nir_load_vtxin_pco(1,5)) {
        uint3 args=nir_dma_ld_pco(3,input);
        input=args.xy;
        count=args.z;
    }
    uint i=4*(get_group_id(0)*32+get_local_id(0));
    if(i<count) {
        uint4 a=nir_dma_ld_pco(4,nir_uadd64_32(input.x,input.y,4*i));
        uint4 b=nir_dma_ld_pco(4,nir_uadd64_32(input.x,input.y,4*(count+i)));
        uint4 s=a+b;
        nir_dma_st_pco(1,nir_uadd64_32(output.x,output.y,4*i),s.x,s.y);
        nir_dma_st_pco(1,nir_uadd64_32(output.x,output.y,4*i+8),s.z,s.w);
    }
}
