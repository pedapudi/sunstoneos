/* A fixed one-workgroup memory/arithmetic probe for the pinned PCO compiler. */
#include "pco/usclib/libcl.h"

KERNEL(32)
cs_sunstone_probe_common(void)
{
    uint lane = nir_load_instance_num_pco();
    if (lane < 32u) {
        uint2 input = (uint2)(nir_load_vtxin_pco(1, 0), nir_load_vtxin_pco(1, 1));
        uint2 output = (uint2)(nir_load_vtxin_pco(1, 2), nir_load_vtxin_pco(1, 3));
        uint2 values = nir_dma_ld_pco(2, input);
        uint value = (values.x + (lane + 1u) * values.y) ^ (0x9e3779b9u + (lane << 16));
        uint2 destination = nir_uadd64_32(output.x, output.y, lane * 4u);
        nir_dma_st_pco(1, destination, value);
    }
}
