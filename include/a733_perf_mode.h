#ifndef SUNSTONE_A733_PERF_MODE_H
#define SUNSTONE_A733_PERF_MODE_H
#include <stdint.h>
/* High and low performance modes (docs/designs/performance-modes.md).
 *
 * A mode names one operating point per block: CPU-L (the six Cortex-A55
 * cores, DCDC5), CPU-B (the two Cortex-A76 cores, DCDC3), the DSU (DCDC5),
 * the GPU (DCDC4) and the NPU (DCDC2). High is the vendor's top point for the
 * speed bin with the vendor's voltage; low is the fastest vendor point at the
 * lowest voltage of the bin's column. The DSU is never selected: its point
 * follows the clusters by the vendor dsufreq rule.
 *
 * The switch engine below is pure: it plans, orders and records, and every
 * hardware access goes through the owner's act callback. */
enum a733_perf_block { A733_PERF_CPU_L, A733_PERF_CPU_B, A733_PERF_DSU, A733_PERF_GPU, A733_PERF_NPU,
    A733_PERF_BLOCKS };
#define A733_PERF_BIT(b) (1u << (b))
#define A733_PERF_CPU (A733_PERF_BIT(A733_PERF_CPU_L) | A733_PERF_BIT(A733_PERF_CPU_B))
/* Blocks a request may name; the DSU follows CPU-L and CPU-B. */
#define A733_PERF_SELECTABLE (A733_PERF_CPU | A733_PERF_BIT(A733_PERF_GPU) | A733_PERF_BIT(A733_PERF_NPU))
#define A733_PERF_ALL ((1u << A733_PERF_BLOCKS) - 1u)
enum a733_perf_mode { A733_PERF_UNKNOWN, A733_PERF_HIGH, A733_PERF_LOW, A733_PERF_MIXED };
struct a733_perf_point { uint32_t mhz, mv; };

/* One speed-bin column: its CPU clock driver column index (vf0000, vf0100,
 * vf0200, vf0300, vf0400, vf0500, vf0600, vf0401, vf0501 are 0 to 8), its
 * name for reports (0x0400 for vf0400) and the high and low point of every
 * block, indexed [mode - A733_PERF_HIGH][block]. The DSU points must equal
 * the rule's result for the clusters' points. */
struct a733_perf_entry {
    unsigned column;
    uint16_t name;
    struct a733_perf_point point[2][A733_PERF_BLOCKS];
};
/* The entry for the SID word 0x0300624c, or 0 for a bin without one
 * (including code 0, an unprogrammed fuse). Pure. */
const struct a733_perf_entry *a733_perf_entry(uint32_t sid_word);
/* Readback decoders, pure. A PLL word gives 24 MHz x N / (M1 x P0), with N
 * bits 15:8, P0 bits 22:20 and M1 bit 1 each plus one, when PLL_REF decodes
 * to 24 MHz and the PLL is enabled, locked and open (bits 31:27); 0
 * otherwise. The GPU clock word (CCU +0xb20, ccu-sun60iw2.c:1020-1043)
 * selects PLL_GPU0 or a PERI0 output (800, 600, 400, 300, 200 MHz) in bits
 * 26:24 and divides by the table 0 -> 1, 8 -> 2, 12 -> 4 in bits 3:0. The
 * NPU clock word (+0xb00) divides PLL_NPU (selector 0) by bits 4:0 plus one.
 * Each returns 1 with the rate and whether the clock is stopped (gate bit
 * 31 clear, or no source), or 0 for a word it cannot decode. */
uint32_t a733_perf_pll_mhz(uint32_t oscillator, uint32_t reference, uint32_t pll);
int a733_perf_gpu_mhz(uint32_t oscillator, uint32_t reference, uint32_t peri0, uint32_t pll, uint32_t clock,
    uint32_t *mhz, unsigned *stopped);
int a733_perf_npu_mhz(uint32_t oscillator, uint32_t reference, uint32_t pll, uint32_t clock, uint32_t *mhz,
    unsigned *stopped);
/* All entries, for checks. */
const struct a733_perf_entry *a733_perf_entries(unsigned *count);
/* The vendor dsufreq rule: the smaller of three quarters of the faster
 * cluster, rounded down to 26 MHz, and the fastest DSU row whose voltage
 * equals that of the CPU-L row at l_mhz (or else the fastest row at the
 * column's lowest voltage, which is also the floor while CPU-B runs at
 * 1488 MHz or more). 1 and the point, or 0 when CPU-L is not a row of the
 * column or the result is not a DSU row. Pure. */
int a733_perf_dsu(unsigned column, uint32_t l_mhz, uint32_t b_mhz, struct a733_perf_point *dsu);

/* Actions the owner performs. READ fills the point read back from the
 * hardware (a programmed rate and voltage). HOLD completes once the
 * accelerator can change (for the NPU: once its session has stopped) and
 * keeps it so; RELEASE ends a hold and cannot fail. CPU changes one CPU domain through the
 * CPU clock driver, which orders voltage and frequency itself. VOLTAGE and
 * RATE change an accelerator's supply and clock. */
enum a733_perf_action { A733_PERF_READ, A733_PERF_HOLD, A733_PERF_RELEASE, A733_PERF_CHANGE_CPU,
    A733_PERF_VOLTAGE, A733_PERF_RATE };
/* PENDING: started or still running. DONE. REFUSED: nothing was written.
 * EXPIRED: the deadline passed; the block is at its old point, or for a CPU
 * domain at its old rate with at most a raised voltage that covers it.
 * FAILED: the block's state is not known and its owner retains it. */
