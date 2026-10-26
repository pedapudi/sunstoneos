#ifndef SUNSTONE_A733_PLL_LOCK_H
#define SUNSTONE_A733_PLL_LOCK_H
#include <stdint.h>

/* LOCK qualification of an A733 PLL, shared by the CPU clock, the NPU startup
 * and the video decoder. The A733 User Manual V1.00 (4.1.4.2) and the board
 * kernel's lock helper with CONFIG_AW_STANDARD_CCU (ccu_common.c
 * ccu_helper_wait_for_lock, lines 47-64 at linux-orangepi 2ac08e8c) accept a
 * PLL once three reads in a row show LOCK, each read 3 us after the one
 * before, at most 100 reads; the PLL may drive a clock 20 us later. The helper
 * is header-only so that every image that builds one of the three drivers
 * builds it without another source file.
 *
 * Times are whole microseconds. A reading t means some instant in [t, t+1),
 * so a gap of g microseconds is guaranteed only from t+g+1 on. */
#define A733_PLL_LOCK_READS 3u
#define A733_PLL_LOCK_GAP_US 3u
#define A733_PLL_LOCK_SETTLE_US 20u
#define A733_PLL_LOCK_MAX_READS 100u
#define A733_PLL_LOCK_BIT (UINT32_C(1)<<28)

struct a733_pll_lock { uint64_t counted_at; unsigned locked; };

/* Account one read of the control word, timed after the read completed. An
 * unlocked read restarts the count. A locked read counts when it is the first
 * of a run or comes at least the gap after the last counted read; an earlier
 * locked read is ignored, which lets a caller that polls faster than the gap
 * use the same rule. Returns 1 once three reads in a row have counted. */
static inline int a733_pll_lock_count(struct a733_pll_lock *l,uint64_t read_us,int locked)
{
    if(!locked){l->locked=0;return 0;}
    if(l->locked && read_us<l->counted_at+A733_PLL_LOCK_GAP_US+1u)return 0;
    l->counted_at=read_us;
    return ++l->locked>=A733_PLL_LOCK_READS;
}
/* The first whole microsecond at which the PLL may drive a clock. */
static inline uint64_t a733_pll_lock_settled(const struct a733_pll_lock *l)
{return l->counted_at+A733_PLL_LOCK_SETTLE_US+1u;}

/* The wait for callers that spin: every read at least the gap after the
 * previous one, whatever it showed, as the kernel helper spaces its reads.
 * read returns 1 with the control word, now 1 with the time; either returning
 * anything else ends the wait, which is how callers impose a deadline. Every
 * read must equal word apart from LOCK, so another writer or a changed field
 * ends the wait. Returns 1 locked and settled, 0 otherwise. */
static inline int a733_pll_lock_wait(int (*read)(void *,uint32_t *),int (*now)(void *,uint64_t *),
    void *cookie,uint32_t word)
{
    struct a733_pll_lock l={0,0};
    uint64_t t,next=0;uint32_t v;
    for(unsigned reads=0;reads<A733_PLL_LOCK_MAX_READS;++reads) {
        do if(now(cookie,&t)!=1)return 0; while(t<next);
        if(read(cookie,&v)!=1 || (v&~A733_PLL_LOCK_BIT)!=word || now(cookie,&t)!=1)return 0;
        next=t+A733_PLL_LOCK_GAP_US+1u;
        if(a733_pll_lock_count(&l,t,(v&A733_PLL_LOCK_BIT)!=0)) {
            do if(now(cookie,&t)!=1)return 0; while(t<a733_pll_lock_settled(&l));
            return 1;
        }
    }
    return 0;
}
#endif
