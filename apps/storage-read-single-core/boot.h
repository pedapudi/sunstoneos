#ifndef SUNSTONE_SD_SINGLE_CORE_BOOT_H
#define SUNSTONE_SD_SINGLE_CORE_BOOT_H
/* Preserve the admitted storage wire ABI; these names do not imply SMP. */
#include "../../platform/a733-smp-cached/boot.h"
#define GICD_BASE 0x03400000UL
#define GICR_BASE 0x03460000UL
#define GICR_SGI (GICR_BASE + 0x10000UL)
/* Separate the reused single-core platform log from the storage report. */
#define RESULT_BASE 0x42020000UL
#define RESULT_SIZE 16384U
#define LOG_OFFSET 256U
#define RESULT_MAGIC 0x534b4552UL
#define TICK_IRQ 30U
#define RECOVERY_IRQ 29U
#define COUNTER_HZ 24000000UL
struct boot_result {
 uint64_t magic,stage,status,log_length,esr,elr,far,interrupt;
 uint64_t start_counter,end_counter,mode,gicd_ctlr,gicd_typer,gicr_typer,priority_mask,reserved;
};
#define RESULT ((volatile struct boot_result *)RESULT_BASE)
static inline uint32_t mmio_read(uintptr_t a){return *(volatile uint32_t *)a;}
static inline void mmio_write(uintptr_t a,uint32_t v){*(volatile uint32_t *)a=v;}
static inline uint64_t counter(void){uint64_t v;__asm__ volatile("isb\n mrs %0,cntpct_el0":"=r"(v)::"memory");return v;}
int sd_single_core_secondaries_admitted(void);
void sd_single_core_monitor_init(void);
_Noreturn void sd_single_core_exception(uint64_t,uint64_t,uint64_t);
#endif