enum a733_perf_status { A733_PERF_PENDING, A733_PERF_DONE, A733_PERF_REFUSED, A733_PERF_EXPIRED,
    A733_PERF_FAILED };
/* Why a request was refused or a switch stopped. */
enum a733_perf_reason {
    A733_PERF_OK,
    A733_PERF_ACTIVE,      /* A command is already running. */
    A733_PERF_NO_BIN,      /* No table entry for the speed bin. */
    A733_PERF_REQUEST,     /* Unknown mode, an empty selection or the DSU named. */
    A733_PERF_NOT_OWNED,   /* A named block is not owned by this image. */
    A733_PERF_THERMAL,     /* The thermal level is not NORMAL with every sensor read. */
    A733_PERF_NOT_READY,   /* An owner has not started or has stopped. */
    A733_PERF_BUDGET,      /* The CPU grant lacks the changes the switch needs. */
    A733_PERF_RULE,        /* The plan would break the DSU rule. */
    A733_PERF_BUSY,        /* An accelerator job did not finish within the hold bound. */
    A733_PERF_STEP_REFUSED, A733_PERF_STEP_EXPIRED, A733_PERF_STEP_FAILED,
    A733_PERF_READ_FAILED  /* A block could not be read back. */
};
enum a733_perf_command { A733_PERF_COMMAND_NONE, A733_PERF_COMMAND_READ, A733_PERF_COMMAND_SWITCH };
struct a733_perf_ops {
    int (*now)(void *, uint64_t *);
    /* Owner facts before any access: A733_PERF_OK, or the reason the owner
     * refuses a switch of these blocks that needs cpu_changes CPU changes. */
    enum a733_perf_reason (*admit)(void *, unsigned blocks, unsigned cpu_changes);
    enum a733_perf_status (*act)(void *, enum a733_perf_action, enum a733_perf_block, uint32_t value,
        uint64_t deadline, int start, struct a733_perf_point *read);
};
/* Deadlines of one action, microseconds. HOLD covers every accelerator held
 * together and the NPU's 1 s submit limit. */
#define A733_PERF_READ_US 150000u
#define A733_PERF_HOLD_US 1500000u
#define A733_PERF_CPU_US 120000u
#define A733_PERF_ACCEL_US 50000u
#define A733_PERF_STEPS 16u
struct a733_perf_step { uint8_t action, block; uint16_t value; };
/* The present switch's record. read holds the last read-back points; ok the
 * blocks read in the last pass. */
struct a733_perf_record {
    uint32_t command, mode_requested, blocks, result, reason, failed_block, mode;
    uint32_t owned, read_ok;
    struct a733_perf_point target[A733_PERF_BLOCKS], read[A733_PERF_BLOCKS];
    uint64_t started_us, finished_us;
};
enum a733_perf_result { A733_PERF_RESULT_NONE, A733_PERF_RESULT_RUNNING, A733_PERF_RESULT_DONE,
    A733_PERF_RESULT_REFUSED, A733_PERF_RESULT_STOPPED };
struct a733_perf {
    const struct a733_perf_entry *entry;
    struct a733_perf_ops ops;
    void *cookie;
    unsigned owned, phase, index, count, held, start, block;
    uint64_t deadline;
    struct a733_perf_step plan[A733_PERF_STEPS];
    struct a733_perf_record record;
};
/* Pure. owned: the blocks this image changes (the DSU follows CPU-L). Every
 * block is read back whether owned or not. 0 for a bin without an entry; the
 * engine then refuses every switch and still reads back. */
int a733_perf_init(struct a733_perf *, uint32_t sid_word, unsigned owned, const struct a733_perf_ops *,
    void *cookie);
/* Owner task. Starts a READ, or a SWITCH of the selected blocks to a mode,
 * when no command runs. Returns A733_PERF_OK, or the reason the request is
 * refused before any access; a refusal is also recorded. */
enum a733_perf_reason a733_perf_request(struct a733_perf *, enum a733_perf_command, enum a733_perf_mode,
    unsigned blocks);
/* Owner task: advances the running command by its actions until one is
 * pending, at most A733_PERF_STEPS actions per call. Returns the record's
 * result: RUNNING while the command runs, then DONE, REFUSED or STOPPED. */
enum a733_perf_result a733_perf_step(struct a733_perf *);
/* The mode that read-back points show for the owned blocks. Pure. */
enum a733_perf_mode a733_perf_mode_of(const struct a733_perf_entry *, unsigned owned, unsigned read_ok,
    const struct a733_perf_point read[A733_PERF_BLOCKS]);
/* Pure planner: the ordered actions that move the blocks in `change` from
 * `from` to `to`. A hold of every changing accelerator first; then each
 * accelerator's change (voltage before rate when raising, after it when
 * lowering) and its release; then the DSU when it lowers and its present
 * clusters admit the target, the clusters that lower (CPU-L, CPU-B), the
 * clusters that raise (CPU-B, CPU-L), and the DSU when it raises or its
 * lowering had to wait. Returns the count, or -1 when the DSU's target is
 * above three quarters of the faster cluster it would then serve. */
int a733_perf_plan(const struct a733_perf_entry *, unsigned change, const struct a733_perf_point from[A733_PERF_BLOCKS],
    const struct a733_perf_point to[A733_PERF_BLOCKS], struct a733_perf_step out[A733_PERF_STEPS]);
#endif
