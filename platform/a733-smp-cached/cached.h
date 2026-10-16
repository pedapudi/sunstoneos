#ifndef SUNSTONE_A733_CACHED_H
#define SUNSTONE_A733_CACHED_H
#include "boot.h"
#include "a733_cpu_coherence.h"
#include "a733_cpu_continue.h"
struct a733_cached_record {
    uint64_t epoch,core,state,error,redistributor,esr,far,elr;
    uint64_t sctlr3,ttbr3,clusteridr,startup_deadline,diagnostic_deadline,reserved[3];
};
struct a733_cached_runtime {
    uint64_t epoch,startup_deadline,diagnostic_deadline,redistributor;
    uint64_t reserved[12];
};
extern volatile struct a733_cached_record a733_cached_records[8];
extern struct a733_cached_runtime a733_cached_runtime[8];
extern uint64_t a733_cached_el3_l1[512],a733_cached_el1_l1[512];
extern unsigned char a733_cpu_el1_stacks[];
void a733_cached_prepare(uint64_t epoch,uint64_t startup,uint64_t diagnostic,uint64_t token);
void a733_cached_boot_check(void);
void a733_cached_block_walks(void);
int a733_cached_allow_ns_code(void);
_Noreturn void a733_cached_enter_el1(unsigned core,uint64_t epoch,uint64_t startup,uint64_t diagnostic,uint64_t rdist);
_Noreturn void a733_cached_el1(unsigned core,uint64_t epoch,uint64_t startup,uint64_t diagnostic,uint64_t rdist);
_Noreturn void a733_cached_smc(unsigned status,uint64_t detail);
#endif
