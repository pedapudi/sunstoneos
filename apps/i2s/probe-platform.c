#include "probe.h"
#include "boot.h"
#include "sunstone_probe.h"
#include "a733_mmio.h"
#define CCU UINT32_C(0x02002000)
#define DMA UINT32_C(0x04601000)
#define BIT(n) (UINT32_C(1)<<(n))
struct i2s_probe_saved i2s_probe_saved __attribute__((section(".monitor_state"),aligned(64)));
_Static_assert(sizeof(struct i2s_probe_saved)%64==0,"EL3 snapshot has dedicated cache lines");
extern void i2s_probe_monitor_init(void);
extern uint64_t i2s_probe_monitor_restore(void);
static void fault(unsigned site,uint32_t value)
{
    uint64_t el;__asm__ volatile("mrs %0,CurrentEL":"=r"(el));
    if(el==12)sunstone_probe_fault(site,value,(uintptr_t)__builtin_return_address(0),0);
    sunstone_probe_fail(site,value);
}
static uint32_t rd(uintptr_t a){return a733_mmio_read32_dsb_sy_both(a);}
static void wr(uintptr_t a,uint32_t v){a733_mmio_write32_dsb_sy_both(a,v);}
static void wait_mask(uintptr_t a,uint32_t mask,uint32_t expected,unsigned site)
{
    uint64_t end=counter()+COUNTER_HZ/10;
    for(;;){uint32_t v=rd(a);if(counter()>=end)fault(site,v);if((v&mask)==expected)return;}
}
static void same(uintptr_t a,uint32_t mask,uint32_t expected,unsigned site)
{uint32_t v=rd(a);if((v^expected)&mask)fault(site,v);}
static void delay(unsigned us)
{uint64_t end=counter()+us*(COUNTER_HZ/1000000);while(counter()<end){} }
static void gates(uint32_t wanted)
{
    /* Manual 4.1.4.6: enable outputs before forcing No-Auto. Completion is
     * the effective gate status, not readback of the policy register. */
    uint32_t v=rd(CCU+0x191c);wr(CCU+0x191c,v|0x60000);wr(CCU+0x191c,v|0x60006);
    if(wanted)wait_mask(CCU+0x199c,0x60000,0x60000,0x41);
}
void monitor_init(void)
{
    RESULT->mode=0; /* This image's modes come from its retained input object. */
    i2s_probe_monitor_init();
    if(!sunstone_probe_input.mode)return;
    struct i2s_probe_saved *s=&i2s_probe_saved;
    s->bus=rd(CCU+0x704);s->mbus=rd(CCU+0x5e4);s->audio=rd(CCU+0x280);s->audio_gate=rd(CCU+0x191c);
    if(!(s->bus&BIT(16)))delay(20); /* DMA reset remains asserted for 20 us. */
    wr(CCU+0x704,s->bus|BIT(16));wr(CCU+0x704,s->bus|0x10001);wr(CCU+0x5e4,s->mbus|1);
    same(CCU+0x704,0x10001,0x10001,0x40);same(CCU+0x5e4,1,1,0x40);
    /* Own the controller only after all preceding memory transactions finish. */
    wait_mask(DMA+0x30,0x8000ffff,0,0x42);
    s->security=rd(DMA+0x20);s->cpu=rd(DMA+0x34);s->mcu=rd(DMA+0x38);s->automatic=rd(DMA+0x28);
    for(unsigned ch=0;ch<2;++ch){
        uintptr_t a=DMA+0x100+ch*64;
        uint32_t busy=rd(a)|rd(a+4)|rd(a+0x28)|rd(a+0x34)|rd(a+0x38)|rd(a+0x3c);
        if(busy)fault(0x43,busy);
    }
    /* The DMA SPI is shared by every channel; an inherited notification
     * cannot be discarded merely because no transfer is currently busy. */
    for(unsigned ch=0;ch<16;++ch){
        uintptr_t a=DMA+0x100+64*ch;
        uint32_t pending=rd(a+0x34)|rd(a+0x38);if(pending)fault(0x43,pending);
    }
    wr(DMA+0x20,s->security|3);wr(DMA+0x28,4);
    wr(DMA+0x38,s->mcu&~3u);wr(DMA+0x34,s->cpu|3);
    if((rd(DMA+0x20)&3)!=3)fault(0x44,rd(DMA+0x20));
    gates(s->audio&BIT(27));
    unsigned bank=143/32,bit=143%32;
    uintptr_t group=GICD_BASE+0x80+4*bank,mod=GICD_BASE+0xd00+4*bank;
    uintptr_t cfg=GICD_BASE+0xc00+4*(143/16);
    s->group=rd(group);s->modifier=rd(mod);s->priority=*(volatile uint8_t *)(GICD_BASE+0x400+143);s->configuration=rd(cfg);
    s->route=*(volatile uint64_t *)(GICD_BASE+0x6000+8*143);
    wr(GICD_BASE+0x180+4*bank,BIT(bit));wait_mask(GICD_BASE,BIT(31),0,0x45);
    if(rd(GICD_BASE+0x300+4*bank)&BIT(bit))fault(0x46,rd(GICD_BASE+0x300+4*bank));
    if(rd(GICD_BASE+0x200+4*bank)&BIT(bit))fault(0x46,rd(GICD_BASE+0x200+4*bank));
    wr(group,s->group|BIT(bit));wr(mod,s->modifier&~BIT(bit));
    wr(cfg,s->configuration&~(3u<<(2*(143%16))));
    *(volatile uint8_t *)(GICD_BASE+0x400+143)=0xd8;
    *(volatile uint64_t *)(GICD_BASE+0x6000+8*143)=0;__asm__ volatile("dsb sy":::"memory");
    s->active=1;
}
void i2s_probe_irq_enable(void)
{
    /* GICD.RWP completes an earlier disable. The architecture guarantees
     * a subsequent ISENABLER read observes the new enable write. */
    wait_mask(GICD_BASE,BIT(31),0,0x47);
    wr(GICD_BASE+0x100+4*(143/32),BIT(143%32));
    same(GICD_BASE+0x100+4*(143/32),BIT(143%32),BIT(143%32),0x47);
}
void i2s_probe_irq_disable(void)
{
    wr(GICD_BASE+0x180+4*(143/32),BIT(143%32));wait_mask(GICD_BASE,BIT(31),0,0x48);
    same(GICD_BASE+0x100+4*(143/32),BIT(143%32),0,0x48);
}
uint64_t sunstone_probe_restore_interrupts(void)
{
    struct i2s_probe_saved *s=&i2s_probe_saved;
    if(s->active){
        i2s_probe_irq_disable();
        wait_mask(DMA+0x30,0x8000ffff,0,0x49); /* Includes global MBUS FIFO empty. */
        for(unsigned ch=0;ch<2;++ch){
            uintptr_t a=DMA+0x100+64*ch;
            if(rd(a)||rd(a+0x34)||rd(a+0x38))fault(0x4a,rd(a));
        }
        wr(DMA+0x38,s->mcu&~3u);wr(DMA+0x34,s->cpu);wr(DMA+0x38,s->mcu);
        wr(DMA+0x28,s->automatic);wr(DMA+0x20,s->security);
        same(DMA+0x34,0xffff,s->cpu,0x4d);same(DMA+0x38,0xffff,s->mcu,0x4d);
        same(DMA+0x28,7,s->automatic,0x4d);same(DMA+0x20,0xffff,s->security,0x4d);
        unsigned bank=143/32,bit=143%32;
        wr(GICD_BASE+0x280+4*bank,BIT(bit));
        wr(GICD_BASE+0x80+4*bank,s->group);wr(GICD_BASE+0xd00+4*bank,s->modifier);
        wr(GICD_BASE+0xc00+4*(143/16),s->configuration);
        *(volatile uint8_t *)(GICD_BASE+0x400+143)=s->priority;
        *(volatile uint64_t *)(GICD_BASE+0x6000+8*143)=s->route;
        __asm__ volatile("dsb sy":::"memory");
        wait_mask(GICD_BASE,BIT(31),0,0x4e);
        same(GICD_BASE+0x80+4*bank,UINT32_MAX,s->group,0x4e);
        same(GICD_BASE+0xd00+4*bank,UINT32_MAX,s->modifier,0x4e);
        same(GICD_BASE+0xc00+4*(143/16),UINT32_MAX,s->configuration,0x4e);
        same(GICD_BASE+0x200+4*bank,BIT(bit),0,0x4e);
        if(*(volatile uint8_t *)(GICD_BASE+0x400+143)!=s->priority||
           *(volatile uint64_t *)(GICD_BASE+0x6000+8*143)!=s->route)fault(0x4e,143);
        /* Close all consumers first. Manual PLL Disable ordering is output
         * gate, lock detector, PLL, LDO. LOCK is undefined after detector off. */
        uint32_t current=rd(CCU+0x280);
        if((current^s->audio)&~BIT(28)){
            /* An adopted active PLL was never retuned. An unexpected change
             * cannot be repaired by exposing an unlocked restart. */
            if(s->audio&BIT(31))fault(0x4b,current);
            wr(CCU+0x280,current&~BIT(27));current=rd(CCU+0x280);
            wr(CCU+0x280,current&~BIT(29));current=rd(CCU+0x280);
            wr(CCU+0x280,current&~BIT(31));current=rd(CCU+0x280);
            if(!(s->audio&BIT(30)))wr(CCU+0x280,current&~BIT(30));
            wr(CCU+0x280,s->audio&~BIT(28));
            if((rd(CCU+0x280)^s->audio)&~BIT(28))fault(0x4b,rd(CCU+0x280));
        }
        wr(CCU+0x191c,s->audio_gate);
        /* Forced old gates have defined effective state; Auto policy has
         * demand-dependent status and cannot require the saved observation. */
        uint32_t forced=(s->audio_gate&6)<<16;
        if(forced)wait_mask(CCU+0x199c,forced,s->audio_gate&forced,0x4c);
        same(CCU+0x191c,0x60006,s->audio_gate,0x4c);
        wr(CCU+0x704,s->bus);wr(CCU+0x5e4,s->mbus);
        same(CCU+0x704,0x10001,s->bus,0x4f);same(CCU+0x5e4,1,s->mbus,0x4f);
        s->active=0;
    }
    return i2s_probe_monitor_restore();
}
