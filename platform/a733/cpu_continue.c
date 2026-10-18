#include "a733_cpu_continue.h"

enum { DISTRIBUTOR, SCAN, CONTROL, MODE, WAKE_ENABLE, WAKE_CHECK,
    R_CTLR, WAKER, WAKE_CLEAR, WAKE_WAIT, DISABLE, DISABLE_WAIT,
    CLEAR_PENDING, CLEAR_ACTIVE, ACTIVE_CHECK, GROUP, MODIFIER, CONFIG,
    CONFIG_WRITE, PRIORITY, PRIORITY_WRITE, ENABLE, ENABLE_WAIT,
    VERIFY_GROUP, VERIFY_MODIFIER, VERIFY_ENABLE, VERIFY_PRIORITY,
    POWER_GROUP_WAIT, POWER_REQUEST, POWER_VERIFY };
enum { ERROR_IO=1, ERROR_TIME, ERROR_INITIAL, ERROR_VERIFY };
static int separate(const void *a,size_t an,const void *b,size_t bn)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return a && b && an<=UINTPTR_MAX-x && bn<=UINTPTR_MAX-y && (x+an<=y || y+bn<=x);
}
int a733_cpu_sgi1r(unsigned core,uint64_t *out)
{if(!out || core>=8)return 0;*out=((uint64_t)core<<16)|1;return 1;}
int a733_cpu_continue_tables(uint64_t a[512],uint64_t b[512],uint64_t pa)
{
    if(!separate(a,4096,b,4096) || ((uintptr_t)a&4095) || ((uintptr_t)b&4095) ||
       (pa&4095) || pa<UINT32_C(0x42000000) || pa>=UINT32_C(0x42200000))return 0;
    for(unsigned i=0;i<512;++i)a[i]=b[i]=0;
    a[0]=(UINT64_C(1)<<54)|(UINT64_C(1)<<53)|(1u<<10)|1;
    a[1]=pa|3;
    for(unsigned i=0;i<64;++i) {
        uint64_t address=UINT64_C(0x40000000)+((uint64_t)i<<21);
        uint64_t attrs=(1u<<10)|(3u<<8)|(1u<<2)|1;
        if(address==UINT64_C(0x41000000))attrs|=3u<<6;
        else {
            attrs|=(UINT64_C(1)<<54)|(UINT64_C(1)<<53);
            if(address!=UINT64_C(0x42000000))attrs|=1u<<6;
        }
        b[i]=address|attrs;
    }
    return 1;
}
int a733_cpu_continue_init(struct a733_cpu_continue *s,const struct a733_cpu_continue_config *c)
{
    unsigned core;
    if(!separate(s,sizeof(*s),c,sizeof(*c)) || s->initialized || !c->epoch || !c->deadline_us ||
       !c->ops.read32 || !c->ops.read64 || !c->ops.write32 || !c->ops.now_us ||
       !a733_cpu_index(c->mpidr,&core))return 0;
    *s=(struct a733_cpu_continue){.config=*c,.core=core,.state=A733_CONTINUE_PENDING,.initialized=1};return 1;
}
static void fail(struct a733_cpu_continue *s,unsigned error)
{s->state=A733_CONTINUE_RETAINED;s->error=error;}
static int time_ok(struct a733_cpu_continue *s)
{
    uint64_t now;
    if(s->config.ops.now_us(s->config.context,&now)!=1 || now<s->last_now || now>=s->config.deadline_us) {
        fail(s,ERROR_TIME);return 0;
    }
    s->last_now=now;return 1;
}
static int read32(struct a733_cpu_continue *s,uintptr_t a,uint32_t *out)
{if(s->config.ops.read32(s->config.context,a,out)==1)return 1;fail(s,ERROR_IO);return 0;}
static int write32(struct a733_cpu_continue *s,uintptr_t a,uint32_t v)
{++s->writes_attempted;if(s->config.ops.write32(s->config.context,a,v)==1)return 1;fail(s,ERROR_IO);return 0;}
unsigned a733_cpu_continue_poll(struct a733_cpu_continue *s)
{
    if(!s || !s->initialized)return A733_CONTINUE_RETAINED;
    if(s->entered || s->state!=A733_CONTINUE_PENDING)return s->state;
    s->entered=1;if(!time_ok(s))goto out;
    uintptr_t r=s->redistributor,g=r+0x10000,control=0x07050200+s->core*4;
    uint32_t v=0;int advance=1;
    switch(s->phase) {
    case DISTRIBUTOR:
        if(read32(s,0x03400000,&v) && ((v&0x73)!=0x33 || (v&(1u<<31))))fail(s,ERROR_INITIAL);
        break;
    case SCAN: {
        uint64_t typer=0;
        if(s->config.ops.read64(s->config.context,0x03460008+s->frame*0x20000,&typer)!=1) {fail(s,ERROR_IO);break;}
        uint32_t affinity=(uint32_t)(typer>>32);unsigned index=(affinity>>8)&255;
        if((typer&2) || (affinity&~UINT32_C(0x700)) || index>=8 || (s->seen&(1u<<index)) ||
           (!!(typer&16)!=(s->frame==7))) {fail(s,ERROR_INITIAL);break;}
        s->seen|=1u<<index;
        if(index==s->core)s->redistributor=0x03460000+s->frame*0x20000;
        advance=++s->frame==8;
        if(advance && (s->seen!=255 || !s->redistributor))fail(s,ERROR_INITIAL);
        if(advance && !s->core)s->phase=WAKE_CHECK; /* core0 has no hotplug handoff */
        break;
    }
    case CONTROL:if(read32(s,control,&s->control) && (s->control&1))fail(s,ERROR_INITIAL);break;
    case MODE:if(read32(s,control+0x20,&v) && (v&1))fail(s,ERROR_INITIAL);break;
    case WAKE_ENABLE:(void)write32(s,control,s->control&~2u);break;
    case WAKE_CHECK:if(read32(s,control,&v) && v!=(s->control&~2u))fail(s,ERROR_VERIFY);break;
    /* GIC-600 power protocol: TF-A 0892ad4, drivers/arm/gic/v3/gic-x00.c.
     * Select this core only (RDAG=0). Group transitions may reject a request;
     * recheck stability before reissuing it within the original deadline. */
    case POWER_GROUP_WAIT:
        if(read32(s,r+0x24,&v))advance=((v>>2)&1)==((v>>3)&1);
        break;
    case POWER_REQUEST:(void)write32(s,r+0x24,0);break;
    case POWER_VERIFY:
        if(read32(s,r+0x24,&v) && (v&1)) {s->phase=POWER_GROUP_WAIT;advance=0;}
        break;
    case R_CTLR:if(read32(s,r,&v)) {if(v&1)fail(s,ERROR_INITIAL);advance=!(v&8);}break;
    case WAKER:if(read32(s,r+0x14,&s->waker))advance=!(s->waker&2) || !!(s->waker&4);break;
    case WAKE_CLEAR:(void)write32(s,r+0x14,s->waker&~2u);break;
    case WAKE_WAIT:if(read32(s,r+0x14,&v)) {advance=!(v&4);if(v&2)fail(s,ERROR_VERIFY);}break;
    case DISABLE:(void)write32(s,g+0x180,UINT32_MAX);break;
    case DISABLE_WAIT:if(read32(s,r,&v))advance=!(v&8);break;
    case CLEAR_PENDING:(void)write32(s,g+0x280,UINT32_MAX);break;
    case CLEAR_ACTIVE:(void)write32(s,g+0x380,UINT32_MAX);break;
    case ACTIVE_CHECK:if(read32(s,g+0x300,&v) && v)fail(s,ERROR_VERIFY);break;
    case GROUP:(void)write32(s,g+0x80,UINT32_MAX);break;
    case MODIFIER:(void)write32(s,g+0xd00,0);break;
    case CONFIG:(void)read32(s,g+0xc04,&s->configuration);break;
    case CONFIG_WRITE:(void)write32(s,g+0xc04,s->configuration&~((2u<<26)|(2u<<28)));break;
    case PRIORITY:(void)read32(s,g+0x400,&s->priority);break;
    case PRIORITY_WRITE:(void)write32(s,g+0x400,(s->priority&~255u)|0xf0);break;
    case ENABLE:(void)write32(s,g+0x100,1);break;
    case ENABLE_WAIT:if(read32(s,r,&v))advance=!(v&8);break;
    case VERIFY_GROUP:if(read32(s,g+0x80,&v) && v!=UINT32_MAX)fail(s,ERROR_VERIFY);break;
    case VERIFY_MODIFIER:if(read32(s,g+0xd00,&v) && v)fail(s,ERROR_VERIFY);break;
    case VERIFY_ENABLE:if(read32(s,g+0x100,&v) && v!=1)fail(s,ERROR_VERIFY);break;
    case VERIFY_PRIORITY:if(read32(s,g+0x400,&v) && v!=((s->priority&~255u)|0xf0))fail(s,ERROR_VERIFY);break;
    default:fail(s,ERROR_VERIFY);break;
    }
    if(s->state==A733_CONTINUE_PENDING && time_ok(s) && advance) {
        if(s->phase==VERIFY_PRIORITY)s->state=A733_CONTINUE_GIC_READY;
        else if(s->phase==WAKE_CHECK)s->phase=POWER_GROUP_WAIT;
        else if(s->phase==POWER_VERIFY)s->phase=R_CTLR;
        else ++s->phase;
    }
out:s->entered=0;return s->state;
}

