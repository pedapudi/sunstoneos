/* Caller data: count, factor, bias, count unsigned words. */
#include "pco/usclib/libcl.h"
KERNEL(32)
cs_vector_affine_common(void)
{
    uint2 input=(uint2)(nir_load_vtxin_pco(1,0),nir_load_vtxin_pco(1,1));
    uint2 output=(uint2)(nir_load_vtxin_pco(1,2),nir_load_vtxin_pco(1,3));
    uint3 header=nir_dma_ld_pco(3,input);
    for(uint i=nir_load_instance_num_pco();i<header.x;i+=32u) {
        uint2 source=nir_uadd64_32(input.x,input.y,12u+4u*i);
        uint value=nir_dma_ld_pco(1,source);
        uint2 dest=nir_uadd64_32(output.x,output.y,4u*i);
        nir_dma_st_pco(1,dest,value*header.y+header.z);
    }
}
