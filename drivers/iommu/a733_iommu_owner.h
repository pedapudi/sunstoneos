/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SUNSTONE_A733_IOMMU_OWNER_H
#define SUNSTONE_A733_IOMMU_OWNER_H
/* The one IOMMU owner of the OS image. Every OS-image master (G2D 10, decoder 6 and 7, and later camera, ISP and
 * display) attaches to it; it installs one directory, shared by both banks as the vendor driver's single domain does
 * (sunxi-iommu-v2.c sunxi_iommu_hw_init writes one TTB to both instances), at the first attach, and adds each later
 * master's spans to that directory. Table memory comes from the caller's allocator through ops.table: a 16 KiB
 * directory and one 4 KiB second-level table per 4 MiB window that a span touches, allocated when first needed.
 * The owner never holds the IOMMU clock itself: ops.clock takes the A733_CLOCK_IOMMU_SYS lease at the first attach
 * and releases it after the last detach disables both banks. One caller at a time: the OS wiring serializes calls.
 * Every call is bounded: the only wait is the vendor's 10 us after a flush or invalidation request. */
#include <stddef.h>
#include <stdint.h>
#include "a733_iommu.h"

#define A733_IOMMU_OWNER_DIRECTORY_BYTES 16384u
#define A733_IOMMU_OWNER_L2_BYTES 4096u
#define A733_IOMMU_OWNER_MASTERS 12u /* IDs 0..11; 4 and 5 do not exist */
/* Masters the first attach sets to bypass when they are not attached: USB (0), an IOMMU master child with no power
 * domain in sun60iw2p1.dtsi, live in the OS image, and bypassed by the vendor hw_init (DEFAULT_BYPASS_VALUE bit 0).
 * Every other unattached master is left untouched: its micro registers need its power domain and clocks, and the
 * vendor's register wrappers skip a master that has not probed. */
#define A733_IOMMU_OWNER_BYPASSED 0x0001u

struct a733_iommu_owner_ops {
    /* Microsecond clock. */
    int (*now_us)(void *, uint64_t *);
    /* One ordered Device access at [0, 0x20000) from A733_IOMMU_BASE; a write completes before return. 1 or -1. */
    int (*read)(void *, uint32_t offset, uint32_t *);
    int (*write)(void *, uint32_t offset, uint32_t);
    /* Zeroed, device-coherent (non-cacheable) table memory: bytes, aligned to align. cpu is the CPU address of
     * physical. 1 or -1. release returns it. */
    int (*table)(void *, size_t bytes, size_t align, uint32_t **cpu, uint64_t *physical);
    void (*release)(void *, uint32_t *cpu, uint64_t physical, size_t bytes);
    /* Order the table stores before the next device access (a barrier on non-cacheable memory). 1 or -1. */
    int (*publish)(void *);
    /* take 1: acquire the IOMMU clock lease; take 0: release it. 1 or -1. */
    int (*clock)(void *, int take);
};
struct a733_iommu_owner_fault {
    uint32_t bank_status[2], l1_status[2], l2_status[2];
    uint32_t micro_status, micro_low, micro_high, micro_data;
};
/* Read at the first attach before any write. */
struct a733_iommu_owner_entry {
    uint32_t enable[2], status[2], l1[2], l2[2];
    uint32_t micro_status[A733_IOMMU_OWNER_MASTERS], bypass[A733_IOMMU_OWNER_MASTERS]; /* touched masters only */
    uint32_t recorded;
};
enum a733_iommu_owner_state { A733_IOMMU_OWNER_IDLE, A733_IOMMU_OWNER_ACTIVE, A733_IOMMU_OWNER_FAULT };
struct a733_iommu_owner {
    struct a733_iommu_owner_ops ops;
    void *context;
    uint32_t *directory;
    uint64_t directory_physical;
    uint16_t attached, translated;             /* master bits */
    uint16_t touched;                          /* masters whose micro registers this lifetime wrote; the last detach
                                                * flushes them all */
    enum a733_iommu_owner_state state;
    unsigned tables, entries;                  /* second-level tables allocated, leaf entries present */
    uint32_t fault_step, fault_word;           /* the first failure: step and the register offset or word */
    uint32_t fault_read;                       /* for a readback mismatch, the value read */
    uint32_t refused_word;                     /* last attach refusal with no change: 1 clock lease, 2 entry state */
    struct a733_iommu_owner_entry entry;
    struct a733_iommu_owner_fault faults[A733_IOMMU_OWNER_MASTERS];
    uint16_t faulted;                          /* masters whose fault record is set */
    uint8_t initialized, clock_held;
};
int a733_iommu_owner_init(struct a733_iommu_owner *, const struct a733_iommu_owner_ops *, void *context);
/* Attach masters (bits of real master IDs) with their spans, each span complete pages, IOVA below
 * A733_IOMMU_IOVA_LIMIT and physical below A733_IOMMU_PHYSICAL_LIMIT, disjoint from every span already present.
 * The first attach records the entry state, refuses a bank that is already enabled or reports a fault, then installs
 * the directory in both banks, sets the attached masters to translate and A733_IOMMU_OWNER_BYPASSED to bypass, and
 * enables both banks; a later attach writes the new entries, invalidates the TLB and page-walk caches over each new
 * span, then stops bypassing the new masters. 1 done, -1 refused (no change, refused_word set) or failed (state
 * FAULT). The caller attaches a master only while its power domain is on and its clocks run. */
int a733_iommu_owner_attach(struct a733_iommu_owner *, uint16_t masters, const struct a733_iommu_span *, unsigned count);
/* Add spans for masters already attached (a new buffer). Same rules and invalidation as a later attach. */
int a733_iommu_owner_map(struct a733_iommu_owner *, const struct a733_iommu_span *, unsigned count);
/* Remove spans: entries cleared, then the TLB and page-walk caches invalidated over each span. The caller has
 * drained every master that used them. */
int a733_iommu_owner_unmap(struct a733_iommu_owner *, const struct a733_iommu_span *, unsigned count);
/* Detach drained masters: each master's fault words are scanned into faults[master], then it is bypassed again. After
 * the last one, both banks are flushed with every master this lifetime touched, and disabled, the tables are released
 * and the clock lease is given back. Detach also runs after a fault, so the quiesce path can
 * still disable the banks; a bank that does not read back disabled keeps its tables and the lease. A fault stays
 * recorded for the lifetime: later attaches are refused. 1 done, -1 failed. */
int a733_iommu_owner_detach(struct a733_iommu_owner *, uint16_t masters);
/* Read the bank and the master's micro fault words into faults[master]; 1 none, 0 a fault recorded, -1 failed. */
int a733_iommu_owner_scan(struct a733_iommu_owner *, unsigned master);
/* The device address of a CPU physical address in a span, for checks. 1 or -1. */
int a733_iommu_owner_translate(const struct a733_iommu_owner *, uint64_t iova, unsigned access, uint64_t *physical);
#endif
