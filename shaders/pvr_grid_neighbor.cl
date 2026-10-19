/* Horizontal three-point sum with clipped edges, on a 2D unsigned matrix. */
#include "pco/usclib/libcl.h"
KERNEL(32)
cs_grid_neighbor_common(void)
{
    uint2 input=(uint2)(nir_load_vtxin_pco(1,0),nir_load_vtxin_pco(1,1));
    uint2 output=(uint2)(nir_load_vtxin_pco(1,2),nir_load_vtxin_pco(1,3));
    uint3 header=nir_dma_ld_pco(3,input);
    uint x=get_group_id(0)*32+get_local_id(0);
    uint y=get_group_id(1)+get_local_id(1);
    uint i=y*header.y+x;
    if(x<min(header.y,(uint)get_global_size(0))&&y<min(header.z,(uint)get_global_size(1))&&i<header.x) {
        uint2 source=nir_uadd64_32(input.x,input.y,12+4*i);
        uint value=nir_dma_ld_pco(1,source);
        if(x>0) {
            source=nir_uadd64_32(input.x,input.y,12+4*(i-1));
            value+=nir_dma_ld_pco(1,source);
        }
        if(x+1<header.y) {
            source=nir_uadd64_32(input.x,input.y,12+4*(i+1));
            value+=nir_dma_ld_pco(1,source);
        }
        uint2 dest=nir_uadd64_32(output.x,output.y,4*i);
        nir_dma_st_pco(1,dest,value);
    }
}
