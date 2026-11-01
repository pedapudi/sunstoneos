/* SPDX-License-Identifier: MIT
 * Direct FP32 control with the same argument, output and workgroup geometry
 * as tiled_gemm.cl. Each work item reads its own A row and B column. */
#include "pco/usclib/libcl.h"
KERNEL2D(4,8)
cs_queue_gemm_common(void)
{
    uint2 arguments=(uint2)(nir_load_vtxin_pco(1,0),nir_load_vtxin_pco(1,1));
    uint2 output=(uint2)(nir_load_vtxin_pco(1,2),nir_load_vtxin_pco(1,3));
    uint4 args=nir_dma_ld_pco(4,arguments);
    uint depth=nir_dma_ld_pco(1,nir_uadd64_32(arguments.x,arguments.y,16));
    uint row=get_group_id(1)*8+get_local_id(1);
    uint col=get_group_id(0)*4+get_local_id(0);
    if(row<args.z&&col<args.w) {
        float acc=0.0f;
        for(uint k=0;k<depth;++k) {
            uint a=nir_dma_ld_pco(1,nir_uadd64_32(args.x,args.y,4*(row*depth+k)));
            uint b=nir_dma_ld_pco(1,nir_uadd64_32(args.x,args.y,4*(args.z*depth+k*args.w+col)));
            acc=fma(as_float(a),as_float(b),acc);
        }
        nir_dma_st_pco(1,nir_uadd64_32(output.x,output.y,4*(row*args.w+col)),as_uint(acc));
    }
}
