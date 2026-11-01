/* SPDX-License-Identifier: MIT
 * Row-major FP32 C[M,N]=A[M,K]*B[K,N]. Workgroup tile 8 rows x 4 columns,
 * depth tile 8. The queue argument block contains input pointer, M, N, K.
 * Input packs A then B. Every lane reaches both barriers on every depth tile. */
#include "pco/usclib/libcl.h"
KERNEL2D(4,8)
cs_queue_gemm_common(void)
{
    __local float tile_a[64];
    __local float tile_b[32];
    uint2 arguments=(uint2)(nir_load_vtxin_pco(1,0),nir_load_vtxin_pco(1,1));
    uint2 output=(uint2)(nir_load_vtxin_pco(1,2),nir_load_vtxin_pco(1,3));
    uint4 args=nir_dma_ld_pco(4,arguments);
    uint depth=nir_dma_ld_pco(1,nir_uadd64_32(arguments.x,arguments.y,16));
    uint lane=get_local_id(1)*4+get_local_id(0);
    uint row=get_group_id(1)*8+get_local_id(1);
    uint col=get_group_id(0)*4+get_local_id(0);
    float acc=0.0f;
    for(uint base=0;base<depth;base+=8) {
        for(uint at=lane;at<64;at+=32) {
            uint ar=get_group_id(1)*8+at/8;
            uint ak=base+at%8;
            uint value=0;
            if(ar<args.z&&ak<depth)
                value=nir_dma_ld_pco(1,nir_uadd64_32(args.x,args.y,4*(ar*depth+ak)));
            tile_a[at]=as_float(value);
        }
        uint bk=base+lane/4;
        uint bc=get_group_id(0)*4+lane%4;
        uint value=0;
        if(bk<depth&&bc<args.w)
            value=nir_dma_ld_pco(1,nir_uadd64_32(args.x,args.y,4*(args.z*depth+bk*args.w+bc)));
        tile_b[lane]=as_float(value);
        barrier(CLK_LOCAL_MEM_FENCE);
        for(uint k=0;k<8;++k)
            acc=fma(tile_a[get_local_id(1)*8+k],tile_b[k*4+get_local_id(0)],acc);
        barrier(CLK_LOCAL_MEM_FENCE);
    }
    if(row<args.z&&col<args.w)
        nir_dma_st_pco(1,nir_uadd64_32(output.x,output.y,4*(row*args.w+col)),as_uint(acc));
}
