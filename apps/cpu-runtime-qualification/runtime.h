#ifndef A733_RUNTIME_QUALIFICATION_H
#define A733_RUNTIME_QUALIFICATION_H
#define QUAL_MAGIC 0x3154525133333741
#define QUAL_CORES 8
#define QUAL_SYS_WORDS 64
#define QUAL_CORE_BYTES 1024
#define QUAL_SYSTEM_OFFSET 128
#define QUAL_LOCAL_OFFSET 640
#define QUAL_STACK_BYTES 4096
#ifndef __ASSEMBLER__
#include <stdint.h>
#include <stddef.h>
struct qualification_core {
    uint64_t epoch, captured, cleaned, restored, request, acknowledged, error, reserved;
    uint64_t rounds, background, sgis, failure, ticks, drained_epoch, reserved_result[2];
    uint64_t system[QUAL_SYS_WORDS];
    uint64_t local[16];
    uint64_t off_observed, padding[31];
};
_Static_assert(sizeof(struct qualification_core)==QUAL_CORE_BYTES,"qualification core geometry");
_Static_assert(offsetof(struct qualification_core,system)==QUAL_SYSTEM_OFFSET,"system offset");
_Static_assert(offsetof(struct qualification_core,local)==QUAL_LOCAL_OFFSET,"local offset");
struct qualification_report {
    uint64_t magic,version,bytes,epoch,deadline,state,error,core;
    uint64_t distributor,banks,enable[32];
    uint64_t power[8][5];
    uint64_t entry_low,entry_high,rom_entry,reserved;
    struct qualification_core cpu[QUAL_CORES];
};
_Static_assert(offsetof(struct qualification_report,cpu)==688 && sizeof(struct qualification_report)==8880,"report geometry");
extern volatile struct qualification_report a733_qualification_report;
extern unsigned char a733_qualification_stacks[QUAL_CORES][QUAL_STACK_BYTES];
int a733_qualification_boot(uint64_t epoch,uint64_t deadline);
int a733_qualification_local(unsigned core,uint64_t epoch);
_Noreturn void a733_qualification_worker_done(unsigned core);
_Noreturn void a733_qualification_terminal_el0(unsigned core);
_Noreturn void a733_qualification_captured(unsigned core);
void a733_qualification_system_save(uint64_t *out);
_Noreturn void a733_qualification_system_restore(unsigned core,const uint64_t *saved);
_Noreturn void a733_qualification_off(unsigned core);
_Noreturn void a733_qualification_rom(void);
#endif
#endif
