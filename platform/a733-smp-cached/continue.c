#if defined(SUNSTONE_A733_EXTENDED_DEVICE_MEMORY)
#include "a733_device_memory_layout.h"
#endif
#include "cached.h"
#include "a733_cached_memory.h"
#include <stddef.h>
#define MONITOR __attribute__((section(".bss.a733_smp_monitor"),aligned(128)))
#define BIT(n) (UINT64_C(1)<<(n))
volatile struct a733_cached_record a733_cached_records[8] MONITOR;
static struct a733_cpu_coherence coherence[8] MONITOR;
/* Explicit section placement is required for every mutable pre-cache object. */
static uint64_t boot_values[4] MONITOR;
struct a733_cached_runtime a733_cached_runtime[8] __attribute__((aligned(128)));
unsigned char a733_cpu_el1_stacks[8*A733_CONTINUE_STACK_BYTES] __attribute__((aligned(4096)));
_Static_assert(sizeof(struct a733_cached_record)==128,"assembly record layout");
_Static_assert(offsetof(struct a733_cached_record,esr)==40,"assembly fault layout");
extern volatile uint8_t ucPrimaryCoreInitDoneFlag,ucSecondaryCoresReadyFlags[];
extern unsigned char __bss_start[],__bss_end[],__tables_start[],__tables_end[];
extern unsigned char __monitor_bss_start[],__monitor_bss_end[];
static void sync_memory(void) {__asm__ volatile("dsb sy":::"memory");}
static unsigned native_core(void)
{
    uint64_t mpidr;unsigned c;
    __asm__ volatile("mrs %0,mpidr_el1":"=r"(mpidr));
    return a733_cpu_index(mpidr,&c)?c:8;
}
static _Noreturn void park(void) {for(;;)__asm__ volatile("wfe");}
static void failed(unsigned c,unsigned error)
{
    if(c<8) {a733_cached_records[c].error=error;sync_memory();a733_cached_records[c].state=6;sync_memory();}
    park();
}
static int time_now(void *unused,uint64_t *out)
{(void)unused;*out=a733_smp_counter()/24;return 1;}
static int read32(void *ctx,uintptr_t a,uint32_t *out)
{
    struct a733_cpu_continue *s=ctx;uintptr_t c=0x07050200+s->core*4,r=s->redistributor,g=r+0x10000;
    if(a!=0x03400000 && a!=c && a!=c+0x20 && (!r ||
       (a!=r && a!=r+0x14 && a!=r+0x24 && a!=g+0x300 && a!=g+0x80 && a!=g+0xd00 &&
        a!=g+0xc04 && a!=g+0x400 && a!=g+0x100)))return 0;
    sync_memory();*out=*(volatile uint32_t *)a;sync_memory();return 1;
}
static int read64(void *ctx,uintptr_t a,uint64_t *out)
{
    (void)ctx;if(a<0x03460008 || a>0x03540008 || ((a-0x03460008)&0x1ffff))return 0;
    sync_memory();*out=*(volatile uint64_t *)a;sync_memory();return 1;
}
static int write32(void *ctx,uintptr_t a,uint32_t value)
{
    struct a733_cpu_continue *s=ctx;uintptr_t c=0x07050200+s->core*4,r=s->redistributor,g=r+0x10000;
    if(a!=c && (!r || (a!=r+0x14 && a!=r+0x24 && a!=g+0x180 && a!=g+0x280 && a!=g+0x380 &&
       a!=g+0x80 && a!=g+0xd00 && a!=g+0xc04 && a!=g+0x400 && a!=g+0x100)))return 0;
    sync_memory();*(volatile uint32_t *)a=value;sync_memory();return 1;
}
static void prepare_core(unsigned core)
{
    struct a733_cpu_coherence *s=&coherence[core];
    if(a733_cpu_coherence_init(s,&a733_cpu_coherence_native_ops,NULL,core,boot_values[3],boot_values[1]/24)!=1)
        failed(core,1);
    int status;do {status=a733_cpu_coherence_poll(s);}while(!status);
    if(status!=1 || (core && (s->identity.clusteridr!=coherence[0].identity.clusteridr ||
        s->identity.clustercfr!=coherence[0].identity.clustercfr)))failed(core,2);
    a733_cached_records[core].epoch=boot_values[0];
    a733_cached_records[core].core=core;
    a733_cached_records[core].clusteridr=s->identity.clusteridr;
    a733_cached_records[core].startup_deadline=boot_values[1];
    a733_cached_records[core].diagnostic_deadline=boot_values[2];
    sync_memory();a733_cached_records[core].state=1;sync_memory();
    a733_cached_block_walks();
}
void a733_cached_prepare(uint64_t epoch,uint64_t startup,uint64_t diagnostic,uint64_t token)
{
    if(native_core()!=0 || !epoch || !token || a733_smp_counter()>=startup || startup>=diagnostic)
        a733_smp_stop(2,0x100);
    boot_values[0]=epoch;boot_values[1]=startup;boot_values[2]=diagnostic;boot_values[3]=token;
    if(!a733_cached_memory_prepare())a733_smp_stop(2,0x101);
    /* The source image and initial RAM are immutable/idle under external cold-boot
     * ownership. Clean publication is not a substitute for that admission. */
    sync_memory();
    if(a733_cpu_coherence_publish(0x41000000,0x200000,startup)!=1 ||
       a733_cpu_coherence_publish(0x41200000,(uintptr_t)__bss_end-0x41200000,startup)!=1 ||
       a733_cpu_coherence_publish((uintptr_t)__tables_start,__tables_end-__tables_start,startup)!=1)
        a733_smp_stop(2,0x102);
    /* No subsequent CPU access to any table page, including reads. */
    prepare_core(0);
}
static void local_boot(unsigned c,uint64_t epoch)
{
    if(c>=8 || native_core()!=c || epoch!=boot_values[0] || !epoch)park();
    if(c)prepare_core(c);
    uint64_t mpidr;__asm__ volatile("mrs %0,mpidr_el1":"=r"(mpidr));
    struct a733_cpu_continue s={0};
    struct a733_cpu_continue_config config={.ops={read32,read64,write32,time_now},.context=&s,
        .epoch=epoch,.deadline_us=boot_values[1]/24,.mpidr=mpidr};
    if(!a733_cpu_continue_init(&s,&config))failed(c,3);
    while(a733_cpu_continue_poll(&s)==A733_CONTINUE_PENDING) {}
    if(s.state!=A733_CONTINUE_GIC_READY)failed(c,4);
    a733_cached_records[c].redistributor=s.redistributor;
    sync_memory();a733_cached_records[c].state=2;sync_memory();
    uint64_t last=0;
    for(unsigned i=0;i<8;++i)for(;;) {
        sync_memory();uint64_t state=a733_cached_records[i].state,now=a733_smp_counter();
        if(state==6 || now<last || now>=boot_values[1])failed(c,5);
        last=now;if(state>=2)break;
    }
    if(a733_cached_allow_ns_code()!=1)failed(c,8);
    if(a733_cpu_coherence_enable_el3((uintptr_t)a733_cached_el3_l1,boot_values[1])!=1)failed(c,6);
    uint64_t sctlr,ttbr;
    __asm__ volatile("mrs %0,sctlr_el3\n mrs %1,ttbr0_el3":"=r"(sctlr),"=r"(ttbr));
    a733_cached_records[c].sctlr3=sctlr;a733_cached_records[c].ttbr3=ttbr;
    sync_memory();a733_cached_records[c].state=3;sync_memory();
    for(unsigned i=0;i<8;++i)for(;;) {
        sync_memory();uint64_t state=a733_cached_records[i].state,now=a733_smp_counter();
        if(state==6 || now<last || now>=boot_values[1])failed(c,7);
        last=now;if(state==3)break;
    }
    a733_cached_enter_el1(c,epoch,boot_values[1],boot_values[2],s.redistributor);
}
void a733_cpu_continue_primary_boot(uint64_t epoch) {local_boot(0,epoch);}
void a733_cpu_continue_boot(unsigned long core,uint64_t epoch,
                            struct a733_cpu_report *report,struct a733_cpu_command *command)
{
    if(!core || core>=8 || report!=&a733_cpu_mailboxes[core].report ||
       command!=&a733_cpu_mailboxes[core].command || command->state!=A733_ENTRY_RELEASE ||
       report->state!=A733_ENTRY_RELEASED || command->epoch!=epoch || report->epoch!=epoch ||
       command->core!=core || report->core!=core)park();
    local_boot((unsigned)core,epoch);
}
_Noreturn void a733_cached_el1(unsigned c,uint64_t epoch,uint64_t startup,uint64_t diagnostic,uint64_t rdist)
{
    if(c>=8 || native_core()!=c || !epoch || a733_smp_counter()>=startup)a733_cached_smc(3,0x200);
    a733_cached_runtime[c].epoch=epoch;a733_cached_runtime[c].startup_deadline=startup;
    a733_cached_runtime[c].diagnostic_deadline=diagnostic;a733_cached_runtime[c].redistributor=rdist;
    if(a733_smp_pre_scheduler(c,epoch,startup/24)!=1)a733_cached_smc(3,0x201);
    if(!c) {
#if defined(SUNSTONE_A733_EXTENDED_DEVICE_MEMORY)
        int prepared;
        do { prepared = a733_device_memory_prepare(); } while (prepared == 0);
        if (prepared != 1) a733_cached_smc(3, 0x203);
#endif
        a733_smp_primary_el1();a733_cached_smc(4,0x202);
    }
    uint64_t last=0;
    while(!__atomic_load_n(&ucPrimaryCoreInitDoneFlag,__ATOMIC_ACQUIRE)) {
        uint64_t now=a733_smp_counter();
        if(now<last || now>=startup)a733_cached_smc(3,0x203);
        last=now;__asm__ volatile("yield");
    }
    __atomic_store_n(&ucSecondaryCoresReadyFlags[c-1],1,__ATOMIC_RELEASE);
    __asm__ volatile("dsb sy\n sev\n svc #106":::"memory");
    a733_cached_smc(4,0x204);
}
