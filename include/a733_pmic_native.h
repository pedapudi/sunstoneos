#ifndef SUNSTONE_A733_PMIC_NATIVE_H
#define SUNSTONE_A733_PMIC_NATIVE_H
#include "a733_pmic.h"
struct a733_platform_locks;
struct a733_pmic_native {
    struct a733_pmic_ops operations;
    struct a733_platform_locks *locks;
    void *task;
    unsigned initialized;
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
