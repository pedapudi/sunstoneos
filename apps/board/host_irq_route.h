#ifndef SUNSTONE_BOARD_HOST_IRQ_ROUTE_H
#define SUNSTONE_BOARD_HOST_IRQ_ROUTE_H
/* Private EL3 boot helpers, included after ordered MMIO accessors. USB2 xHCI
 * is SPI155/INTID187. The host task is pinned to CPU0 before scheduling.
 * This retained application has no normal-return path or route release. */
static __attribute__((noinline)) int board_host_irq_admit(void)
{
    const struct a733_board_session *s=board_session_snapshot();
    if(!s)return 0;
    if(!a733_board_host_workload(s->workload))return 1;
    uint64_t el,mpidr;
    __asm__ volatile("mrs %0,CurrentEL\n mrs %1,mpidr_el1" : "=r"(el),"=r"(mpidr));
    if(el!=12 || (mpidr&UINT64_C(0xff00ffffff)) ||
       a733_smp_counter()>=admission.startup_deadline)return 0;
    uint32_t ctlr=read32(0x03400000);
    if((ctlr&0x70u)!=0x30u || (ctlr&(1u<<31)) ||
       (read32(0x03400004)&31u)<5 ||
       (*(volatile uint64_t *)(uintptr_t)0x03460008>>32)!=0 ||
       (read32(0x03460000)&9u))return 0;
    return !((read32(0x03400114)|read32(0x03400214)|read32(0x03400314))&(1u<<27));
}
static __attribute__((noinline)) int board_host_irq_route(void)
{
    if(!board_host_irq_admit())return 0;
    const struct a733_board_session *s=board_session_snapshot();
    if(!a733_board_host_workload(s->workload))return 1;
    /* Common boot has propagated all SPI disables and disabled the groups.
     * Configure only this source, then leave enabling to its native owner. */
    if(read32(0x03400000)&7u)return 0;
    const uint32_t bit=1u<<27;
    uint32_t group=read32(0x03400094)|bit;
    uint32_t modifier=read32(0x03400d14)&~bit;
    uint32_t configuration=read32(0x03400c2c)&~(3u<<22);
    uint32_t priority=(read32(0x034004b8)&0x00ffffffu)|0xd8000000u;
    write32(0x03400094,group);write32(0x03400d14,modifier);
    write32(0x03400c2c,configuration);write32(0x034004b8,priority);
    /* Use a single 64-bit access for the complete affinity route. */
    *(volatile uint64_t *)(uintptr_t)0x034065d8=0;
    barrier();
    return read32(0x03400094)==group && read32(0x03400d14)==modifier &&
        read32(0x03400c2c)==configuration && read32(0x034004b8)==priority &&
        *(volatile uint64_t *)(uintptr_t)0x034065d8==0 &&
        !((read32(0x03400114)|read32(0x03400214)|read32(0x03400314))&bit) &&
        !(read32(0x03400000)&(1u<<31)) && a733_smp_counter()<admission.startup_deadline;
}
#endif
