#include "return.h"
#include "../gpu-render/application.h"
#include "../gpu-render/session.h"
#include "a733_storage_digest.h"
#include <string.h>

extern const struct a733_smp_contract *sd_single_core_contract(void);
extern unsigned char __gpu_private_start[],__gpu_private_end[];
extern unsigned char __tables_start[],__tables_end[];
/* File-backed immutable metadata for the host; never inferred from its headers. */
const uint64_t gpu_return_wb_geometry[2] __attribute__((used,retain)) = {
    GPU_RETURN_WB_START,GPU_RETURN_WB_END
};
static uint64_t last_tick __attribute__((section(".monitor_state")));

#define REPORT ((volatile struct a733_smp_result *)A733_SMP_RESULT_ADDRESS)
static _Noreturn void fail(unsigned reason)
{a733_smp_stop(0x100u+reason,reason);}
static int bounded(uint64_t deadline)
{
    uint64_t now=counter();
    if(now<last_tick||now>=deadline)return 0;
    last_tick=now;return 1;
}
static int private_pointer(const void *p,size_t bytes)
{
    uintptr_t start=(uintptr_t)p,lo=(uintptr_t)__gpu_private_start,hi=(uintptr_t)__gpu_private_end;
    return bytes&&start>=lo&&start<hi&&bytes<=hi-start;
}
static int parent(const struct device_memory_pool *p,const struct device_memory_lease *lease,
    const struct device_memory_extent *extent,uint64_t owner,uintptr_t start,unsigned pages,unsigned *slots)
{
    if(lease->pool!=(uintptr_t)p||lease->owner!=owner||!lease->generation||
       lease->slot>=DEVICE_MEMORY_MAX_ALLOCATIONS||(*slots&(1u<<lease->slot))||
       extent->cpu!=start||extent->physical!=start||extent->bytes!=(size_t)pages*4096)return 0;
    const struct device_memory_allocation *a=&p->allocations[lease->slot];
    if(a->state!=DEVICE_MEMORY_DEVICE||a->owner!=owner||a->generation!=lease->generation||
       a->first!=(start-p->backing.cpu)/4096||a->pages!=pages||p->next_generation<=a->generation)return 0;
    *slots|=1u<<lease->slot;return 1;
}
static int pool(const struct device_memory_pool *p,uintptr_t start,size_t bytes)
{
    return private_pointer(p,sizeof(*p))&&p->initialized==1&&p->backing.cpu==start&&
        p->backing.physical==start&&p->backing.bytes==bytes;
}
int gpu_return_allocations(void)
{
    const struct a733_gpu_reserved *f=&app.firmware;
    const struct a733_gpu_compute_reserved *c=&app.compute;
    const struct device_memory_pool *wb=f->wb_pool,*nc=f->nc_pool;
    if(wb==nc||!pool(wb,GPU_RETURN_WB_START,0x30000000)||!pool(nc,0x78000000,0x01000000)||
       c->binding.firmware!=f||c->binding.session!=c||c->binding.service!=&app.service||
       c->binding.wb!=wb||c->binding.nc!=nc||c->binding.owner!=10)return 0;
    unsigned ws=0,ns=0;
    if(!parent(wb,&f->wb_lease,&f->wb_extent,9,GPU_RETURN_WB_START,34,&ws)||
       !parent(wb,&c->payload_lease,&c->payload_extent,10,GPU_RETURN_PAYLOAD_START,88,&ws)||
       !parent(wb,&c->table_lease,&c->table_extent,10,GPU_RETURN_TABLE_START,12,&ws)||
       !parent(nc,&f->nc_lease,&f->nc_extent,9,0x78000000,45,&ns)||
       !parent(nc,&c->firmware_lease,&c->firmware_extent,10,0x7802d000,6,&ns))return 0;
    for(unsigned i=0;i<DEVICE_MEMORY_MAX_ALLOCATIONS;++i){
        if(!(ws&(1u<<i))&&wb->allocations[i].state!=DEVICE_MEMORY_FREE)return 0;
        if(!(ns&(1u<<i))&&nc->allocations[i].state!=DEVICE_MEMORY_FREE)return 0;
    }
    return 1;
}
static int state(const struct gpu_boot_contract *contract)
{
    const struct a733_smp_contract *c=&contract->base;
    const struct a733_board_session *sealed=gpu_application_session();
    if(!sealed||!gpu_finite_return_ready()||REPORT->magic!=A733_SMP_RESULT_MAGIC||
       REPORT->epoch!=c->epoch||REPORT->generation!=c->generation||
       memcmp(c->build_id,a733_smp_build_id,32)||memcmp(sd_single_core_contract(),c,sizeof(*c)))return 0;
    uint8_t digest[32];storage_digest_bytes(sealed,sizeof(*sealed),digest);
    if(memcmp(digest,contract->session_digest,32))return 0;
    struct a733_board_session expected=*sealed;
    expected.session_start_ticks=c->startup_deadline-A733_SMP_COUNTER_HZ;
    expected.deadline_us=c->diagnostic_deadline/24;
    if(memcmp(&app.session,&expected,sizeof(expected))||app.compute_sequence[0]!=1||
       app.compute_sequence[1]!=2||!gpu_return_allocations())return 0;
    return gpu_return_device_quiet();
}
int gpu_return_device_quiet(void)
{
    __asm__ volatile("dsb sy":::"memory");
    return !(mmio_read(0x02002b20)&0x80000000)&&!(mmio_read(0x02002b24)&0x10001)&&
        !(mmio_read(0x07065000)&15)&&!(mmio_read(0x07065008)&15)&&
        mmio_read(0x07066000)==0&&mmio_read(0x07066008)==0&&
        !(mmio_read(GICD_BASE)&0x80000000)&&
        !((mmio_read(GICD_BASE+0x108)|mmio_read(GICD_BASE+0x208)|mmio_read(GICD_BASE+0x308))&0x80000000);
}
static int wait_clear(uintptr_t address,uint32_t bits,uint64_t deadline)
{while(mmio_read(address)&bits)if(!bounded(deadline))return 0;return bounded(deadline);}
static int restore(const struct gpu_return_gic *s,uint64_t deadline)
{
    if(s->ready!=1||s->banks<3||s->banks>32||(s->ctlr&0x30)!=0x30)return 0;
    mmio_write(GICD_BASE,mmio_read(GICD_BASE)&~7u);
    if(!wait_clear(GICD_BASE,1u<<31,deadline))return 0;
    for(unsigned i=1;i<s->banks;++i)mmio_write(GICD_BASE+0x180+4*i,UINT32_MAX);
    if(!wait_clear(GICD_BASE,1u<<31,deadline))return 0;
    mmio_write(GICR_SGI+0x180,UINT32_MAX);
    if(!wait_clear(GICR_BASE,8,deadline))return 0;
    mmio_write(GICR_SGI+0x280,(1u<<TICK_IRQ)|(1u<<RECOVERY_IRQ));
    mmio_write(GICR_SGI+0x380,(1u<<TICK_IRQ)|(1u<<RECOVERY_IRQ));
    mmio_write(GICR_SGI+0x80,s->group);mmio_write(GICR_SGI+0xd00,s->modifier);
    mmio_write(GICR_SGI+0xc04,s->configuration);mmio_write(GICR_SGI+0x41c,s->priorities);
    mmio_write(GICD_BASE+0x88,(mmio_read(GICD_BASE+0x88)&0x7fffffff)|(s->gpu_group&0x80000000));
    mmio_write(GICD_BASE+0xd08,(mmio_read(GICD_BASE+0xd08)&0x7fffffff)|(s->gpu_modifier&0x80000000));
    mmio_write(GICD_BASE+0xc14,(mmio_read(GICD_BASE+0xc14)&0x3fffffff)|(s->gpu_configuration&0xc0000000));
    mmio_write(GICD_BASE+0x45c,(mmio_read(GICD_BASE+0x45c)&0x00ffffff)|(s->gpu_priority&0xff000000));
    *(volatile uint64_t *)(GICD_BASE+0x62f8)=s->gpu_route;
    mmio_write(GICR_SGI+0x100,s->enabled);
    if(!wait_clear(GICR_BASE,8,deadline))return 0;
    for(unsigned i=1;i<s->banks;++i)mmio_write(GICD_BASE+0x100+4*i,s->spi_enabled[i]);
    if(!wait_clear(GICD_BASE,1u<<31,deadline))return 0;
    mmio_write(GICD_BASE,s->ctlr);
    __asm__ volatile("dsb sy":::"memory");return wait_clear(GICD_BASE,1u<<31,deadline);
}
_Noreturn void gpu_return_finish(const struct gpu_boot_contract *c,const struct gpu_return_gic *s)
{
    uint64_t el,control,flags,scr,ctr,caller,el1;
    __asm__ volatile("msr daifset,#15\n msr cntps_ctl_el1,xzr\n msr cntp_ctl_el0,xzr\n"
        "msr cntv_ctl_el0,xzr\n msr ICC_IGRPEN0_EL1,xzr\n msr ICC_IGRPEN1_EL3,xzr\n isb\n"
        "mrs %0,CurrentEL\n mrs %1,sctlr_el3\n mrs %2,daif\n mrs %3,scr_el3\n mrs %4,ctr_el0\n mrs %5,spsr_el3\n mrs %6,sctlr_el1"
        :"=r"(el),"=r"(control),"=r"(flags),"=r"(scr),"=r"(ctr),"=r"(caller),"=r"(el1)::"memory");
    if((el1&0x1005)||el!=12||(control&0x02001005)||(flags&0x3c0)!=0x3c0||!(scr&1)||
       ((ctr>>16)&15)!=4||((ctr>>24)&15)!=4||(caller&15)!=4||!(caller&0x80))fail(1);
    uint64_t start=counter(),deadline=start+2*A733_SMP_COUNTER_HZ;
    if(deadline<start||start>=c->base.diagnostic_deadline)fail(2);
    if(deadline>c->base.diagnostic_deadline)deadline=c->base.diagnostic_deadline;
    last_tick=start;
    const struct gpu_return_receipt *receipt=&gpu_return_receipt;
    uintptr_t private_start=(uintptr_t)__gpu_private_start;
    uintptr_t private_end=((uintptr_t)__gpu_private_end+63)&~(uintptr_t)63;
    if(receipt->magic!=GPU_RETURN_RECEIPT_MAGIC||receipt->epoch!=c->base.epoch||
       receipt->generation!=c->base.generation||receipt->lease!=app.session.lease||
       receipt->status||receipt->complete!=1||receipt->clean_start!=private_start||
       receipt->clean_end!=private_end||receipt->wb_start!=GPU_RETURN_WB_START||receipt->wb_end!=GPU_RETURN_WB_END||
       receipt->clean_lines!=(private_end-private_start+GPU_RETURN_WB_BYTES)/64||
       receipt->deadline<=start||receipt->deadline>c->base.diagnostic_deadline||
       receipt->log_length!=RESULT->log_length)fail(3);
    if(deadline>receipt->deadline)deadline=receipt->deadline;
    if(!state(c)||!bounded(deadline))fail(4);
    if(!restore(s,deadline))fail(5);
    /* Only published Nonsecure bytes have been consumed. EL3 VA maintenance
     * cannot publish Nonsecure EL1 cache lines and is deliberately absent. */
    volatile struct gpu_return_result *out=(void *)GPU_RETURN_RESULT_ADDRESS;
    out->magic=0;out->version=1;out->bytes=sizeof(*out);out->epoch=c->base.epoch;out->generation=c->base.generation;
    out->sequence[0]=app.compute_sequence[0];out->sequence[1]=app.compute_sequence[1];out->word_count=64;out->clean_lines=receipt->clean_lines;
    for(unsigned n=0;n<2;++n)for(unsigned i=0;i<32;++i)out->words[n*32+i]=app.compute_words[n][i];
    out->cleanup_start=start;
    if(!bounded(deadline))fail(7);
    out->cleanup_end=last_tick;
    RESULT->status=0;RESULT->end_counter=last_tick;RESULT->stage=5;
    REPORT->status=0;REPORT->detail=0;REPORT->end_counter=last_tick;REPORT->completed_mask=1;
    __asm__ volatile("dsb sy":::"memory");out->magic=GPU_RETURN_MAGIC;
    __asm__ volatile("dsb sy":::"memory");REPORT->stage=5;
    __asm__ volatile("dsb sy":::"memory");gpu_return_reset();
}
