/* Success-only shutdown for the finite current-port workload. All state and
 * cleanup stacks below are Secure Device memory, including while EL3 uses
 * the current Nonsecure alias for image-RAM cache maintenance. */
#include "runtime.h"
#include "cached.h"
#include "FreeRTOS.h"
#include "task.h"
#include "a733_fault.h"
#define DEVICE __attribute__((section(".bss.a733_smp_monitor"),aligned(128)))
#define GICD UINT32_C(0x03400000)
#define MAGIC UINT64_C(0x3154525133333741)
volatile struct qualification_report a733_qualification_report DEVICE;
unsigned char a733_qualification_stacks[QUAL_CORES][QUAL_STACK_BYTES]
    __attribute__((section(".bss.a733_smp_monitor"),aligned(4096)));
/* EL0 workers publish before their final SVC. This range is part of the final
 * cached writable clean; nothing writes it after capture-complete. */
static uint64_t arrived[QUAL_CORES];
static inline void barrier(void){__asm__ volatile("dsb sy":::"memory");}
static uint32_t rd(uintptr_t p){uint32_t v=*(volatile uint32_t *)p;barrier();return v;}
static void wr(uintptr_t p,uint32_t v){*(volatile uint32_t *)p=v;barrier();}
static int before(void){return a733_smp_counter()<a733_qualification_report.deadline;}
static _Noreturn void retained(unsigned core,uint64_t reason)
{
    if(core<8)a733_qualification_report.cpu[core].error=reason;
    a733_qualification_report.error=reason;barrier();
    for(;;)__asm__ volatile("wfe");
}
static int wait_word(uintptr_t p,uint32_t mask,uint32_t expected)
{
    uint64_t previous=a733_smp_counter();
    for(;;){uint64_t now=a733_smp_counter();
        if(now<previous || now>=a733_qualification_report.deadline || a733_qualification_report.error)return 0;
        previous=now;if((rd(p)&mask)==expected)return 1;}
}
int a733_qualification_boot(uint64_t epoch,uint64_t deadline)
{
    volatile struct qualification_report *r=&a733_qualification_report;
    if(!epoch || a733_smp_counter()>=deadline || r->magic)return 0;
    r->magic=MAGIC;r->version=1;r->bytes=sizeof(*r);r->epoch=epoch;r->deadline=deadline;
    uint32_t ctlr=rd(GICD),banks=(rd(GICD+4)&31)+1;
    if((ctlr&0x80000070)!=0x30)return 0;
    r->distributor=ctlr;r->banks=banks;
    for(unsigned i=1;i<banks;++i)r->enable[i]=rd(GICD+0x100+i*4);
    r->entry_low=rd(0x08001004);r->entry_high=rd(0x08001008);r->rom_entry=0x20;
    for(unsigned c=1;c<8;++c){
        uintptr_t cfg=0x08001000+c*0x1000,control=0x07050200+c*4;
        if((rd(0x07052008+c*0x1000)&15) || ((rd(control)|rd(control+0x20))&1))return 0;
        r->power[c][0]=rd(cfg);r->power[c][1]=rd(cfg+4);r->power[c][2]=rd(cfg+8);
        r->power[c][3]=rd(control);r->power[c][4]=rd(control+0x20);
    }
    a733_qualification_system_save((uint64_t *)(uintptr_t)r->cpu[0].system);
    r->cpu[0].reserved=1;
    r->state=1;barrier();return 1;
}
int a733_qualification_local(unsigned core,uint64_t epoch)
{
    if(core>=8 || epoch!=a733_qualification_report.epoch || !before())return 0;
    volatile struct qualification_core *c=&a733_qualification_report.cpu[core];
    if(c->epoch)return 0;
    uintptr_t r=0x03460000+core*0x20000,g=r+0x10000;
    uint32_t power=rd(r+0x24);
    if((power&1) || ((power>>3)&1)!=((power>>2)&1) || rd(r))return 0;
    const uintptr_t offsets[]={0x14,0x24,0x10080,0x10d00,0x10c04,0x10400,0x1041c,0x10100,0x10200,0x10300};
    for(unsigned i=0;i<10;++i)c->local[i]=rd(r+offsets[i]);
    if(c->local[0]!=(core?6u:0u) || c->local[7] || c->local[8] || c->local[9])return 0;
    /* The current controller also discovers affinity; reject an unexpected
     * frame before saving system state or modifying the interrupt interface. */
    uint64_t typer=*(volatile uint64_t *)(r+8);barrier();
    if((typer>>32)!=(uint64_t)core<<8 || (typer&2) || !!(typer&16)!=(core==7))return 0;
    (void)g;
    if(core)a733_qualification_system_save((uint64_t *)(uintptr_t)c->system);
    /* Returning cleanup cannot restore a live interrupt/timer producer or
     * a pre-existing translated regime after its owned workload stops. */
    if((c->system[0]|c->system[12]|c->system[22]|c->system[31])&UINT64_C(0x1005) ||
       (c->system[9]&1) || (c->system[10]&3) ||
       (c->system[46]&1) || (c->system[48]&1))return 0;
    c->epoch=epoch;barrier();return 1;
}
_Noreturn void a733_qualification_worker_done(unsigned core)
{
    configASSERT(core<8 && portGET_CORE_ID()==core);
    uint64_t epoch=a733_cached_runtime[core].epoch;
    uint64_t deadline=a733_cached_runtime[core].diagnostic_deadline;
    while(!__atomic_load_n(&a733_smp_core_results[core].background_progress,__ATOMIC_ACQUIRE)){
        configASSERT(a733_smp_counter()<deadline);vTaskDelay(1);
    }
    configASSERT(a733_smp_core_results[core].task_rounds==32 && a733_smp_core_results[core].sgis &&
        !a733_smp_core_results[core].failure && (core || a733_smp_core_results[0].ticks>=32));
    __atomic_store_n(&arrived[core],epoch,__ATOMIC_RELEASE);
    for(unsigned i=0;i<8;++i)while(__atomic_load_n(&arrived[i],__ATOMIC_ACQUIRE)!=epoch)
        configASSERT(a733_smp_counter()<deadline);
    a733_qualification_terminal_el0(core);
}
/* Called only after the provenance-checked success SMC executed the current
 * recorder. The assembly entry has selected this core's Device cleanup stack. */
