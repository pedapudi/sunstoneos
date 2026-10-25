/* Measurements for the multicore performance board session
 * (docs/board-runbooks/multicore-performance-session.md), built only with
 * SUNSTONE_A733_PERF_SESSION. They run before the unchanged workload and
 * write only the reserved words of each core's a733_smp_core_results record:
 *
 *   reserved[0]  PMU cycles of 64 critical-section pairs, least of 16 samples
 *   reserved[1]  PMU cycles of one SGI to the own core, least of 16 samples
 *   reserved[2]  cores 1-7: least CNTPCT difference, this core minus core 0
 *   reserved[3]  cores 1-7: least CNTPCT difference, core 0 minus this core
 *   reserved[4]  core 0: tick IRQs across the forced late tick
 *   reserved[5]  core 0: kernel ticks across the forced late tick
 *   reserved[6]  core 0: tick IRQs taken
 *   reserved[7], reserved[8]  zero
 *   reserved[9]  PMU cycles of an empty region, already subtracted above
 *   reserved[10] the record version, PERF_SESSION_VERSION, once complete
 *
 * Differences 2 and 3 are signed: a negative value means that a core read a
 * counter value below one that the other core had already published.
 *
 * Only the workers wait for the measurements; the background tasks run
 * throughout, so no core ever runs its idle task. The returning profile ends
 * a core's run when its idle task first starts, so the measurements cannot
 * use idle cores, and wake placement between idle cores is measured in the
 * emulators only.
 *
 * The returning-profile machinery owns the writable interval that ends at
 * 0x41291000, a few hundred bytes after the workload's data. The measurement
 * tasks therefore keep their control blocks and stacks in the lower halves of
 * the background tasks' stacks; each background task's own use stays far
 * above the midpoint. One 128-byte object holds the shared state. */
#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "boot.h"
#include "perf_session.h"

#define SAMPLES 16u
#define PAIRS 64u
#define SKEW_EXCHANGES 1024u
#define LATE_TICKS 132000u /* 5.5 periods of the 24 MHz counter at 1 kHz */
#define SETTLE_TICKS 1000u
#define PERF_SESSION_VERSION 0x5045524631u /* "PERF1" */

static inline uint64_t counter(void)
{uint64_t v;__asm__ volatile("isb\n mrs %0, cntpct_el0":"=r"(v)::"memory");return v;}
static inline uint64_t cycles(void)
{uint64_t v;__asm__ volatile("isb\n mrs %0, pmccntr_el0":"=r"(v)::"memory");return v;}
static int timely(uint64_t deadline){return a733_smp_counter()/24<deadline;}

/* One cache line per direction of the counter exchange. Line 0 also holds
 * the core whose turn it is; line 1 the measurement-task state. A common
 * symbol, which link.ld places after every .bss section, so the workload's
 * data keeps the addresses that the returning-profile machinery pins. */
struct __attribute__((aligned(64))) a733_perf_shared {
    volatile uint64_t value;uint32_t sequence,turn;
    uint8_t pad[48];
    volatile uint64_t value1;uint32_t sequence1,measured;
    StaticTask_t *workload;TaskHandle_t perf0;uint64_t unused[2];
};
extern struct a733_perf_shared a733_perf_shared;
__attribute__((common)) struct a733_perf_shared a733_perf_shared;
#define shared a733_perf_shared
_Static_assert(sizeof(struct a733_perf_shared)==128,"one cache line per direction");

int a733_perf_pre_scheduler(unsigned c,uint64_t deadline)
{
    /* The cycle counter runs, and EL0 tasks may read it (PMUSERENR_EL0.EN
     * and .CR). MDCR_EL2.TPM and MDCR_EL3.TPM are zero (continue.S). */
    uint64_t control;
    __asm__ volatile("mrs %0, pmcr_el0":"=r"(control));
    __asm__ volatile("msr pmcr_el0, %0\n msr pmcntenset_el0, %1\n msr pmuserenr_el0, %2\n isb"
        ::"r"(control|1u),"r"(UINT64_C(1)<<31),"r"(UINT64_C(5)):"memory");
    if(!c) {
        for(unsigned n=1;n<8;++n) {
            __atomic_store_n(&shared.turn,n,__ATOMIC_RELEASE);
            int64_t least=INT64_MAX;
            for(uint32_t i=1;i<=SKEW_EXCHANGES;++i) {
                const uint32_t sequence=(n<<16)|i;
                shared.value=counter();
                __atomic_store_n(&shared.sequence,sequence,__ATOMIC_RELEASE);
                while(__atomic_load_n(&shared.sequence1,__ATOMIC_ACQUIRE)!=sequence)
                    if(!timely(deadline))return 0;
                const int64_t difference=(int64_t)(counter()-shared.value1);
                if(difference<least)least=difference;
            }
            a733_smp_core_results[n].reserved[3]=(uint64_t)least;
        }
        __atomic_store_n(&shared.turn,8,__ATOMIC_RELEASE);
        return 1;
    }
    while(__atomic_load_n(&shared.turn,__ATOMIC_ACQUIRE)!=c)
        if(!timely(deadline))return 0;
    int64_t least=INT64_MAX;
    for(uint32_t i=1;i<=SKEW_EXCHANGES;++i) {
        const uint32_t sequence=(c<<16)|i;
        while(__atomic_load_n(&shared.sequence,__ATOMIC_ACQUIRE)!=sequence)
            if(!timely(deadline))return 0;
        const int64_t difference=(int64_t)(counter()-shared.value);
        if(difference<least)least=difference;
        shared.value1=counter();
        __atomic_store_n(&shared.sequence1,sequence,__ATOMIC_RELEASE);
    }
    a733_smp_core_results[c].reserved[2]=(uint64_t)least;
    return 1;
}

