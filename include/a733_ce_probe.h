/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_A733_CE_PROBE_H
#define SUNSTONE_A733_CE_PROBE_H
/* The CE-only board probe (docs/status/security.md, "CE board probe"): take the crypto engine through the vendor's
 * non-secure bring-up, run ONE 32-byte TRNG task, and record how it went. The record never holds random bytes.
 *
 * The probe proves only that the engine runs on the board under the vendor sequence and returns a block. It does not
 * qualify the entropy, and the health tests are at the hardware's reset defaults: nothing programs or reads them
 * (docs/crypto-engine.md). The state machine here touches no hardware; the caller supplies the operations, so the host
 * check runs the code the image links. */
#include <stddef.h>
#include <stdint.h>

#define A733_CE_PROBE_MAGIC UINT64_C(0x3152504345333337) /* bytes "733ECPR1" */
#define A733_CE_PROBE_VERSION 1u
#define A733_CE_PROBE_STEP_LIMIT_US 2000000u             /* the TRNG task's bound: the platform's 2 s step */
#define A733_CE_PROBE_BYTES 32u

enum a733_ce_probe_stage { A733_CE_PROBE_NEW, A733_CE_PROBE_ENTRY, A733_CE_PROBE_CLOCKED, A733_CE_PROBE_BUFFERS,
    A733_CE_PROBE_CONFIGURED, A733_CE_PROBE_REQUESTED, A733_CE_PROBE_DONE };

/* flags: bit 0 the health tests are at hardware defaults and were not programmed (always set), bit 1 the CE was found
 * enabled at admission and was taken down first, bit 2 the task completed, bit 3 the 32 bytes were not all zero,
 * bit 4 the engine's non-secure double-entropy source was set (CE_REG_LPC bit 1) when the task finished.
 * failure_stage: bits 7:0 the stage that did not complete, bits 31:8 the failing step's own detail. */
struct a733_ce_probe_record {
    uint64_t magic;
    uint32_t version, bytes;
    uint32_t stage, failed, failure_stage, flags;
    uint32_t entry_clk, entry_bgr, entry_mbus;      /* CE_CLK_REG, CE_BGR_REG, CCU 0x5e4 when the probe starts */
    uint32_t clocked_clk, clocked_bgr, clocked_mbus; /* the same after the bring-up */
    uint32_t status_ns, error_ns, lpc;               /* NS+0xc, NS+0x18, NS+0xd0 when the task finished */
    uint32_t bytes_returned, task_us, step_limit_us;
    uint64_t descriptor_physical, output_physical;
};
_Static_assert(sizeof(struct a733_ce_probe_record)==96,"CE probe record");

struct a733_ce_probe_ops {
    void *ctx;
    int (*now_us)(void *,uint64_t *);                 /* 1 */
    int (*read)(void *,uintptr_t,uint32_t *);         /* 1; CCU and non-secure CE words */
    int (*bring_up)(void *);                          /* 1 done, 0 pending, negative failure: reset-and-gate cycle, clocks */
    int (*alloc)(void *,uint64_t *descriptor,uint64_t *output); /* 1 ready, 0 pending, negative failure */
    int (*configure)(void *);                         /* 1, 0 pending, negative: a733_ce_init */
    int (*request)(void *,uint64_t deadline_us);      /* 1 started, 0 busy, negative failure */
    int (*poll)(void *,uint8_t block[A733_CE_PROBE_BYTES]); /* 1 block copied, 0 pending, negative failure */
    void (*retain)(void *);                           /* called once on failure */
    unsigned (*detail)(void *);                       /* optional: why the failing stage failed, 24 bits, into failure_stage bits 31:8 */
};
struct a733_ce_probe {
    struct a733_ce_probe_ops ops;
    struct a733_ce_probe_record record;
    uint64_t started_us;
    unsigned stage;
};
void a733_ce_probe_init(struct a733_ce_probe *,const struct a733_ce_probe_ops *);
/* One bounded step. 0 more, 1 done (record complete), negative failed (record says where). */
int a733_ce_probe_step(struct a733_ce_probe *);
#endif
