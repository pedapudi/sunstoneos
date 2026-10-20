#ifndef SUNSTONE_SPI_IRQ_ROUTE_H
#define SUNSTONE_SPI_IRQ_ROUTE_H
/* Included in private EL3 boot after its ordered MMIO helpers.
 * SPI3 uses INTID59: bank1 bit27, configuration3 bits23:22,
 * priority word14 byte3. Delivery remains masked until the owner arms it. */
static __attribute__((noinline)) int spi_irq_quiet(void)
{
    const uint32_t bit=UINT32_C(1)<<27;
    return ((read32(0x03400004)&31)+1)*32>59 &&
        !(read32(0x03400000)&(UINT32_C(1)<<31)) &&
        !((read32(0x03400104)|read32(0x03400204)|read32(0x03400304))&bit);
}
static __attribute__((noinline)) int spi_irq_route(uint64_t mpidr)
{
    const uint32_t bit=UINT32_C(1)<<27;
    uint64_t route=(mpidr&0xffffff)|(((mpidr>>32)&255u)<<32);
    if(route || !spi_irq_quiet())return 0;
    uint32_t group=read32(0x03400084)|bit;
    uint32_t modifier=read32(0x03400d04)&~bit;
    uint32_t trigger=read32(0x03400c0c)&~(3u<<22);
    /* Secure priority 0xd8 is Nonsecure 0xb0, the selected port's API limit. */
    uint32_t priority=(read32(0x03400438)&UINT32_C(0x00ffffff))|UINT32_C(0xd8000000);
    write32(0x03400084,group);
    write32(0x03400d04,modifier);
    write32(0x03400c0c,trigger);
    write32(0x03400438,priority);
    *(volatile uint64_t *)(uintptr_t)0x034061d8=route;
    barrier();
    return read32(0x03400084)==group && read32(0x03400d04)==modifier &&
        read32(0x03400c0c)==trigger && read32(0x03400438)==priority &&
        *(volatile uint64_t *)(uintptr_t)0x034061d8==route && spi_irq_quiet();
}
#endif
