#include "a733_counter_clock.h"

int a733_counter_clock_init(struct a733_counter_clock *c,uint32_t expected)
{
    if (!c || c->initialized || !expected) return 0;
    uint64_t frequency,level,ticks;
    __asm__ volatile("mrs %0, CurrentEL\n mrs %1, cntfrq_el0\n isb\n mrs %2, cntpct_el0"
        : "=r"(level),"=r"(frequency),"=r"(ticks) :: "memory");
    if (level!=4 || frequency!=expected) return 0;
    atomic_init(&c->busy,0);atomic_init(&c->failed,0);
    c->frequency=expected;c->last_ticks=ticks;c->initialized=1;return 1;
}
int a733_counter_clock_now(void *cookie,uint64_t *out)
{
    struct a733_counter_clock *c=cookie;
    if (!c || !c->initialized || !out || atomic_load_explicit(&c->failed,memory_order_acquire)) return 0;
    uint64_t flags,ticks,frequency;
    __asm__ volatile("mrs %0, daif\n msr daifset, #15" : "=r"(flags) :: "memory");
    unsigned acquired=0;
    for(unsigned attempt=0;attempt<256;++attempt) {
        unsigned expected=0;
        if(atomic_compare_exchange_strong_explicit(&c->busy,&expected,1,memory_order_acquire,memory_order_relaxed)) {
            acquired=1;break;
        }
        __asm__ volatile("yield" ::: "memory");
    }
    int ok=0;uint64_t microseconds=0;
    if(acquired) {
        __asm__ volatile("mrs %0, cntfrq_el0\n isb\n mrs %1, cntpct_el0"
            : "=r"(frequency),"=r"(ticks) :: "memory");
        if(frequency==c->frequency && ticks>=c->last_ticks &&
            ticks/c->frequency<=UINT64_MAX/1000000 &&
            !atomic_load_explicit(&c->failed,memory_order_acquire)) {
            uint64_t whole=(ticks/c->frequency)*1000000;
            uint64_t fraction=(ticks%c->frequency)*1000000/c->frequency;
            if(fraction<=UINT64_MAX-whole) {
                c->last_ticks=ticks;microseconds=whole+fraction;ok=1;
            }
        }
        if(!ok) atomic_store_explicit(&c->failed,1,memory_order_release);
        atomic_store_explicit(&c->busy,0,memory_order_release);
    } else atomic_store_explicit(&c->failed,1,memory_order_release);
    __asm__ volatile("msr daif, %0" :: "r"(flags) : "memory");
    if(ok) *out=microseconds;
    return ok;
}