_Noreturn void a733_qualification_captured(unsigned core)
{
    volatile struct qualification_report *r=&a733_qualification_report;
    if(core>=8 || r->state!=1 || !before())retained(core,1);
    volatile struct qualification_core *c=&r->cpu[core];
    if(c->epoch!=r->epoch || c->captured || arrived[core]!=r->epoch)retained(core,2);
    const volatile struct a733_fault_record *fault=(const void *)(uintptr_t)(A733_FAULT_SLOTS_ADDRESS+core*128);
    if(fault->magic!=A733_FAULT_SLOT_MAGIC || fault->info!=A733_FAULT_INFO(A733_FAULT_KIND_EXIT,0,core) ||
       fault->status || fault->detail || fault->epoch!=r->epoch)retained(core,3);
    const struct a733_smp_core_result *v=&a733_smp_core_results[core];
    if(v->task_rounds!=32 || !v->background_progress || !v->sgis || v->failure || (!core && v->ticks<32))retained(core,4);
    c->rounds=v->task_rounds;c->background=v->background_progress;c->sgis=v->sgis;c->failure=v->failure;c->ticks=v->ticks;
    barrier();c->captured=r->epoch;barrier();
    for(unsigned i=0;i<8;++i)while(r->cpu[i].captured!=r->epoch){if(!before() || r->error)retained(core,5);barrier();}
    uintptr_t base=0x03460000+core*0x20000,g=base+0x10000;
    wr(g+0x180,UINT32_C(0x40000001));
    if(!wait_word(base,8,0) || rd(g+0x300) || (rd(g+0x200)&~UINT32_C(0x40000001)))retained(core,6);
    wr(g+0x280,UINT32_C(0x40000001));
    if(rd(g+0x200) || rd(g+0x100))retained(core,7);
    c->drained_epoch=r->epoch;barrier();
    for(unsigned i=0;i<8;++i)while(r->cpu[i].drained_epoch!=r->epoch){
        if(!before() || r->error)retained(core,8);barrier();
    }
    /* Implemented in assembly: validate current NS translations and clean
     * with no further Normal-memory stores, then restore permitted controls. */
    a733_qualification_system_restore(core,(const uint64_t *)(uintptr_t)c->system);
}
/* The assembly caller has cleaned image RAM and still uses the dedicated
 * Device stack. This routine stores only to Device reports and MMIO. */
