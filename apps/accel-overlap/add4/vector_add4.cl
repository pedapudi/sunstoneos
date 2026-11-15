/* SPDX-License-Identifier: MIT
 * Queue argument block: the input binding's 64-bit address, then count.
 * Input contains count unsigned words in each of two consecutive vectors;
 * count is a multiple of four. Binding zero is the output; one work item
 * adds four consecutive words with one 4-word load per vector and writes
 * the sums with two 2-word stores. */
#include "pco/usclib/libcl.h"
KERNEL(32)
cs_queue_add4_common(void)
{
    uint2 arguments=(uint2)(nir_load_vtxin_pco(1,0),nir_load_vtxin_pco(1,1));
    uint2 output=(uint2)(nir_load_vtxin_pco(1,2),nir_load_vtxin_pco(1,3));
    uint3 args=nir_dma_ld_pco(3,arguments);
    uint i=4*(get_group_id(0)*32+get_local_id(0));
    if(i<args.z) {
        uint4 a=nir_dma_ld_pco(4,nir_uadd64_32(args.x,args.y,4*i));
        uint4 b=nir_dma_ld_pco(4,nir_uadd64_32(args.x,args.y,4*(args.z+i)));
        uint4 s=a+b;
        nir_dma_st_pco(1,nir_uadd64_32(output.x,output.y,4*i),s.x,s.y);
        nir_dma_st_pco(1,nir_uadd64_32(output.x,output.y,4*i+8),s.z,s.w);
    }
}
