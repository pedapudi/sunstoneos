/* Model result block: same layout as platform/a733/boot.h in model memory. */
#ifndef MODEL_BOOT_H
#define MODEL_BOOT_H
#include <stdint.h>
extern uint8_t model_result[];
#define RESULT_BASE ((uintptr_t)model_result)
#define RESULT_SIZE 16384U
#define LOG_OFFSET 256U
#define COUNTER_HZ 24000000UL
struct boot_result {
    uint64_t magic, stage, status, log_length;
    uint64_t esr, elr, far, interrupt;
    uint64_t start_counter, end_counter, mode;
    uint64_t gicd_ctlr, gicd_typer, gicr_typer, priority_mask, reserved;
};
#define RESULT ((volatile struct boot_result *)RESULT_BASE)
uint64_t counter(void);
#endif
