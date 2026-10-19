#ifndef SUNSTONE_UART_IRQ_ROUTE_H
#define SUNSTONE_UART_IRQ_ROUTE_H
/* Included by the private EL3 boot unit after its ordered MMIO helpers.
 * UART2 is SPI4/INTID36. The native owner enables delivery later; this
 * routine configures only a disabled, inactive source with no pending event.
 */
static __attribute__((noinline)) int uart_irq_route(uint64_t mpidr)
{
    const uint32_t bit=1u<<4;
    if((read32(0x03400000)&(1u<<31)) ||
       ((read32(0x03400104)|read32(0x03400204)|read32(0x03400304))&bit))return 0;
    uint32_t group=read32(0x03400084)|bit;
    uint32_t modifier=read32(0x03400d04)&~bit;
    uint32_t trigger=read32(0x03400c08)&~(3u<<8);
    uint32_t priority=(read32(0x03400424)&~255u)|0xd8u;
    uint64_t route=(mpidr&0xffffff)|(((mpidr>>32)&255u)<<32);
    write32(0x03400084,group);
    write32(0x03400d04,modifier);
    write32(0x03400c08,trigger);
    write32(0x03400424,priority);
    *(volatile uint64_t *)(uintptr_t)0x03406120=route;
    barrier();
    return read32(0x03400084)==group && read32(0x03400d04)==modifier &&
        read32(0x03400c08)==trigger && read32(0x03400424)==priority &&
        *(volatile uint64_t *)(uintptr_t)0x03406120==route &&
        !((read32(0x03400104)|read32(0x03400204)|read32(0x03400304))&bit);
}
#endif
