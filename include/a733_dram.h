#ifndef SUNSTONE_A733_DRAM_H
#define SUNSTONE_A733_DRAM_H
/* Memory-bus priority for display scan-out, the display port's latency
 * window and the DRAM decode check (docs/designs/dram-controller.md,
 * docs/designs/dram-bus-priority.md, docs/dram.md). Sources: pinned
 * sunxi-nsi.c, sunxi-nsi.h and sunxi-nsi-bw.c (gpu-isolation/drivers/bus),
 * the nsi0 node of the pinned sun60iw2p1.dtsi, and A733 manual section 4.1.6
 * for the CCU words. */
#include <stdatomic.h>
#include <stdint.h>
#include "a733_cpu_clock.h"

#define A733_NSI_BASE UINT32_C(0x02020000)
#define A733_NSI_PORT(n) (A733_NSI_BASE+UINT32_C(0x200)*(n))
#define A733_NSI_CLOCK UINT32_C(0x02002580)  /* NSI_CLK_REG: gate 31, reset 30. */
#define A733_NSI_CONFIG UINT32_C(0x02002584) /* NSI_BGR_REG: configuration gate 0, reset 16. */
#define A733_MBUS_CLOCK UINT32_C(0x02002588) /* MBUS_CLK_REG: dynamic frequency scaling 28. */
#define A733_DRAM_DE_PORT 2u
/* DE port 2, IOMMU0 port 14, IOMMU1 port 15, derived in "DE uses NSI port 2
 * and its IOMMU walkers use ports 14 and 15" of docs/designs/dram-controller.md. */
#define A733_DRAM_PORTS 3u

/* Register access as the framebuffer provides it: 1 on success. */
struct a733_dram_io {
    void *context;
    int (*read)(void *,uintptr_t,uint32_t *);
    int (*write)(void *,uintptr_t,uint32_t);
};

struct a733_dram_block;
/* Built only with SUNSTONE_DRAM_PRIORITY set to 1 (reads only) or 2 (table).
 * The arguments are not checked: the one caller passes the framebuffer's own
 * register pair and evidence block. The caller holds the register lock for
 * the whole call and runs it once per framebuffer instance, after the media
 * clock has verified the NSI and MBUS clock gates and before any master on
 * ports 2, 14 or 15 starts. The hook zeroes the block and fills its hook
 * fields and result bits. Two CCU reads first: a failed read stops the call
 * with failed set to A733_DRAM_CCU_FAILED, and an NSI reset asserted or gate
 * closed fails with failed 0 and no NSI access. Then, for each field, one
 * read; in mode 2 a field that differs from the table is written with the
 * other bits kept and read back under the field mask. The first failed
 * access or mismatch stops the call. Returns 1 on success; on failure the
 * caller takes its retain path. */
int a733_dram_priority(const struct a733_dram_io *,struct a733_dram_block *);
/* The words the hook may access: CCU +0x580 and +0x584 and the +0x10, +0x14
 * and +0x18 words of ports 2, 14 and 15. */
int a733_dram_address(uintptr_t);

/* The display port's latency window, built only with
 * SUNSTONE_GPU_DISPLAY_DRAM_LOAD. Every time is an architectural counter
 * value. The callbacks belong to the framebuffer's load accessor: enter
 * returns 1 with the register lock held, 0 when busy (nothing changes and
 * the next step retries) and any other value on failure. counter is ISB
 * followed by MRS CNTPCT_EL0. The window adds no barrier: the accessor ends
 * every access with DSB SY, which completes the last NSI access of a phase
 * before the counter read that ends it. Every counter sample must not be
 * earlier than the one before it (a733_monotonic_advance); a sample that is
 * refuses the window in flight as late. */
struct a733_dram_window_ops {
    struct a733_dram_io io;
    int (*enter)(void *);
    void (*leave)(void *);
    uint64_t (*counter)(void *);
};
enum a733_dram_phase { A733_DRAM_IDLE, A733_DRAM_PHASE_A, A733_DRAM_PHASE_B, A733_DRAM_ENDED };
/* Counts, in block order. */
enum { A733_DRAM_STARTED, A733_DRAM_ACCEPTED_A, A733_DRAM_ACCEPTED_B, A733_DRAM_LATE,
    A733_DRAM_IDLE_REFUSED, A733_DRAM_FREE_RUNNING, A733_DRAM_OUT_OF_RANGE,
    A733_DRAM_ABANDONED, A733_DRAM_COUNTS };
/* Step results: the output task wakes the copy tasks on BEGIN_B and they
 * stop on ENDED. */
#define A733_DRAM_BEGIN_B 1
#define A733_DRAM_END 2
struct a733_dram_window {
    struct a733_dram_window_ops ops;
    uint64_t period, phase_a, end, counter_hz; /* Counter ticks; end is absolute. */
    uint64_t t0, t1, phase_start, b_start, b_end, last; /* last: latest counter sample. */
    uint64_t sum[2], square[2], data[2]; /* Per phase: sum of x, of x squared, read data units. */
    uint32_t cycles, fabric_hz, enable;
    uint16_t count[A733_DRAM_COUNTS], mhz[A733_CPU_DOMAINS];
    uint16_t ticks_max; /* Longest start or collect in counter ticks, saturating. */
    unsigned initialized, in_flight, fallback, early, scaling; /* scaling: MBUS_DFS_EN was set. */
    atomic_uint phase; /* Written by the output task only, read by the copy tasks. */
};
/* Zeroed storage once. P is 50 ms: counter_hz and fabric_hz (the MBUS rate
 * that the media clock verified, at most 600 MHz) must be multiples of 20.
 * end is the counter value at which phase B ends, stop_at minus 1 s. The
 * report is the CPU clock report read once through the load accessor;
 * undecoded domains record 0 MHz. */