#if defined(__aarch64__)
#include "FreeRTOSConfig.h"
#if !defined(SUNSTONE_A733_SMP_DIAGNOSTIC) || configNUMBER_OF_CORES != 8
#error "CPU continuation requires the dedicated eight-core diagnostic image"
#endif
struct a733_cpu_continue_plan a733_cpu_continue_plan
    __attribute__((section(".bss.a733_cpu_continue_monitor"),aligned(128)));
struct a733_cpu_continue_record a733_cpu_continue_records[8]
    __attribute__((section(".bss.a733_cpu_continue_monitor"),aligned(128)));
uint64_t a733_cpu_continue_exclusives[8][16]
    __attribute__((section(".bss.a733_cpu_continue_monitor"),aligned(128)));
uint64_t a733_cpu_continue_level1[512]
    __attribute__((section(".bss.a733_cpu_continue_monitor"),aligned(4096)));
uint64_t a733_cpu_continue_level2[512]
    __attribute__((section(".bss.a733_cpu_continue_monitor"),aligned(4096)));
unsigned char a733_cpu_el1_stacks[8*A733_CONTINUE_STACK_BYTES]
    __attribute__((aligned(4096)));
extern volatile uint8_t ucPrimaryCoreInitDoneFlag,ucSecondaryCoresReadyFlags[];
extern unsigned char a733_cpu_continue_vectors[],a733_cpu_continue_el1_vectors[];
extern void a733_cpu_continue_enter_el1(unsigned core,uint64_t epoch);
extern int a733_cpu_continue_exclusive_probe(unsigned long core);
static void sync_memory(void) {__asm__ volatile("dsb sy":::"memory");}
static unsigned native_core(void)
{
    uint64_t mpidr;unsigned core;
    __asm__ volatile("mrs %0, mpidr_el1":"=r"(mpidr));
    return a733_cpu_index(mpidr,&core)?core:8;
}
static int native_time(void *unused,uint64_t *out)
{
    (void)unused;uint64_t ticks,freq;
    __asm__ volatile("isb\n mrs %0, cntpct_el0\n mrs %1, cntfrq_el0":"=r"(ticks),"=r"(freq)::"memory");
    if(!freq || freq>UINT32_MAX || ticks/freq>UINT64_MAX/1000000)return 0;
    uint64_t whole=ticks/freq*1000000,part=ticks%freq*1000000/freq;
    if(whole>UINT64_MAX-part)return 0;*out=whole+part;return 1;
}
static int monitor_range(const void *p,size_t n)
{
    uintptr_t a=(uintptr_t)p;
    return a>=UINT32_C(0x42000000) && a<UINT32_C(0x42200000) && n<=UINT32_C(0x42200000)-a;
}
static int code_range(const void *p,size_t n)
{
    uintptr_t a=(uintptr_t)p;
    return a>=UINT32_C(0x41000000) && a<UINT32_C(0x41200000) && n<=UINT32_C(0x41200000)-a;
}
static int linker_ranges(void)
{
    uintptr_t stacks=(uintptr_t)a733_cpu_el1_stacks;
    return monitor_range(&a733_cpu_continue_plan,sizeof(a733_cpu_continue_plan)) &&
        monitor_range(a733_cpu_continue_records,sizeof(a733_cpu_continue_records)) &&
        monitor_range(a733_cpu_continue_exclusives,sizeof(a733_cpu_continue_exclusives)) &&
        monitor_range(a733_cpu_continue_level1,sizeof(a733_cpu_continue_level1)) &&
        monitor_range(a733_cpu_continue_level2,sizeof(a733_cpu_continue_level2)) &&
        monitor_range(a733_cpu_mailboxes,sizeof(a733_cpu_mailboxes)) &&
        monitor_range(a733_cpu_entry_stacks,sizeof(a733_cpu_entry_stacks)) &&
        code_range(a733_cpu_secondary_entry,4) && code_range(a733_cpu_secondary_continue,4) &&
        code_range(a733_cpu_continue_vectors,2048) && code_range(a733_cpu_continue_el1_vectors,2048) &&
        !(stacks&4095) && stacks>=UINT32_C(0x41200000) &&
        stacks+sizeof(a733_cpu_el1_stacks)<=UINT32_C(0x42000000);
}
int a733_cpu_continue_prepare(uint64_t epoch,uint64_t deadline)
{
    uint64_t el,sctlr,now;
    __asm__ volatile("mrs %0, CurrentEL":"=r"(el));if(el!=12)return 0;
    __asm__ volatile("mrs %0, sctlr_el3":"=r"(sctlr));
    if(native_core()!=0 || (sctlr&A733_ENTRY_SCTLR_REJECT) || !epoch ||
       !native_time(NULL,&now) || deadline<=now || !linker_ranges())return 0;
    volatile const uint64_t *plan=(volatile const uint64_t *)&a733_cpu_continue_plan;
    for(unsigned i=0;i<16;++i)if(plan[i])return 0;
    volatile const uint64_t *records=(volatile const uint64_t *)a733_cpu_continue_records;
    for(unsigned i=0;i<128;++i)if(records[i])return 0;
    volatile const uint64_t *exclusive=(volatile const uint64_t *)a733_cpu_continue_exclusives;
    for(unsigned i=0;i<128;++i)if(exclusive[i])return 0;
    if(!a733_cpu_continue_tables(a733_cpu_continue_level1,a733_cpu_continue_level2,
                                 (uintptr_t)a733_cpu_continue_level2))return 0;
    a733_cpu_continue_plan.epoch=epoch;a733_cpu_continue_plan.deadline_us=deadline;
    a733_cpu_continue_plan.ttbr0=(uintptr_t)a733_cpu_continue_level1;
    sync_memory();a733_cpu_continue_plan.magic=A733_CONTINUE_MAGIC;
    sync_memory();return 1;
}
static int native_read32(void *ctx,uintptr_t address,uint32_t *out)
{
    struct a733_cpu_continue *s=ctx;
    uintptr_t c=0x07050200+s->core*4,r=s->redistributor,g=r+0x10000;
    if(address!=0x03400000 && address!=c && address!=c+0x20 &&
       (!r || (address!=r && address!=r+0x14 && address!=r+0x24 && address!=g+0x300 &&
       address!=g+0x80 && address!=g+0xd00 && address!=g+0xc04 &&
       address!=g+0x400 && address!=g+0x100)))return 0;
    sync_memory();*out=*(volatile uint32_t *)address;sync_memory();return 1;
}
static int native_read64(void *ctx,uintptr_t address,uint64_t *out)
{
    (void)ctx;
    if(address<0x03460008 || address>0x03540008 || ((address-0x03460008)&0x1ffff))return 0;
    sync_memory();*out=*(volatile uint64_t *)address;sync_memory();return 1;
}
static int native_write32(void *ctx,uintptr_t address,uint32_t value)
{
    struct a733_cpu_continue *s=ctx;
    uintptr_t c=0x07050200+s->core*4,r=s->redistributor,g=r+0x10000;
    if(address!=c && (!r || (address!=r+0x14 && address!=r+0x24 && address!=g+0x180 && address!=g+0x280 &&
       address!=g+0x380 && address!=g+0x80 && address!=g+0xd00 && address!=g+0xc04 &&
       address!=g+0x400 && address!=g+0x100)))return 0;
    sync_memory();*(volatile uint32_t *)address=value;sync_memory();return 1;
}
static void record_state(unsigned core,uint64_t epoch,unsigned state,unsigned error,uint64_t r)
{
    volatile struct a733_cpu_continue_record *p=&a733_cpu_continue_records[core];
    p->epoch=epoch;p->core=core;p->error=error;p->redistributor=r;
    sync_memory();p->state=state;sync_memory();__asm__ volatile("sev":::"memory");
}
static _Noreturn void park(void) {for(;;)__asm__ volatile("wfe");}
static int run_local(unsigned core,uint64_t epoch,uint64_t mpidr)
{
    struct a733_cpu_continue state={0};
    struct a733_cpu_continue_config config={.ops={native_read32,native_read64,native_write32,native_time},
        .context=&state,.epoch=epoch,.deadline_us=a733_cpu_continue_plan.deadline_us,.mpidr=mpidr};
    if(!a733_cpu_continue_init(&state,&config))return 0;
    while(a733_cpu_continue_poll(&state)==A733_CONTINUE_PENDING) {}
    record_state(core,epoch,state.state,state.error,state.redistributor);
    return state.state==A733_CONTINUE_GIC_READY;
}
void a733_cpu_continue_primary_boot(uint64_t epoch)
{
    uint64_t mpidr,now;
    __asm__ volatile("mrs %0, mpidr_el1":"=r"(mpidr));
    if(native_core()!=0 || !linker_ranges() || !epoch || a733_cpu_continue_plan.epoch!=epoch ||
       a733_cpu_continue_plan.magic!=A733_CONTINUE_MAGIC || !native_time(NULL,&now) ||
       now>=a733_cpu_continue_plan.deadline_us)park();
    record_state(0,epoch,A733_CONTINUE_PENDING,0,0);
    if(!run_local(0,epoch,mpidr))park();
    a733_cpu_continue_enter_el1(0,epoch);park();
}
void a733_cpu_continue_boot(unsigned long core,uint64_t epoch,
                            struct a733_cpu_report *report,struct a733_cpu_command *command)
{
    uint64_t mpidr,now;
    __asm__ volatile("mrs %0, mpidr_el1":"=r"(mpidr));
    if(!core || core>=8 || native_core()!=core || !linker_ranges() ||
       report!=&a733_cpu_mailboxes[core].report || command!=&a733_cpu_mailboxes[core].command)park();
    volatile const struct a733_cpu_continue_plan *plan=&a733_cpu_continue_plan;
    if(plan->magic!=A733_CONTINUE_MAGIC || plan->epoch!=epoch || !epoch ||
       plan->ttbr0!=(uintptr_t)a733_cpu_continue_level1 ||
       command->state!=A733_ENTRY_RELEASE || command->epoch!=epoch || command->core!=core ||
       report->state!=A733_ENTRY_RELEASED || report->epoch!=epoch || report->core!=core ||
       !native_time(NULL,&now) || now>=plan->deadline_us)park();
    for(unsigned i=0;i<12;++i)if(plan->reserved[i])park();
    /* This report is dedicated to continuation; entry acknowledgment stays stable. */
    record_state((unsigned)core,epoch,A733_CONTINUE_PENDING,0,0);
    if(!run_local((unsigned)core,epoch,mpidr))park();
    a733_cpu_continue_enter_el1((unsigned)core,epoch);
    park();
}
void a733_cpu_continue_el1(unsigned long core,uint64_t epoch)
{
    uint64_t el,sctlr,mair,tcr,ttbr,now,last=0;
    __asm__ volatile("mrs %0, CurrentEL\n mrs %1, sctlr_el1\n mrs %2, mair_el1\n"
        "mrs %3, tcr_el1\n mrs %4, ttbr0_el1":"=r"(el),"=r"(sctlr),"=r"(mair),"=r"(tcr),"=r"(ttbr));
    if(core>=8 || native_core()!=core || el!=4 || (sctlr&0x1005)!=1 ||
       mair!=0x4400 || tcr!=UINT64_C(0x200803019) || ttbr!=(uintptr_t)a733_cpu_continue_level1 ||
       a733_cpu_continue_plan.epoch!=epoch)park();
    uint64_t r=a733_cpu_continue_records[core].redistributor;
    if(!native_time(NULL,&now) || now>=a733_cpu_continue_plan.deadline_us ||
       !a733_cpu_continue_exclusive_probe(core) ||
       a733_smp_pre_scheduler(core,epoch,a733_cpu_continue_plan.deadline_us)!=1 ||
       !native_time(NULL,&last) || last<now || last>=a733_cpu_continue_plan.deadline_us) {
        record_state((unsigned)core,epoch,A733_CONTINUE_RETAINED,5,r);park();
    }
    record_state((unsigned)core,epoch,A733_CONTINUE_EL1_READY,0,r);
    if(!core) {
        a733_smp_primary_el1();
        record_state(0,epoch,A733_CONTINUE_RETAINED,ERROR_VERIFY,r);park();
    }
    for(;;) {
        if(!native_time(NULL,&now) || now<last || now>=a733_cpu_continue_plan.deadline_us) {
            record_state((unsigned)core,epoch,A733_CONTINUE_RETAINED,ERROR_TIME,r);park();
        }
        last=now;
        if(__atomic_load_n(&ucPrimaryCoreInitDoneFlag,__ATOMIC_ACQUIRE))break;
        __asm__ volatile("yield");
    }
    record_state((unsigned)core,epoch,A733_CONTINUE_SCHEDULER_REQUEST,0,r);
    __atomic_store_n(&ucSecondaryCoresReadyFlags[core-1],1,__ATOMIC_RELEASE);
    __asm__ volatile("dsb sy\n sev\n svc #106":::"memory");
    record_state((unsigned)core,epoch,A733_CONTINUE_RETAINED,ERROR_VERIFY,r);park();
}
#endif
