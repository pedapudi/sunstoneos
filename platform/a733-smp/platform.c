#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "a733_cpu_continue.h"
#include "boot.h"
static uint64_t tick_interval,next_tick,diagnostic_deadline;
static uint32_t probe_lock,probe_count,probe_arrived[8],probe_finished[8],probe_release;
struct a733_smp_core_result a733_smp_core_results[8] __attribute__((aligned(128)));
extern uint64_t a733_smp_diagnostic_deadline(void);
extern void FreeRTOS_SGI_Handler(void);
extern void a733_smp_diagnostic_start(void);
static unsigned core(void)
{uint64_t c;__asm__ volatile("mrs %0, tpidr_el1":"=r"(c));return (unsigned)c;}
/* A bounded exclusive pair avoids the kernel's unlimited spin until inter-core
 * exclusion has been exercised. Every loop has the common boot deadline. */
static int probe_take(uint64_t deadline)
{
    for(unsigned attempt=0;attempt<1000000;++attempt) {
        if(a733_smp_counter()/24>=deadline)return 0;
        uint32_t old,failed;
        __asm__ volatile("ldaxr %w0,[%2]\n cbnz %w0,1f\n mov %w1,#1\n"
            "stlxr %w0,%w1,[%2]\n b 2f\n1: clrex\n2:"
            :"=&r"(old),"=&r"(failed):"r"(&probe_lock):"memory");
        if(!old)return 1;
        __asm__ volatile("yield");
    }
    return 0;
}
int a733_smp_pre_scheduler(unsigned long c,uint64_t epoch,uint64_t deadline)
{
    /* TPIDRRO_EL0 is UNKNOWN at reset. A zero task field makes the FreeRTOS
     * port read the core index through SVC until it first resumes a task. */
    __asm__ volatile("msr tpidrro_el0, xzr":::"memory");
    if(c>=8 || !epoch)return 0;
    __atomic_store_n(&probe_arrived[c],1,__ATOMIC_RELEASE);
    for(unsigned i=0;i<8;++i)while(!__atomic_load_n(&probe_arrived[i],__ATOMIC_ACQUIRE))
        if(a733_smp_counter()/24>=deadline)return 0;
    for(unsigned i=0;i<32;++i) {
        if(!probe_take(deadline))return 0;
        uint32_t before=probe_count;
        __asm__ volatile("dmb sy":::"memory");
        probe_count=before+1;
        __atomic_store_n(&probe_lock,0,__ATOMIC_RELEASE);
    }
    __atomic_store_n(&probe_finished[c],1,__ATOMIC_RELEASE);
    if(!c) {
        for(unsigned i=0;i<8;++i)while(!__atomic_load_n(&probe_finished[i],__ATOMIC_ACQUIRE))
            if(a733_smp_counter()/24>=deadline)return 0;
        if(probe_count!=256)return 0;
        __atomic_store_n(&probe_release,1,__ATOMIC_RELEASE);
    }
    while(!__atomic_load_n(&probe_release,__ATOMIC_ACQUIRE))
        if(a733_smp_counter()/24>=deadline)return 0;
    return 1;
}
uint64_t a733_smp_sgi_value(unsigned interrupt,unsigned target)
{
    uint64_t value;
    configASSERT(interrupt==0 && a733_cpu_sgi1r(target,&value));
    return value;
}
_Noreturn void a733_smp_exit_privileged(unsigned status,uint64_t detail)
{
    __asm__ volatile("msr daifset,#15":::"memory");
    unsigned c=core();
    if(c<8 && status)__atomic_store_n(&a733_smp_core_results[c].failure,detail?detail:status,__ATOMIC_RELEASE);
    if(!c)a733_smp_stop(status,detail);
    for(;;)__asm__ volatile("wfe");
}
_Noreturn void platform_exit(unsigned status)
{
    register uint64_t s __asm__("x0")=status;
    register uint64_t d __asm__("x1")=status;
    __asm__ volatile("svc #119":"+r"(s),"+r"(d)::"memory");
    for(;;) {}
}
_Noreturn void platform_panic(const char *file,unsigned line)
{
    (void)file;
    register uint64_t s __asm__("x0")=4;
    register uint64_t d __asm__("x1")=line;
    __asm__ volatile("svc #119":"+r"(s),"+r"(d)::"memory");
    for(;;) {}
}
void a733_smp_primary_el1(void)
{
    uintptr_t r=a733_cpu_continue_records[0].redistributor,g=r+0x10000;
    *(volatile uint8_t *)(g+0x400+30)=portLOWEST_USABLE_INTERRUPT_PRIORITY<<portPRIORITY_SHIFT;
    *(volatile uint32_t *)(g+0x100)=1u<<30;
    __asm__ volatile("dsb sy":::"memory");
    while(*(volatile uint32_t *)r&8)a733_smp_startup_check();
    diagnostic_deadline=a733_smp_diagnostic_deadline();
    for(unsigned c=0;c<8;++c)vInterruptCore(0,c);
    a733_smp_diagnostic_start();
    a733_smp_stop(4,1);
}
void platform_tick_setup(void)
{
    configASSERT(core()==0);
    tick_interval=A733_SMP_COUNTER_HZ/configTICK_RATE_HZ;
    next_tick=a733_smp_counter();platform_tick_ack();
    uint64_t one=1;
    __asm__ volatile("msr cntp_ctl_el0,%0\n isb"::"r"(one):"memory");
}
void platform_tick_ack(void)
{
    next_tick+=tick_interval;
    __asm__ volatile("msr cntp_cval_el0,%0\n isb"::"r"(next_tick):"memory");
}
void vApplicationIRQHandler(uint64_t interrupt)
{
    unsigned c=core();
    if(a733_smp_counter()>=diagnostic_deadline)a733_smp_exit_privileged(3,interrupt);
    if(interrupt==30 && !c) {
        ++a733_smp_core_results[c].ticks;FreeRTOS_Tick_Handler();
    } else if(interrupt==0 && c<8) {
        __atomic_add_fetch(&a733_smp_core_results[c].sgis,1,__ATOMIC_RELAXED);
        FreeRTOS_SGI_Handler();
    } else if(interrupt<1020)a733_smp_exit_privileged(4,interrupt+0x100);
}
