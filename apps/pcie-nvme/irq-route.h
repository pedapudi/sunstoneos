#ifndef SUNSTONE_PCIE_NVME_IRQ_ROUTE_H
#define SUNSTONE_PCIE_NVME_IRQ_ROUTE_H
/* Private EL3 boot supplies ordered read32/write32/barrier helpers.
 * SPI153/INTID185: bank5 bit25, configuration11 bits19:18,
 * priority word46 byte1, affinity route185. Delivery remains disabled. */
static __attribute__((noinline)) int pcie_nvme_irq_route(uint64_t mpidr)
{
 const uint32_t bit=UINT32_C(1)<<25;
 if(((read32(0x03400004)&31)+1)*32<=185||(read32(0x03400000)&(1u<<31))||
    ((read32(0x03400114)|read32(0x03400214)|read32(0x03400314))&bit))return 0;
 uint32_t group=read32(0x03400094)|bit;
 uint32_t modifier=read32(0x03400d14)&~bit;
 uint32_t trigger=read32(0x03400c2c)&~(3u<<18);
 uint32_t priority=(read32(0x034004b8)&~(255u<<8))|(0xd8u<<8);
 uint64_t route=(mpidr&0xffffff)|(((mpidr>>32)&255u)<<32);
 write32(0x03400094,group);write32(0x03400d14,modifier);
 write32(0x03400c2c,trigger);write32(0x034004b8,priority);
 *(volatile uint64_t *)(uintptr_t)0x034065c8=route;barrier();
 return read32(0x03400094)==group&&read32(0x03400d14)==modifier&&
  read32(0x03400c2c)==trigger&&read32(0x034004b8)==priority&&
  *(volatile uint64_t *)(uintptr_t)0x034065c8==route&&
  !((read32(0x03400114)|read32(0x03400214)|read32(0x03400314))&bit);
}
#endif
