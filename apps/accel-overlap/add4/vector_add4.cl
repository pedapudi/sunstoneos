/* SPDX-License-Identifier: MIT
 * Inline argument ABI (GPU_QUEUE_ABI_GRID_INLINE, include/gpu_queue.h): the
 * PDS writes the job's argument block into unified registers once per task,
 * so no lane loads it. Registers 0-1 hold the input binding's 64-bit address,
 * register 4 the count, and registers 2-3 binding zero, the output.
 * Input contains count unsigned words in each of two consecutive vectors;
 * count is a multiple of four. One work item adds four consecutive words with
 * one 4-word load per vector and writes the sums with two 2-word stores. */
#include "pco/usclib/libcl.h"
KERNEL(32)
cs_queue_add4_common(void)
{
    uint2 input=(uint2)(nir_load_vtxin_pco(1,0),nir_load_vtxin_pco(1,1));
    uint2 output=(uint2)(nir_load_vtxin_pco(1,2),nir_load_vtxin_pco(1,3));
    uint count=nir_load_vtxin_pco(1,4);
    uint i=4*(get_group_id(0)*32+get_local_id(0));
    if(i<count) {
        uint4 a=nir_dma_ld_pco(4,nir_uadd64_32(input.x,input.y,4*i));
        uint4 b=nir_dma_ld_pco(4,nir_uadd64_32(input.x,input.y,4*(count+i)));
        uint4 s=a+b;
        nir_dma_st_pco(1,nir_uadd64_32(output.x,output.y,4*i),s.x,s.y);
        nir_dma_st_pco(1,nir_uadd64_32(output.x,output.y,4*i+8),s.z,s.w);
    }
}
