#include "boot.h"
#include "a733_cpu_continue.h"
#include <stddef.h>
static struct a733_smp_contract admission __attribute__((section(".bss.a733_smp_monitor")));
static struct a733_cpu_start power[7] __attribute__((section(".bss.a733_smp_monitor")));
static struct a733_cpu_entry owners[7] __attribute__((section(".bss.a733_smp_monitor")));
static unsigned targets[7] __attribute__((section(".bss.a733_smp_monitor")));
#define RESULT ((volatile struct a733_smp_result *)A733_SMP_RESULT_ADDRESS)
static void barrier(void) { __asm__ volatile("dsb sy" ::: "memory"); }
uint64_t a733_smp_counter(void)
{
    uint64_t n; __asm__ volatile("isb\n mrs %0, cntpct_el0":"=r"(n)::"memory"); return n;
}
static uint32_t read32(uintptr_t a) {barrier();uint32_t v=*(volatile uint32_t *)a;barrier();return v;}
static void write32(uintptr_t a,uint32_t v) {barrier();*(volatile uint32_t *)a=v;barrier();}
_Noreturn void a733_smp_stop(unsigned status,uint64_t detail)
{
    __asm__ volatile("msr daifset, #15":::"memory");
    RESULT->completed_mask=status?0:255;
    RESULT->status=status;RESULT->detail=detail;RESULT->end_counter=a733_smp_counter();
    barrier();RESULT->stage=5;barrier();
    /* Keep every CPU, register and memory reservation. The independently armed
     * watchdog owns reset; this image never feeds, disarms or resets it. */
    for(;;)__asm__ volatile("wfe");
}
static int boot_state(void)
{
    uint64_t el,mpidr,sctlr;unsigned core;
    __asm__ volatile("mrs %0, CurrentEL":"=r"(el));if(el!=12)return 0;
    __asm__ volatile("mrs %0, mpidr_el1\n mrs %1, sctlr_el3":"=r"(mpidr),"=r"(sctlr));
    return a733_cpu_index(mpidr,&core) && !core && !(sctlr&A733_ENTRY_SCTLR_REJECT);
}
static int power_read(void *ctx,uintptr_t a,uint32_t *out)
{
    unsigned core=*(const unsigned *)ctx;
    if(!out || !boot_state() || !a733_smp_power_address(core,a,0))return 0;
    *out=read32(a);return 1;
}
static int power_write(void *ctx,uintptr_t a,uint32_t value)
{
    if(!boot_state() || !a733_smp_power_address(*(const unsigned *)ctx,a,1))return 0;
    write32(a,value);return 1;
}
static int power_barrier(void *ctx) {(void)ctx;if(!boot_state())return 0;barrier();return 1;}
static int now_us(void *ctx,uint64_t *out)
{(void)ctx;if(!out || !boot_state())return 0;*out=a733_smp_counter()/24;return 1;}
void a733_smp_startup_check(void)
{
    static uint64_t last;
    uint64_t now=a733_smp_counter();
    if(now<last || now>=admission.startup_deadline)a733_smp_stop(3,now);
    last=now;
}
uintptr_t a733_smp_priority_address(void)
{return a733_cpu_continue_records[0].redistributor+0x10400;}
static void wait_clear(uintptr_t address,uint32_t mask)
{
    do {a733_smp_startup_check();if(!(read32(address)&mask))return;}while(1);
}
#include "a733_el3_security.h"
#include "a733_el3_mbus.h"
void a733_smp_boot(void)
{
    volatile const struct a733_smp_contract *source=(void *)A733_SMP_CONTRACT_ADDRESS;
    volatile const uint64_t *in=(const volatile uint64_t *)source;
    uint64_t *out=(uint64_t *)&admission;
    for(unsigned i=0;i<sizeof(admission)/8;++i)out[i]=in[i];
    barrier();
    if(!boot_state() || !a733_smp_contract_valid(&admission,a733_smp_counter()) ||
       source->magic!=admission.magic || source->generation!=admission.generation ||
       (RESULT->magic==A733_SMP_RESULT_MAGIC && RESULT->generation==admission.generation))
        a733_smp_stop(1,1);
    /* Admission values are recorded by the qualified external arm procedure.
     * Readback can reject changed configuration; it cannot prove timer progress. */
    if((read32(0x02050014)&0xffff)!=admission.watchdog_mode ||
       (read32(0x02050010)&0xffff)!=admission.watchdog_configuration ||
       (read32(0x02050018)&0xffff)!=admission.watchdog_output ||
       read32(0x07090108)!=admission.fel_flag ||
       read32(0x070501c0)!=admission.redirect_flag ||
       read32(0x070501c4)!=admission.redirect_entry ||
       read32(0x070501d4)!=admission.alternate_flag ||
       (read32(0x03000024)&0x100))a733_smp_stop(1,2);
    volatile uint64_t *r=(volatile uint64_t *)RESULT;
    for(unsigned i=0;i<sizeof(*RESULT)/8;++i)r[i]=0;
    RESULT->magic=A733_SMP_RESULT_MAGIC;RESULT->epoch=admission.epoch;
    RESULT->generation=admission.generation;RESULT->start_counter=a733_smp_counter();
    RESULT->status=UINT64_MAX;RESULT->stage=1;
    if(!a733_cpu_continue_prepare(admission.epoch,admission.startup_deadline/24))a733_smp_stop(2,1);
    /* GICv3 security/routing sequence follows the existing EL3 monitor. */
    uint32_t ctlr=read32(0x03400000),banks=(read32(0x03400004)&31)+1;
    if(ctlr&(1u<<6))a733_smp_stop(2,2);
    write32(0x03400000,ctlr&~7u);wait_clear(0x03400000,1u<<31);
    for(unsigned i=1;i<banks;++i)write32(0x03400180+i*4,UINT32_MAX);
    wait_clear(0x03400000,1u<<31);
    write32(0x03400000,(ctlr&~7u)|0x30);wait_clear(0x03400000,1u<<31);
    write32(0x03400000,(ctlr&~7u)|0x33);wait_clear(0x03400000,1u<<31);
    if((read32(0x03400000)&0x73)!=0x33)a733_smp_stop(2,3);
    uint64_t mpidr;__asm__ volatile("mrs %0, mpidr_el1":"=r"(mpidr));
    for(unsigned c=1;c<8;++c) {
        targets[c-1]=c;
        struct a733_cpu_start_config config={.ops={power_read,power_write,power_barrier,now_us},
            .context=&targets[c-1],.boot_mpidr=mpidr,.entry=(uintptr_t)a733_cpu_secondary_entry,
            .lease=admission.lease,.deadline=admission.startup_deadline/24,.core=c};
        if(!a733_cpu_start_init(&power[c-1],&config) ||
           !a733_cpu_entry_native_init(&owners[c-1],&power[c-1],admission.epoch))a733_smp_stop(2,c+16);
        while(a733_cpu_entry_poll(&owners[c-1])==A733_CPU_ENTRY_PENDING) {}
        if(owners[c-1].state!=A733_CPU_ENTRY_DISPATCHED)a733_smp_stop(2,c+32);
    }
    /* The vendor's cold-boot security setup, before any core enters EL1:
     * the secondaries wait for the primary's continuation below. MBUS goes
     * back to boot0's 600 MHz first (include/a733_el3_mbus.h). */
    {unsigned mbus=a733_el3_mbus_restore();if(mbus)a733_smp_stop(2,0x200u|mbus);}
    a733_el3_security_setup();
    RESULT->stage=2;barrier();
    a733_cpu_primary_continue(admission.epoch);
    a733_smp_stop(2,4);
}
uint64_t a733_smp_diagnostic_deadline(void) {return admission.diagnostic_deadline;}
