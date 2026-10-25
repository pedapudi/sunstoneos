#ifndef A733_SECONDARY_LOCAL_PROBE_H
#define A733_SECONDARY_LOCAL_PROBE_H
/* Plain literals only: assembly includes this header, and the host modules
 * read every "#define NAME VALUE" line with a regular expression. */
#define SECONDARY_LOCAL_RECORDS 0x42018a00
#define SECONDARY_DISTRIBUTOR_RECORD 0x42019a00
/* Read-only status bits in each snapshot word (struct secondary_local_state).
 * A before/after equality and a baseline equality ignore them; the records
 * keep the raw words. The references poll these bits and never compare them
 * (Linux irq-gic-v3.c gic_redist_wait_for_rwp; TF-A gicv3_helpers.c).
 * - GICR_CTLR: UWP (31) and RWP (3) report writes in progress (IHI 0069G
 *   12.11.2). The board has read RWP set after a completed wait.
 * - GICR_WAKER: Quiescent (31, GIC-600 TRM table 4-24) and ChildrenAsleep (2)
 *   follow ProcessorSleep. Each handshake waits for ChildrenAsleep; a later
 *   comparison does not.
 * - GICR_ISPENDR0: pending state that a level-sensitive PPI can assert while
 *   disabled. The explicit drain checks still require their sources clear.
 * - GICR_PWRR: RDGPO (3) and RDGPD (2) report the power state of the
 *   redistributor group that all eight frames share (GIC-600 TRM 4.4.5). The
 *   power handshake still requires them equal before it writes RDPD. */
#define GICR_CTLR_STATUS 0x80000008
#define GICR_WAKER_STATUS 0x80000004
#define GICR_ISPENDR0_STATUS 0xffffffff
#define GICR_PWRR_STATUS 0xc
/* GICD_CTLR.RWP (31): a distributor write in progress. Waited for, as Linux
 * gic_do_wait_for_rwp and TF-A gicd_wait_for_pending_write do. */
#define GICD_CTLR_RWP 0x80000000
/* Distributor record error codes (struct secondary_distributor_record.error). */
#define DISTRIBUTOR_ERROR_CONFIGURATION 1
#define DISTRIBUTOR_ERROR_RWP_TIMEOUT 2
/* Return-deadline record (docs/designs/workload-bisect.md). It occupies the
 * first 512 bytes of the area into which a completed coherent survey copies
 * the shared page, so its magic number cannot remain after a completed run. */
#define DEADLINE_RECORD 0x4201b200
#define DEADLINE_RECORD_BYTES 512
#define DEADLINE_RECORD_MAGIC 0x413733444c4e5231
/* 0.5 s at 24 MHz after the startup deadline. */
#define DEADLINE_TICKS 12000000
#ifndef __ASSEMBLER__
#include <stdint.h>
struct secondary_local_state {
    uint64_t ctlr,waker,enable,pending,active,group,modifier,configuration,priority,pwrr;
};
struct secondary_local_access { uint64_t address,value,operation,completed; };
struct secondary_local_record {
    uint64_t state,epoch,core,deadline,base,control_before,control_after;
    uint64_t controller_state,phase,error,writes,cleanup_error,cleanup_writes;
    uint64_t sre_before,sre_after,group0,group1;
    struct secondary_local_state before,after;
    struct secondary_local_access access,controller_access;
    uint64_t reserved[19];
};
_Static_assert(sizeof(struct secondary_local_record)==512,"Per-core local probe report ABI");
/* Returning diagnostic only. Distributor configuration and exclusive CPU,
 * redistributor, report and interrupt ownership precede this call. The target
 * is at EL3 with masked exceptions, no caches and its actual 24MHz counter setup.
 * The target's frame must already be powered, as in the observed core-1 path.
 * Return1 means initialization and restoration passed; return2 means a recorded
 * bounded refusal with unchanged/restored local state; return0 retains ownership.
 * Neither success nor refusal establishes interrupt delivery. */
struct secondary_distributor_record {
    uint64_t state, before, after, banks, deadline;
    uint32_t enable_before[32], enable_after[32];
    uint64_t error, reserved[26];
};
_Static_assert(sizeof(struct secondary_distributor_record)==512,"Distributor report ABI");
int secondary_distributor_begin(uint64_t deadline);
int secondary_distributor_restore(uint64_t deadline);
int secondary_local_probe(unsigned core,uint64_t epoch,uint64_t deadline,
                          volatile struct secondary_local_record *record);
/* 1 when two snapshots agree outside the status bits above. Defined in
 * secondary-distributor-probe.c, the one unit that every overlay running a
 * local restoration links. */
int secondary_local_same(const volatile struct secondary_local_state *a,
                         const volatile struct secondary_local_state *b);
#endif
#endif