void a733_perf_tick_irq(void)
{++a733_smp_core_results[0].reserved[6];}

#define LEAST(result,body) do {                                  \
        uint64_t least_=UINT64_MAX;                              \
        for(unsigned i_=0;i_<SAMPLES;++i_) {                     \
            const uint64_t start_=cycles();                      \
            body;                                                \
            const uint64_t cost_=cycles()-start_;                \
            if(cost_<least_)least_=cost_;                        \
        }                                                        \
        (result)=least_;                                         \
    } while(0)

/* A measurement task's control block and stack, in the lower half of a
 * borrowed workload stack of 2,048 words. The region starts BORROW_OFFSET
 * words above the stack's lowest address, so the 16 bytes that the stack
 * overflow check (configCHECK_FOR_STACK_OVERFLOW 2) reads keep their fill
 * pattern. */
#define PERF_STACK_WORDS 960u
#define BORROW_OFFSET 8u
struct borrowed {StaticTask_t tcb;StackType_t stack[PERF_STACK_WORDS];};
_Static_assert(BORROW_OFFSET*sizeof(StackType_t)+sizeof(struct borrowed)<=1024u*sizeof(StackType_t),
               "borrowed region exceeds half a stack");

static void perf(void *argument)
{
    const unsigned c=(unsigned)(uintptr_t)argument;
    configASSERT(portGET_CORE_ID()==c);
    struct a733_smp_core_result *const r=&a733_smp_core_results[c];
    uint64_t empty,pairs,sgi;
    LEAST(empty,(void)0);
    LEAST(pairs,for(unsigned p=0;p<PAIRS;++p){taskENTER_CRITICAL();taskEXIT_CRITICAL();});
    LEAST(sgi,vInterruptCore(portYIELD_CORE_INT_ID,(uint8_t)c));
    r->reserved[9]=empty;r->reserved[0]=pairs-empty;r->reserved[1]=sgi-empty;
    if(c) {
        __atomic_or_fetch(&shared.measured,1u<<c,__ATOMIC_RELEASE);
        vTaskSuspend(NULL);platform_panic("perf task resumed",5);
    }
    while(__atomic_load_n(&shared.measured,__ATOMIC_ACQUIRE)!=0xfeu)vTaskDelay(1);
    /* A tick held behind the interrupt mask for 5.5 periods. */
    const uint64_t irqs=r->reserved[6];const TickType_t ticks=xTaskGetTickCount();
    taskDISABLE_INTERRUPTS();
    for(const uint64_t start=counter();counter()-start<LATE_TICKS;) {}
    taskENABLE_INTERRUPTS();
    for(const uint64_t start=counter();counter()-start<SETTLE_TICKS;) {}
    r->reserved[4]=r->reserved[6]-irqs;r->reserved[5]=xTaskGetTickCount()-ticks;
    for(unsigned i=0;i<8;++i)
        __atomic_store_n(&a733_smp_core_results[i].reserved[10],PERF_SESSION_VERSION,__ATOMIC_RELEASE);
    for(unsigned i=0;i<8;++i)vTaskResume((TaskHandle_t)&shared.workload[i]);
    vTaskSuspend(NULL);platform_panic("perf task resumed",5);
}

void a733_perf_session_start(StaticTask_t *tasks,StackType_t *stacks,unsigned stack_words)
{
    configASSERT(stack_words==2048u);
    shared.workload=tasks;
    /* Workers are tasks 0-7 and background tasks 8-15. Only the workers wait. */
    for(unsigned i=0;i<8;++i)vTaskSuspend((TaskHandle_t)&tasks[i]);
    /* Perf task c borrows the lower half of background task c's stack. */
    for(unsigned c=0;c<8;++c) {
        struct borrowed *b=(struct borrowed *)&stacks[(8u+c)*stack_words+BORROW_OFFSET];
        TaskHandle_t t=xTaskCreateStaticAffinitySet(perf,"perf",PERF_STACK_WORDS,(void *)(uintptr_t)c,4,
            b->stack,&b->tcb,1UL<<c);
        configASSERT(t);
        if(!c)shared.perf0=t;
    }
}
