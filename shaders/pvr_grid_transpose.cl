/* Transpose a row-major unsigned matrix; count,width,height precede its data. */
#include "pco/usclib/libcl.h"
KERNEL2D(8,4)
cs_grid_transpose_common(void)
{
    uint2 input=(uint2)(nir_load_vtxin_pco(1,0),nir_load_vtxin_pco(1,1));
    uint2 output=(uint2)(nir_load_vtxin_pco(1,2),nir_load_vtxin_pco(1,3));
    uint3 header=nir_dma_ld_pco(3,input);
    uint x=get_group_id(0)*8+get_local_id(0);
    uint y=get_group_id(1)*4+get_local_id(1);
    uint i=y*header.y+x;
    if(x<min(header.y,(uint)get_global_size(0))&&y<min(header.z,(uint)get_global_size(1))&&i<header.x) {
        uint2 source=nir_uadd64_32(input.x,input.y,12+4*i);
        uint value=nir_dma_ld_pco(1,source);
        uint2 dest=nir_uadd64_32(output.x,output.y,4*(x*header.z+y));
        nir_dma_st_pco(1,dest,value);
    }
}