int a733_dram_window_init(struct a733_dram_window *,const struct a733_dram_window_ops *,
    uint64_t counter_hz,uint32_t fabric_hz,uint64_t end,const struct a733_cpu_clock_report *);
/* Output task, once per loop pass. eligible is display stage 3, application
 * state 1 and three presentations done. Phase A starts with the first
 * eligible step, which also starts a window, and lasts 2 s; phase B runs
 * until end. Phases change only between windows. The first start reads
 * MBUS_CLK_REG before its six accesses: with dynamic frequency scaling
 * enabled the MBUS rate can change, so P in MBUS cycles would not be 50 ms;
 * the step then sets scaling, leaves, ends the phases early and returns
 * A733_DRAM_END. The step that begins phase B and the step that ends the
 * phases start no window and make no register access. Returns a negative
 * value on a failed access or a failed enter, with the lock still held after
 * a failed access, for the caller's retain path. The window then ends the
 * phases (release), and every later step and abandon returns at once with no
 * enter, access or counter read. Otherwise returns 0 or one of
 * A733_DRAM_BEGIN_B and A733_DRAM_END.
 * Once eligibility is lost the window in flight is abandoned and the phases
 * end. The span of each start (t1 - t0) and collect (t_end minus the step's
 * pending sample), which covers enter and every access but not leave, goes
 * into ticks_max. */
int a733_dram_window_step(struct a733_dram_window *,unsigned eligible);
/* At stop_at and on the output fault path: no register access. Ending the
 * phases here marks them ended early; during phase B one counter read
 * records the end of phase B. After the phases have ended, including after
 * a failure, it does nothing. */
void a733_dram_window_abandon(struct a733_dram_window *);
/* Counter words +0xc0..+0xdc of port 2, and CCU +0x588 for reading. */
int a733_dram_window_address(uintptr_t);

/* One copy task's pass counter, in its own cache line. */
struct a733_dram_copy_slot { _Alignas(64) atomic_uint passes; };
#define A733_DRAM_COPY_BYTES (UINT32_C(2)<<20)
/* Copy task, once per pass: loads the phase and *state with acquire. Returns
 * 1 after copying 2 MiB from source to destination, 0 when the phase has
 * ended or the state is not 1 (the task suspends), and -1 before phase B
 * (the task waits for its notification). */
int a733_dram_copy_pass(struct a733_dram_window *,const atomic_uint *state,
    struct a733_dram_copy_slot *,void *destination,const void *source);

/* The 136-byte evidence block of the gpu-display observation reply, in the
 * little-endian layout of docs/designs/dram-controller.md. Every member is
 * naturally aligned, so the structure has no padding. */
#define A733_DRAM_HOOK_BUILT 1u
#define A733_DRAM_TABLE_IMAGE 2u
#define A733_DRAM_HOOK_PASSED 4u
#define A733_DRAM_DISABLE_FIRST 8u
#define A733_DRAM_ENDED_EARLY 16u
#define A733_DRAM_MBUS_SCALING 32u /* The first start found MBUS_DFS_EN set. */
#define A733_DRAM_CCU_FAILED 10u   /* failed: a CCU read of the hook failed. */
struct a733_dram_block {
    uint16_t count[A733_DRAM_COUNTS];
    uint16_t result, failed; /* Result bits; 0, 1 + 3 * port index + field index, or A733_DRAM_CCU_FAILED. */
    uint16_t inherited[A733_DRAM_PORTS], final[A733_DRAM_PORTS];
    uint16_t mhz[A733_CPU_DOMAINS], phase_ticks_max;
    uint32_t nsi_clock, nsi_config, fabric_hz, phase_b_ms;
    uint64_t phase[2][3]; /* Phase A, then B: sum of x, sum of x squared, read data units. */
    uint32_t passes[8];
};
_Static_assert(sizeof(struct a733_dram_block)==136,"evidence block size");
/* Load images only, after a733_dram_priority: the window and copy-slot fields. */
void a733_dram_window_block(const struct a733_dram_window *,const struct a733_dram_copy_slot slots[8],
    struct a733_dram_block *);

/* EL3, before memory.c maps 0x8000_0000-0xFFFF_FFFF as write-back (layout
 * 2). Returns 1 only when this boot's controller configuration shows a linear
 * decode to 0x3_4000_0000 with no protected region in that range. No pinned
 * source documents the DRAMC, SMC0 or SMC1 registers, so it makes no access
 * and returns 0, which keeps stage 2 refused (docs/designs/dram-bus-priority.md). */
int a733_dram_decode_check(void);
#endif
