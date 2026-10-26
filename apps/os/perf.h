#ifndef SUNSTONE_OS_PERF_H
#define SUNSTONE_OS_PERF_H
/* High and low performance modes in the integrated OS image
 * (docs/designs/performance-modes.md, docs/performance-modes.md). */
#include <stdint.h>
/* USB0 operation 8. The request's flags byte is the command: 0 report, 1 read
 * back, 2 switch. A switch names the mode in offset bits 9:8 (1 high, 2 low)
 * and the blocks in bits 4:0 (bit 0 CPU-L, 1 CPU-B, 3 GPU, 4 NPU) and the
 * session's epoch in the generation field. The answer is the report alone,
 * without the 64-byte frame header. */
#define OS_PERF_OP 8u
#define OS_PERF_REPORT_MAGIC UINT32_C(0x4d465250) /* "PRFM" */
#define OS_PERF_REPORT_BYTES 192u
#define OS_PERF_SELECTION_MASK UINT32_C(0x31f)
enum { OS_PERF_REPORT, OS_PERF_READ, OS_PERF_SWITCH };
/* USB0's answer to the request it carries. */
enum { OS_PERF_ANSWER_REPORT, OS_PERF_ANSWER_ACCEPTED, OS_PERF_ANSWER_BUSY, OS_PERF_ANSWER_STALE };
/* Little-endian, 192 bytes. answer and number are USB0's; every other field
 * is the thermal task's last publication. accepted and completed are command
 * numbers, which USB0 assigns from 1; the boot switch carries none. state is
 * 1 while a command or the boot switch runs. Each block's mhz and mv are the
 * last read-back programmed values; stopped marks a gated clock. */
struct os_perf_block { uint32_t target_mhz, target_mv, mhz, mv; };
struct os_perf_report {
    uint32_t magic, version, bytes, sequence;
    uint64_t epoch, generation;
    uint32_t answer, number, accepted, completed;
    uint32_t state, command, result, reason;
    uint32_t failed_block, boot_mode, mode, bin;
    uint32_t changes_left, owned, read_ok, stopped;
    struct os_perf_block block[5];
    uint32_t blocks, mode_requested, elapsed_us, reserved;
};
_Static_assert(sizeof(struct os_perf_report)==OS_PERF_REPORT_BYTES,"OS performance report geometry");
/* The image's boot mode (1 high, 2 low), which the link check records. */
extern const uint32_t os_perf_boot_mode;
struct a733_cpu_clock;
struct a733_thermal_policy;
/* Thermal task, in os_cpu_clock_step after each driver burst, with that
 * burst's result: one bounded service step. Returns 1 once the boot switch
 * has finished, 0 before, and -1 after publishing a terminal failure. The
 * caller must end the session on -1, preserving retained ownership. */
int os_perf_step(struct a733_cpu_clock *, const struct a733_thermal_policy *, int burst);
/* USB0 task: answers one operation 8 request that passed the framing rules. */
void os_perf_usb(unsigned command, uint32_t selection, uint64_t epoch, uint8_t out[OS_PERF_REPORT_BYTES]);
#endif
