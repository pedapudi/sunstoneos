#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "a733_cpu_continue.h"
#include "cached.h"
#ifdef SUNSTONE_A733_CACHED_DEVICES
#include "a733_cached_devices.h"
#endif
static uint64_t tick_interval,next_tick,diagnostic_deadline;
static uint32_t probe_lock,probe_count,probe_arrived[8],probe_finished[8],probe_release;
struct a733_smp_core_result a733_smp_core_results[8] __attribute__((aligned(128)));
extern uint64_t a733_smp_diagnostic_deadline(void);
extern void FreeRTOS_SGI_Handler(void);
#ifdef SUNSTONE_A733_IRQ_TABLE
#include "irq_table.h"
#endif
#ifdef SUNSTONE_A733_PERF_SESSION
#include "perf_session.h"
#endif
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
#ifdef SUNSTONE_A733_PERF_SESSION
    if(!a733_perf_pre_scheduler((unsigned)c,deadline))return 0;
#endif
    return 1;
}
int a733_smp_device_preparation_admitted(void)
{
    uint64_t el, mpidr, control, flags; unsigned index;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    if (el != 4) return 0;
    __asm__ volatile("mrs %0, mpidr_el1\n mrs %1, sctlr_el1\n mrs %2, daif"
        : "=r"(mpidr), "=r"(control), "=r"(flags));
    extern volatile uint8_t ucPrimaryCoreInitDoneFlag;
    if (!a733_cpu_index(mpidr, &index) || index || core() ||
        (control & 0x1005) != 0x1005 || (flags & 0x3c0) != 0x3c0 ||
        __atomic_load_n(&ucPrimaryCoreInitDoneFlag, __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&probe_release, __ATOMIC_ACQUIRE) ||
        a733_smp_counter() >= a733_cached_runtime[0].startup_deadline) return 0;
    for (unsigned c = 0; c < 8; ++c)
        if (!__atomic_load_n(&probe_arrived[c], __ATOMIC_ACQUIRE) ||
            !__atomic_load_n(&probe_finished[c], __ATOMIC_ACQUIRE)) return 0;
    return 1;
}
_Noreturn void a733_smp_exit_privileged(unsigned status,uint64_t detail)
{
    __asm__ volatile("msr daifset,#15":::"memory");
    unsigned c=core();
    if(c<8 && status)__atomic_store_n(&a733_smp_core_results[c].failure,detail?detail:status,__ATOMIC_RELEASE);
    a733_cached_smc(status,detail);
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
#ifdef SUNSTONE_A733_CACHED_DEVICES
    if (!a733_cached_devices_prepare(&a733_cached_device_inputs))
        a733_smp_exit_privileged(4, 0x206);
#endif
    uintptr_t r=a733_cached_runtime[0].redistributor,g=r+0x10000;
    *(volatile uint8_t *)(g+0x400+30)=portLOWEST_USABLE_INTERRUPT_PRIORITY<<portPRIORITY_SHIFT;
    *(volatile uint32_t *)(g+0x100)=1u<<30;
    __asm__ volatile("dsb sy":::"memory");
    while(*(volatile uint32_t *)r&8)a733_smp_startup_check();
    diagnostic_deadline=a733_smp_diagnostic_deadline();
    for(unsigned c=0;c<8;++c)vInterruptCore(0,c);
    a733_smp_diagnostic_start();
    a733_cached_smc(4,1);
}
void platform_tick_setup(void)
{
    configASSERT(core()==0);
    tick_interval=A733_SMP_COUNTER_HZ/configTICK_RATE_HZ;
    next_tick=a733_smp_counter();platform_tick_ack();
    uint64_t one=1;
    __asm__ volatile("msr cntp_ctl_el0,%0\n isb"::"r"(one):"memory");
}
/* CNTP_CTL_EL0.ISTATUS: the counter has reached the compare value. */
static inline uint64_t tick_due(void)
{uint64_t v;__asm__ volatile("mrs %0, cntp_ctl_el0":"=r"(v)::"memory");return v&4u;}
/* Tick handler calls per IRQ, as Linux hrtimer_interrupt retries its expiry
 * at most three times; the level-sensitive timer interrupt is taken again
 * for any period still due. */
#define TICK_CALLS_PER_IRQ 3u
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
#ifdef SUNSTONE_A733_PERF_SESSION
        a733_perf_tick_irq();
#endif
        /* An IRQ whose timer condition no longer holds processes no tick, as
         * Linux's timer handler acts only on ISTATUS. Each tick handler call
         * advances the compare value by one period (platform_tick_ack), so
         * the next deadline stays in phase. When the condition still holds
         * after the first call, the IRQ was late, and at least one more
         * period is due. With TICK_CALLS_PER_IRQ at 3, one comparison of the
         * counter with the next deadline plus a period decides whether a
         * second is due as well; no division is needed. A counter read taken
         * early can only lower that count. The FromISR critical section of
         * the first call cleared PSTATE.I, which every further call requires
         * set. */
        if(tick_due()) {
            ++a733_smp_core_results[c].ticks;FreeRTOS_Tick_Handler();
            if(tick_due()) {
                _Static_assert(TICK_CALLS_PER_IRQ==3u,"one comparison counts the due periods");
                uint64_t now;
                __asm__ volatile("mrs %0, cntpct_el0":"=r"(now)::"memory");
                unsigned due=1u+(now>=next_tick && now-next_tick>=tick_interval);
                while(due--) {
                    __asm__ volatile("msr daifset,#2":::"memory");
                    ++a733_smp_core_results[c].ticks;FreeRTOS_Tick_Handler();
                }
            }
        }
    } else if(interrupt==0 && c<8) {
        /* Only this core writes its count, so a plain load and store suffice;
         * the relaxed store keeps other cores' relaxed loads untorn. */
        __atomic_store_n(&a733_smp_core_results[c].sgis,a733_smp_core_results[c].sgis+1,__ATOMIC_RELAXED);
        FreeRTOS_SGI_Handler();
#ifdef SUNSTONE_A733_IRQ_TABLE
    } else if(interrupt<1020 && a733_irq_table_dispatch((unsigned)interrupt,c)) {
#endif
    } else if(interrupt<1020)a733_smp_exit_privileged(4,interrupt+0x100);
}

uintptr_t a733_smp_priority_address(void)
{return a733_cached_runtime[0].redistributor+0x10400;}
void a733_smp_startup_check(void)
{
    unsigned c=core();
    if(c>=8 || a733_smp_counter()>=a733_cached_runtime[c].startup_deadline)
        a733_cached_smc(3,0x205);
}
uint64_t a733_smp_diagnostic_deadline(void)
{return a733_cached_runtime[core()].diagnostic_deadline;}
