/* Observe inherited state. This image never enables, stops or resets DMA. */
#include "dma-authority-control.h"
#include "a733_dma_regs.h"
#include "sunstone_probe.h"
#include "boot.h"

#define GIC UINT32_C(0x03400000)
#define DMA UINT32_C(0x04601000)
#define DMA_RESULT ((struct dma_authority_result *)(uintptr_t)0x42000100)
#define IRQ_WORD (4u*(143u/32u))
SUNSTONE_PROBE_IMAGE(0x42000100,sizeof(struct dma_authority_result),0,0,20);
uint64_t dma_authority_saved_vbar;
static uint32_t saved_gic[4];
static unsigned collected;
extern void invalidate_data_cache(void);

static uint32_t read32(uintptr_t address)
{
    uint32_t value=*(volatile uint32_t *)address;
    __asm__ volatile("dsb sy":::"memory");
    return value;
}
static uint32_t distributor_ready(void)
{
    const uint64_t deadline=counter()+2400; /* 100 microseconds at 24 MHz. */
    uint32_t control;
    /* GICD_CTLR.RWP clears before reading the completed enable state. */
    while((control=read32(GIC))&(UINT32_C(1)<<31))
        if(counter()>=deadline)sunstone_probe_fault(0x31,control,
            (uintptr_t)__builtin_return_address(0),GIC);
    return control;
}
void dma_authority_collect(struct dma_authority_sample *s)
{
    s->counter=counter();
    s->bus=read32(0x02002704);s->mbus=read32(0x020025e4);
    s->e902_clock=read32(0x07010210);s->e902_bus=read32(0x0701021c);
    s->cpuidle=read32(0x07050008);
    s->gic_control=distributor_ready();
    s->gic_enable=read32(GIC+0x100+IRQ_WORD);
    s->gic_group=read32(GIC+0x80+IRQ_WORD);
    s->gic_modifier=read32(GIC+0xd00+IRQ_WORD);
    s->gic_pending=read32(GIC+0x200+IRQ_WORD);
    s->gic_active=read32(GIC+0x300+IRQ_WORD);
    s->aperture_sampled=0;
    s->autogate=s->busy=s->cpu_route=s->mcu_route=UINT32_MAX;
    for(unsigned c=0;c<DMA_AUTHORITY_CHANNELS;++c)
        for(unsigned r=0;r<DMA_AUTHORITY_WORDS;++r)s->channel[c][r]=UINT32_MAX;
    /* A gated/reset controller is not accessed. E902's bus and start state
     * must also show exclusion; its gated configuration aperture is untouched. */
    if((s->bus&0x10001)!=0x10001||!(s->mbus&1)||
       (s->e902_bus&0x10003)||(s->e902_clock&(1u<<31))||(s->cpuidle&1))return;
    s->autogate=read32(DMA+A733_DMA_REG_AUTO_GATE);
    s->busy=read32(DMA+A733_DMA_REG_BUSY);
    s->cpu_route=read32(DMA+A733_DMA_REG_CPU_ROUTE);
    s->mcu_route=read32(DMA+A733_DMA_REG_MCU_ROUTE);
    static const unsigned offsets[]={A733_DMA_QUIET_REGISTERS};
    _Static_assert(sizeof(offsets)/sizeof(*offsets)==DMA_AUTHORITY_WORDS,"snapshot geometry");
    for(unsigned c=0;c<DMA_AUTHORITY_CHANNELS;++c)
        for(unsigned r=0;r<DMA_AUTHORITY_WORDS;++r)
            s->channel[c][r]=read32(DMA+A733_DMA_REG_CHANNELS+
                c*A733_DMA_REG_CHANNEL_STRIDE+offsets[r]);
    s->aperture_sampled=1;
}
uint64_t sunstone_probe_quiesce(void)
{
    /* No worker, DMA transfer or interrupt source is started by this image. */
    return collected==DMA_AUTHORITY_SAMPLES&&DMA_RESULT->device_writes==0?
        SUNSTONE_PROBE_READY:0;
}
uint64_t sunstone_probe_restore_interrupts(void)
{
    /* GIC configuration is read-only here. Verify it against the actual entry
     * snapshot; pending inputs may change independently and are not compared. */
    uint32_t control=distributor_ready();
    if((control&~(1u<<31))!=saved_gic[0]||read32(GIC+0x100+IRQ_WORD)!=saved_gic[1]||
       read32(GIC+0x80+IRQ_WORD)!=saved_gic[2]||
       read32(GIC+0xd00+IRQ_WORD)!=saved_gic[3])return 0;
    __asm__ volatile("msr vbar_el3,%0\n isb"::"r"(dma_authority_saved_vbar):"memory");
    return SUNSTONE_PROBE_READY;
}
_Noreturn void dma_authority_main(void)
{
    sunstone_probe_begin();
    /* Match the common monitor: discard inherited aliases after admission,
     * before uncached result writes can later meet whole-cache cleanup. */
    invalidate_data_cache();
    DMA_RESULT->magic=DMA_AUTHORITY_MAGIC;DMA_RESULT->version=1;DMA_RESULT->bytes=sizeof(*DMA_RESULT);
    DMA_RESULT->frequency=COUNTER_HZ;DMA_RESULT->samples=0;DMA_RESULT->device_writes=0;
    uint64_t scr,el1;
    __asm__ volatile("mrs %0,scr_el3\n mrs %1,sctlr_el1":"=r"(scr),"=r"(el1));
    DMA_RESULT->scr_el3=scr;DMA_RESULT->sctlr_el1=el1;
    for(unsigned n=0;n<DMA_AUTHORITY_SAMPLES;++n){
        dma_authority_collect(&DMA_RESULT->sample[n]);
        if(!n){
            const struct dma_authority_sample *s=&DMA_RESULT->sample[0];
            saved_gic[0]=s->gic_control&~(1u<<31);saved_gic[1]=s->gic_enable;
            saved_gic[2]=s->gic_group;saved_gic[3]=s->gic_modifier;
        }
        collected=n+1;DMA_RESULT->samples=collected;sunstone_probe_progress_el3();
        const uint64_t until=counter()+2400;
        while(counter()<until){}
    }
    sunstone_probe_complete(0,sizeof(*DMA_RESULT));
}
