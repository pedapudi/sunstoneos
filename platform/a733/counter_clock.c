#include "a733_counter_clock.h"
#include <stddef.h>

_Static_assert(offsetof(struct a733_counter_clock,last_ticks)>=offsetof(struct a733_counter_clock,failed)+64,
    "history store must not share the waiters' cache line");
#ifdef SUNSTONE_COUNTER_MODEL
/* Host contention model: exception level, masking and counter reads. */
extern uint64_t a733_counter_model_level(void);
extern uint64_t a733_counter_model_mask(void);
extern void a733_counter_model_restore(uint64_t);
extern uint64_t a733_counter_model_frequency(void);
extern uint64_t a733_counter_model_ticks(void);
extern void a733_counter_model_relax(void);
static uint64_t level(void){return a733_counter_model_level();}
static uint64_t mask(void){return a733_counter_model_mask();}
static void restore(uint64_t flags){a733_counter_model_restore(flags);}
static uint64_t frequency_now(void){return a733_counter_model_frequency();}
static uint64_t ticks_now(void){return a733_counter_model_ticks();}
static void relax(void){a733_counter_model_relax();}
#else
static uint64_t level(void){uint64_t v;__asm__ volatile("mrs %0, CurrentEL" : "=r"(v));return v;}
static uint64_t mask(void)
{uint64_t flags;__asm__ volatile("mrs %0, daif\n msr daifset, #15" : "=r"(flags) :: "memory");return flags;}
static void restore(uint64_t flags){__asm__ volatile("msr daif, %0" :: "r"(flags) : "memory");}
static uint64_t frequency_now(void){uint64_t v;__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v) :: "memory");return v;}
static uint64_t ticks_now(void){uint64_t v;__asm__ volatile("isb\n mrs %0, cntpct_el0" : "=r"(v) :: "memory");return v;}
static void relax(void){__asm__ volatile("yield" ::: "memory");}
#endif

int a733_counter_clock_init(struct a733_counter_clock *c,uint32_t expected)
{
    if (!c || c->initialized || !expected || level()!=4) return 0;
    uint64_t frequency=frequency_now(),ticks=ticks_now();
    if (frequency!=expected) return 0;
    atomic_init(&c->next,0);atomic_init(&c->serving,0);atomic_init(&c->failed,0);
    c->frequency=expected;c->last_ticks=ticks;c->initialized=1;return 1;
}
int a733_counter_clock_now(void *cookie,uint64_t *out)
{
    struct a733_counter_clock *c=cookie;
    if (!c || !c->initialized || !out || atomic_load_explicit(&c->failed,memory_order_acquire)) return 0;
    uint64_t flags=mask(),ticks,frequency;
    /* A ticket orders waiters, so a slower core cannot lose every round to
     * faster ones. The wait is bounded by counter time, not by attempts. Each
     * holder masks interrupts for a few instructions, so 100 microseconds is
     * far beyond the wait behind seven live holders. A backward counter
     * sample also ends the wait, because the unsigned difference is huge.
     * An abandoned ticket faults the object, which releases later waiters. */
    const uint64_t limit=c->frequency/10000u+1u;
    const unsigned ticket=atomic_fetch_add_explicit(&c->next,1,memory_order_relaxed);
    uint64_t start=0;unsigned acquired=0;
    for(unsigned waited=0;;waited=1) {
        if(atomic_load_explicit(&c->serving,memory_order_acquire)==ticket){acquired=1;break;}
        if(atomic_load_explicit(&c->failed,memory_order_acquire))break;
        relax();ticks=ticks_now();
        if(!waited)start=ticks;
        else if(ticks-start>=limit)break;
    }
    int ok=0;uint64_t microseconds=0;
    if(acquired) {
        frequency=frequency_now();ticks=ticks_now();
        if(frequency==c->frequency && ticks>=c->last_ticks &&
            !atomic_load_explicit(&c->failed,memory_order_acquire)) {
            /* The native 24 MHz rate converts with a constant divisor and
             * cannot overflow. Other admitted rates keep the checked form. */
            if(frequency==24000000u) {
                microseconds=ticks/24u;ok=1;
            } else if(ticks/frequency<=UINT64_MAX/1000000) {
                uint64_t whole=(ticks/frequency)*1000000;
                uint64_t fraction=(ticks%frequency)*1000000/frequency;
                if(fraction<=UINT64_MAX-whole){microseconds=whole+fraction;ok=1;}
            }
            if(ok) c->last_ticks=ticks;
        }
        if(!ok) atomic_store_explicit(&c->failed,1,memory_order_release);
        atomic_store_explicit(&c->serving,ticket+1u,memory_order_release);
    } else atomic_store_explicit(&c->failed,1,memory_order_release);
    restore(flags);
    if(ok) *out=microseconds;
    return ok;
}
