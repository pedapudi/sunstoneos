/* Caller data: count, first vector, second vector; all unsigned words. */
#include "pco/usclib/libcl.h"
KERNEL(32)
cs_vector_add_dispatch_common(void)
{
    uint2 input=(uint2)(nir_load_vtxin_pco(1,0),nir_load_vtxin_pco(1,1));
    uint2 output=(uint2)(nir_load_vtxin_pco(1,2),nir_load_vtxin_pco(1,3));
    uint count=nir_dma_ld_pco(1,input);
    uint lane=nir_load_instance_num_pco();
    uint i=nir_load_vtxin_pco(1,4)+lane;
    if(lane<nir_load_vtxin_pco(1,5)&&i<count) {
        uint2 a=nir_uadd64_32(input.x,input.y,4u+4u*i);
        uint2 b=nir_uadd64_32(input.x,input.y,4u+4u*(count+i));
        uint value=nir_dma_ld_pco(1,a)+nir_dma_ld_pco(1,b);
        uint2 dest=nir_uadd64_32(output.x,output.y,4u*i);
        nir_dma_st_pco(1,dest,value);
    }
}