int a733_qualification_restore_local(unsigned core)
{
    if(core>=8 || !before() || a733_qualification_report.error)return 0;
    volatile struct qualification_core *c=&a733_qualification_report.cpu[core];
    uintptr_t r=0x03460000+core*0x20000,g=r+0x10000;
    uint64_t group0,group1;
    __asm__ volatile("mrs %0,icc_igrpen0_el1\n mrs %1,icc_igrpen1_el3":"=r"(group0),"=r"(group1));
    if(group0&1 || group1&3 || rd(g+0x100) || rd(g+0x200) || rd(g+0x300))return 0;
    const uintptr_t offsets[]={0x80,0xd00,0xc04,0x400,0x41c};
    for(unsigned i=0;i<5;++i){wr(g+offsets[i],(uint32_t)c->local[2+i]);
        if(rd(g+offsets[i])!=c->local[2+i])return 0;}
    if(core){wr(r+0x14,2);if(!wait_word(r+0x14,6,6))return 0;}
    if(rd(r) || rd(r+0x14)!=c->local[0] || rd(r+0x24)!=c->local[1])return 0;
    return 1;
}
_Noreturn void a733_qualification_off(unsigned core)
{
    volatile struct qualification_report *r=&a733_qualification_report;
    if(core>=8 || !before() || r->error)retained(core,20);
    if(r->cpu[core].captured!=r->epoch || r->cpu[core].cleaned!=r->epoch)retained(core,31);
    r->cpu[core].restored=r->epoch;barrier();
    if(core){
        while(r->cpu[core].request!=r->epoch){if(!before() || r->error)retained(core,21);barrier();}
        extern _Noreturn void a733_qualification_power_down(unsigned,uint64_t);
        a733_qualification_power_down(core,r->epoch);
    }
    for(unsigned c=1;c<8;++c){
        while(r->cpu[c].restored!=r->epoch){if(!before() || r->error)retained(core,22);barrier();}
        r->cpu[c].request=r->epoch;barrier();
        while(r->cpu[c].acknowledged!=r->epoch){if(!before() || r->error)retained(core,23);barrier();}
        if(!wait_word(0x07052008+c*0x1000,15,0))retained(core,24);
        r->cpu[c].off_observed=r->epoch;barrier();
        uintptr_t cfg=0x08001000+c*0x1000,control=0x07050200+c*4;
        const uintptr_t address[]={cfg,cfg+4,cfg+8,control,control+0x20};
        for(unsigned i=0;i<5;++i){wr(address[i],(uint32_t)r->power[c][i]);
            if(rd(address[i])!=r->power[c][i])retained(core,25);}
    }
    if(!wait_word(GICD,UINT32_C(0x80000000),0))retained(core,26);
    wr(GICD,(uint32_t)r->distributor&~7u);
    if(!wait_word(GICD,UINT32_C(0x80000000),0))retained(core,27);
    for(unsigned i=1;i<r->banks;++i)wr(GICD+0x180+4*i,UINT32_MAX);
    if(!wait_word(GICD,UINT32_C(0x80000000),0))retained(core,28);
    for(unsigned i=1;i<r->banks;++i){wr(GICD+0x100+4*i,(uint32_t)r->enable[i]);
        if(rd(GICD+0x100+4*i)!=r->enable[i])retained(core,29);}
    /* Entry requires both ARE bits; this never performs a forbidden 1->0. */
    wr(GICD,(uint32_t)r->distributor);
    if(!wait_word(GICD,UINT32_C(0x80000000),0) || rd(GICD)!=r->distributor)retained(core,30);
    r->cpu[0].off_observed=0;
    r->state=4;barrier();
    a733_qualification_rom();
}
