/* SPDX-License-Identifier: MIT
 * Host execution adapters for the unchanged OpenCL test kernel. */
#define KERNEL2D(x,y) __kernel void
uint host_vtxin(uint offset);
uint host_dma1(uint2 address);
uint4 host_dma4(uint2 address);
void host_store(uint2 address,uint value);
uint2 nir_uadd64_32(uint low,uint high,uint offset);
#define nir_load_vtxin_pco(n,offset) host_vtxin(offset)
#define nir_dma_ld_pco(n,address) host_dma##n(address)
#define nir_dma_st_pco(n,address,value) host_store(address,value)
