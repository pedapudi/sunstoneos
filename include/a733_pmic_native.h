#ifndef SUNSTONE_A733_PMIC_NATIVE_H
#define SUNSTONE_A733_PMIC_NATIVE_H
#include "a733_pmic.h"
struct a733_platform_locks;
/* Division of the counter by its invariant ticks per microsecond without a
 * UDIV instruction (Granlund and Montgomery, "Division by invariant integers
 * using multiplication", PLDI 1994, figure 4.1, with N = 64). For a divisor d
 * from 1 to 2^32, l = ceil(log2 d) and m = floor(2^64 (2^l - d) / d) + 1; then
 * n / d = (t + ((n - t) >> min(l, 1))) >> max(l - 1, 0), with t the high 64
 * bits of m n. checks/a733-pmic-time.c compares it with division. */
struct a733_pmic_divisor { uint64_t m; unsigned s1, s2; };
static inline int a733_pmic_divisor_init(uint64_t d,struct a733_pmic_divisor *out)
{
    if(!d || d>(UINT64_C(1)<<32))return 0;
    unsigned l=0;while((UINT64_C(1)<<l)<d)++l;
    uint64_t r=(UINT64_C(1)<<l)-d,q=0; /* r < d: long division of r * 2^64 by d */
    for(unsigned i=0;i<64;++i){r<<=1;q<<=1;if(r>=d){r-=d;q|=1;}}
    *out=(struct a733_pmic_divisor){q+1,l ? 1u : 0u,l ? l-1 : 0u};
    return 1;
}
static inline uint64_t a733_pmic_divide(uint64_t n,const struct a733_pmic_divisor *d)
{
    const uint64_t t=(uint64_t)(((unsigned __int128)d->m*n)>>64);
    return (t+((n-t)>>d->s1))>>d->s2;
}
struct a733_pmic_native {
    struct a733_pmic_ops operations;
    struct a733_platform_locks *locks;
    void *task;
    unsigned initialized, holding; /* holding: the provider task entered the I2C mutex. */
    /* Ticks per microsecond of the counter clock, set by the first time read
     * on the provider task, the only caller. */
    struct a733_pmic_divisor ticks_per_us;
};
/* Pure allowlist: only legacy TWI DATA/CNTR can be written. All engine, clock,
 * reset, pin, IRQ and autonomous PMU controls are observations only. */
static inline int a733_pmic_native_address(uintptr_t a,unsigned writing)
{
    if(a&3)return 0;
    if(writing)return a==0x07083008u || a==0x0708300cu;
    switch(a) {
    case 0x02002000: case 0x07090160: case 0x0701000c: case 0x07010010: case 0x0701019c:
    case 0x07025000: case 0x03400000: case 0x0340011c:
    case 0x0340021c: case 0x0340031c:
    case 0x07083008: case 0x0708300c: case 0x07083010: case 0x07083014:
    case 0x07083018: case 0x0708301c: case 0x07083020:
    case 0x07083200: case 0x07083210: case 0x07083214: case 0x07083218:
    case 0x0708321c: case 0x07083314: case 0x07083318: case 0x0708331c:return 1;
    default:return 0;
    }
}
/* Initialize before scheduling; real provider task and shared locks persist.
 * No callback substitutes readiness, rail authority or bus transactions.
 * Subsequent I/O requires EL1 task context and actual I2C mutex ownership.
 */
int a733_pmic_native_init(struct a733_pmic_native *,struct a733_platform_locks *,void *task);
#endif
