#include "a733_counter_clock.h"
#include <stddef.h>

_Static_assert(offsetof(struct a733_counter_clock,last_ticks)>=offsetof(struct a733_counter_clock,failed)+64,
    "history store must not share the line of the fault flag that every caller reads");
#ifdef SUNSTONE_COUNTER_MODEL
/* Host contention model: exception level and counter reads. */
extern uint64_t a733_counter_model_level(void);
extern uint64_t a733_counter_model_frequency(void);
extern uint64_t a733_counter_model_ticks(void);
static uint64_t level(void){return a733_counter_model_level();}
static uint64_t frequency_now(void){return a733_counter_model_frequency();}
static uint64_t ticks_now(void){return a733_counter_model_ticks();}
#else
static uint64_t level(void){uint64_t v;__asm__ volatile("mrs %0, CurrentEL" : "=r"(v));return v;}
static uint64_t frequency_now(void){uint64_t v;__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v) :: "memory");return v;}
/* The ISB keeps the read from executing before earlier instructions, as in
 * Linux __arch_counter_get_cntpct (arch/arm64/include/asm/arch_timer.h). */
static uint64_t ticks_now(void){uint64_t v;__asm__ volatile("isb\n mrs %0, cntpct_el0" : "=r"(v) :: "memory");return v;}
#endif

int a733_counter_clock_init(struct a733_counter_clock *c,uint32_t expected)
{
    if (!c || c->initialized || !expected || level()!=4) return 0;
    uint64_t frequency=frequency_now(),ticks=ticks_now();
    if (frequency!=expected) return 0;
    atomic_init(&c->failed,0);atomic_init(&c->last_ticks,ticks);
    c->frequency=expected;c->initialized=1;return 1;
}
int a733_counter_clock_now(void *cookie,uint64_t *out)
{
    struct a733_counter_clock *c=cookie;
    if (!c || !c->initialized || !out || atomic_load_explicit(&c->failed,memory_order_acquire)) return 0;
    /* The history only rises: a caller raises it to its own sample with a
     * compare-and-swap, or reports the history when its sample is not above
     * it. A failed swap reloads the history and samples the counter again, so
     * a caller interrupted between its sample and its swap cannot lower the
     * history or delay other callers. Every sample is taken after the history
     * it is compared with was loaded, so a sample below it shows only the
     * skew between cores' counter reads. Up to 100 microseconds of that is
     * reported as the history; a larger backward step faults the object. */
    const uint64_t skew=c->frequency/10000u+1u;
    uint64_t last=atomic_load_explicit(&c->last_ticks,memory_order_relaxed),ticks;
    for(;;) {
        ticks=ticks_now();
        if(ticks<=last) {
            if(last-ticks>skew){atomic_store_explicit(&c->failed,1,memory_order_release);return 0;}
            ticks=last;break;
        }
        if(atomic_compare_exchange_weak_explicit(&c->last_ticks,&last,ticks,
            memory_order_relaxed,memory_order_relaxed))break;
    }
    const uint64_t frequency=frequency_now();
    uint64_t microseconds=0;int ok=0;
    if(frequency==c->frequency) {
        /* The native 24 MHz rate converts with a constant divisor and
         * cannot overflow. Other admitted rates keep the checked form. */
        if(frequency==24000000u) {
            microseconds=ticks/24u;ok=1;
        } else if(ticks/frequency<=UINT64_MAX/1000000) {
            uint64_t whole=(ticks/frequency)*1000000;
            uint64_t fraction=(ticks%frequency)*1000000/frequency;
            if(fraction<=UINT64_MAX-whole){microseconds=whole+fraction;ok=1;}
        }
    }
    if(!ok){atomic_store_explicit(&c->failed,1,memory_order_release);return 0;}
    *out=microseconds;
    return 1;
}
