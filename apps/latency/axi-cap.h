/* SPDX-License-Identifier: GPL-2.0-only */
/* The AXI2HIF bandwidth cap of stage 3a (docs/designs/concurrent-accel.md, "The AXI2HIF limiter in the vendor
 * driver"): a static total-bandwidth limit on both CPU bridges, set before each capped busy sub-phase and cleared
 * after it. One EL1 task on CPU0, the service core, makes every bridge access; the measurement's control task posts
 * an operation and waits for its status. */
#ifndef SUNSTONE_LATENCY_AXI_CAP_H
#define SUNSTONE_LATENCY_AXI_CAP_H

#include <stdint.h>

#define LATENCY_AXI_MAGIC UINT64_C(0x5353415849434131)        /* "SSAXICA1" */
#define LATENCY_AXI_FOOTER_MAGIC UINT64_C(0x5353415849434631) /* "SSAXICF1" */
#define LATENCY_AXI_VERSION 1u
#define LATENCY_AXI_BRIDGES 2u
#define LATENCY_AXI_WORDS 7u          /* +0x30, then +0x48 to +0x5c */
#define LATENCY_AXI_SUBPHASES 4u      /* U B B U */
#define LATENCY_AXI_SUBPHASE_WAKES 32u
#define LATENCY_AXI_CAPPED_MASK 0x6u  /* sub-phases 1 and 2 */
#define LATENCY_AXI_SWITCHES 16u
/* Limit p = 0 (the total) at f = DRAM0 = 510 MHz, 1/256 byte per cycle (sunxi-nsi.c lines 251-262 and 401-488):
 * 1,500 MB/s per bridge, and the largest word, which is above what DRAM delivers. */
#define LATENCY_AXI_BW_B 753u
#define LATENCY_AXI_SAT_B 483u
#define LATENCY_AXI_BW_NONBINDING 4095u
#define LATENCY_AXI_SAT_NONBINDING 1023u
/* The warm-up windows and the 5% stream check that admits a binding level. */
#define LATENCY_AXI_WARM_MS 50u
#define LATENCY_AXI_WARM_PERCENT 95u
/* Nested deadlines: one operation, then one busy sub-phase. The run budget (run_us) and the lifetime enclose them. */
#define LATENCY_AXI_OP_MS 20u
#define LATENCY_AXI_SUBPHASE_US 1000000u

enum latency_axi_op {
    LATENCY_AXI_ENTRY = 1,     /* read the gates and the seven words; zero any nonzero word */
    LATENCY_AXI_SET_NONBINDING,
    LATENCY_AXI_SET_B,
    LATENCY_AXI_CLEAR,         /* clear bits 0 and 16 of +0x30; the limit words stay */
    LATENCY_AXI_RESTORE,       /* +0x30, +0x48 and +0x4c to 0 */
};
enum latency_axi_status {
    LATENCY_AXI_OK = 0,
    LATENCY_AXI_GATE_CLOSED = 1,  /* a gate read closed: no bridge access follows */
    LATENCY_AXI_READBACK = 2,     /* a word read back other than written */
    LATENCY_AXI_ENTRY_SET = 3,    /* a word read nonzero at entry and was zeroed */
    LATENCY_AXI_BAD_OP = 4,
};

struct latency_axi_window {
    uint64_t start, end;
    uint64_t sweeps_start[8], sweeps_end[8];
};
struct latency_axi_switch {
    uint64_t begin, end;          /* counter ticks around the whole operation, gate reads included */
    uint64_t access_ticks;        /* counter ticks inside the bridge accesses alone */
    uint32_t op, status;
    uint32_t gates[3];            /* the last gate words read: 0x02002c00, 0x02002c0c, 0x08000200 */
    uint32_t accesses;            /* bridge reads and writes made */
    uint32_t readback[LATENCY_AXI_BRIDGES][LATENCY_AXI_WORDS];
};
struct latency_axi_report {
    uint64_t magic, version, bytes;
    uint32_t bw_b, sat_b, bw_nonbinding, sat_nonbinding;
    uint32_t entry[LATENCY_AXI_BRIDGES][LATENCY_AXI_WORDS];
    uint32_t subphase_wakes, capped_mask, switch_count, complete;
    struct latency_axi_window warm[2];                      /* uncapped, then non-binding */
    struct latency_axi_window sub[LATENCY_AXI_SUBPHASES];
    struct latency_axi_switch switches[LATENCY_AXI_SWITCHES];
};
_Static_assert(sizeof(struct latency_axi_report) <= 4096 - 64, "the section and its footer fit the 4 KiB allowance");

extern struct latency_axi_report latency_axi;
/* Create the EL1 task on CPU0 (from the application's init hook). */
void latency_axi_init(void);
/* Post one operation to the CPU0 task and wait for it, at most LATENCY_AXI_OP_MS. Returns its status, or -1 when
 * the task did not answer in time. */
int latency_axi_request(enum latency_axi_op op);
/* The task body. It is named here because the image grants it EL1 (apps/latency/axi-privilege.c). */
void latency_axi_worker(void *argument);
/* One operation, run by the task; exposed for the host check (checks/latency-axi-cap.py). */
uint32_t latency_axi_run(enum latency_axi_op op);

#endif
