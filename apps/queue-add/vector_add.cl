/* SPDX-License-Identifier: MIT
 * Queue argument block: the input binding's 64-bit address, then count.
 * Input contains count unsigned words in each of two consecutive vectors.
 * Binding zero is the output; one work item writes one unsigned sum. */
#include "pco/usclib/libcl.h"
KERNEL(32)
cs_queue_add_common(void)
{
    uint2 arguments=(uint2)(nir_load_vtxin_pco(1,0),nir_load_vtxin_pco(1,1));
    uint2 output=(uint2)(nir_load_vtxin_pco(1,2),nir_load_vtxin_pco(1,3));
    uint3 args=nir_dma_ld_pco(3,arguments);
    uint i=get_group_id(0)*32+get_local_id(0);
    if(i<args.z) {
        uint2 a=nir_uadd64_32(args.x,args.y,4*i);
        uint2 b=nir_uadd64_32(args.x,args.y,4*(args.z+i));
        uint value=nir_dma_ld_pco(1,a)+nir_dma_ld_pco(1,b);
        uint2 dest=nir_uadd64_32(output.x,output.y,4*i);
        nir_dma_st_pco(1,dest,value);
    }
}
