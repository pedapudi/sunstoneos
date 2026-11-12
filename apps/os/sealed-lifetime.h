/* The sealed session's lifetime copy (apps/os/sealed.c), for C and assembly. The EL1 vectors
 * (apps/os/vectors.patch) and the EL0 fault admission (apps/os/cpu-return-el0.S) load the word at
 * os_sealed + OS_SEALED_LIFETIME_OFFSET; the EL3 fatal vector tests a733_smp_lifetime against the same value.
 * sealed.c checks both constants against the C definitions. */
#ifndef SUNSTONE_OS_SEALED_LIFETIME_H
#define SUNSTONE_OS_SEALED_LIFETIME_H
#define OS_SEALED_LIFETIME_OFFSET 4100
#define OS_LIFETIME_RETURN_WORD 3
#define OS_LIFETIME_RESIDENT_WORD 2
#endif
