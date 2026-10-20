/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SUNSTONE_A733_IOMMU_H
#define SUNSTONE_A733_IOMMU_H
#include <stddef.h>
#include <stdint.h>

#define A733_IOMMU_BASE UINT64_C(0x03900000)
#define A733_IOMMU_BANK_BYTES 0x10000u
#define A733_IOMMU_REAL_MASTERS 0x0fcfu
#define A733_IOMMU_CSI_MASTER 1u
#define A733_IOMMU_MAX_SPANS 8u
#define A733_IOMMU_L2_TABLES 4u
#define A733_IOMMU_TABLE_BYTES 32768u
#define A733_IOMMU_READ 1u
#define A733_IOMMU_WRITE 2u

/* IDs 4/5 are absent. IDs 0..3 use bank0; IDs 6..11 use bank1.
 * Each bank has one directory, shared by every translated master.
 */
struct a733_iommu_master {
    const char *name;
    uint8_t id, bank, local_id;
    uint32_t micro_offset; /* Relative to A733_IOMMU_BASE. */
};
const struct a733_iommu_master *a733_iommu_master(unsigned id);
struct a733_iommu_span {
    uint64_t iova, physical, bytes; /* Complete, exclusively reserved pages. */
    unsigned access; /* Device permissions; READ, WRITE, or both. */
};
struct a733_iommu_tables {
    uint32_t *words; /* Exclusive 32KiB, CPU and physical aligned to 16KiB. */
    uint64_t physical;
    struct a733_iommu_span spans[A733_IOMMU_MAX_SPANS];
    unsigned count;
};
/* Offline only: emits 4096 directory entries plus four 1024-entry tables.
 * IOVA [0x1000,2^34); CPU physical [0x40000000,2^34).
 * Refuses aliases, incomplete pages, overlapping physical/IOVA spans, and
 * more than four distinct 4MiB windows. Refusal changes neither output.
 * No live table changes, allocator, high-DRAM alias conversion, or unmapping.
 */
int a733_iommu_build(struct a733_iommu_tables *, uint32_t *words,
    uint64_t table_physical, const struct a733_iommu_span *, unsigned count);
int a733_iommu_validate(const struct a733_iommu_tables *);
int a733_iommu_translate(const struct a733_iommu_tables *, uint64_t iova,
    unsigned access, uint64_t *physical);

/* Admission is a platform handoff proof, not a hardware-idle heuristic.
 * Tokens and evidence stay owned by the controller after ACTIVE or FAULT.
 * No current platform supplies this proof; hardware installation is blocked.
 */
struct a733_iommu_admission {
    uint64_t token;
    uint16_t excluded_masters, powered_masters;
    uint8_t inherited_resolved, transactions_drained, clocks_held, recovery_qualified;
    /* Explicit supervised mode: permanent retention until confirmed whole-
     * board power removal; never substitutes for initial transaction drain. */
    uint8_t retained_power_removal;
};
struct a733_iommu_ops {
    int (*now_us)(void *, uint64_t *);
    /* Return 1 complete, 0 pending, -1 failure. Set nonzero stable token before
     * acquiring anything. Retain both banks, all ten masters' exclusion,
     * their power domains, clocks/resets, CPU/table/buffer reservations, and
     * IRQ routing. Drain outstanding bus transactions and page-table walks
     * independently of this driver's fixed flush delay. Keep every master quiet
     * through ACTIVE. Table spans must
     * be DMA-authorized RAM, disjoint from CPU/controller metadata; words
     * must be the CPU alias of table_physical. No mapping/MMIO changes here.
     * inherited_resolved requires an independently established handoff; an
     * idle CSI or cleared ENABLE register is insufficient evidence.
     */
    int (*admit)(void *, const struct a733_iommu_tables *, uint64_t deadline,
                 struct a733_iommu_admission *);
    /* Ordered Device accesses, token required. Offset [0,0x20000), aligned.
     * Return 1 or -1. A successful write includes a completion barrier.
     */
    int (*read)(void *, uint64_t token, uint32_t off, uint32_t *, uint64_t);
    int (*write)(void *, uint64_t token, uint32_t off, uint32_t, uint64_t);
    /* Publish entire table arena to point of coherency, including a final
     * completion barrier. Return 1, 0 pending, or -1. Own CPU aliases first.
     * Payload cache preparation is a separate camera-memory responsibility.
     */
    int (*publish)(void *, uint64_t token, const void *, size_t, uint64_t);
};
enum a733_iommu_state {
    A733_IOMMU_NEW, A733_IOMMU_ADMIT, A733_IOMMU_CHECK,
    A733_IOMMU_PUBLISH, A733_IOMMU_PROGRAM, A733_IOMMU_ACTIVE,
    A733_IOMMU_SCAN, A733_IOMMU_FAULT
};
struct a733_iommu_fault {
    uint32_t bank_status[2], l1_status[2], l2_status[2];
    uint32_t walk_address_low[2][2], walk_address_high[2][2], walk_data[2][2];
    uint32_t micro_status[10], micro_low[10], micro_high[10], micro_data[10];
    uint16_t scanned_masters; /* Only these micro status sets were accessed. */
    uint8_t complete; /* Partial diagnostic data remains valid after IO failure. */
};
struct a733_iommu {
    struct a733_iommu_ops ops;
    struct a733_iommu_tables tables;
    struct a733_iommu_admission admission;
    struct a733_iommu_fault fault;
    void *context;
    uint64_t deadline, now, wait_until;
    unsigned index, phase;
    uint16_t translated_masters, accessible_masters;
    enum a733_iommu_state state;
    uint8_t initialized, camera_supervised;
};
/* Zeroed fresh storage, one privileged serialized caller, no callback reentry.
 * Initial cold install only: both ENABLE registers must already be zero;
 * faults must be absent. Installs one shared camera directory in both banks;
 * CSI translates, all other masters bypass and remain excluded. Never resets,
 * replaces enabled inherited mappings, acknowledges faults, or releases leases.
 * ACTIVE authorizes a higher-level global owner to admit CSI traffic; other
 * masters still require separately implemented admission and mapping policy.
 */
int a733_iommu_init(struct a733_iommu *, const struct a733_iommu_ops *,
                    void *, const struct a733_iommu_tables *);
/* Same CSI translation and all-ten-master accessibility as the default camera
 * initializer. Requires explicit retained power-removal admission instead of
 * watchdog qualification; all initial reset/drain/exclusion facts stay required.
 * Retains every table, buffer and device resource until whole-board power removal.
 * No timeout, reconnection or completed frame establishes global drain/reuse. */
int a733_iommu_init_camera_supervised(struct a733_iommu *,const struct a733_iommu_ops *,
    void *,const struct a733_iommu_tables *);
/* Fixed decoder profile translates6/7 only. Both banks and all ten masters
 * remain exclusively excluded/drained at entry. Only6/7 must be powered; every
 * skipped master must remain physically quiescent and unavailable for the whole
 * session. No micro MMIO is issued for skipped sets (source skip-mask policy).
 * Accepts real independent recovery OR explicit permanent supervised retention.
 * No later master admission, bank sharing, DMA reuse or release is authorized. */
int a733_iommu_init_decoder(struct a733_iommu *,const struct a733_iommu_ops *,
    void *,const struct a733_iommu_tables *);
/* Fixed display profile: translate/access master8 only, READ-only spans.
 * Same global initial isolation and retained manual-power-removal contract as
 * the decoder profile. No table changes, master sharing or release. */
/* Fixed encoder profile translates/accesses master3 only. Same all-master
 * initial exclusion/drain and permanent retention as decoder; no reuse. */
int a733_iommu_init_encoder(struct a733_iommu *,const struct a733_iommu_ops *,
    void *,const struct a733_iommu_tables *);
/* Fixed G2D profile translates/accesses master10 only. Global initial
 * exclusion/drain and retained recovery contract match the encoder profile. */
int a733_iommu_init_g2d(struct a733_iommu *,const struct a733_iommu_ops *,
    void *,const struct a733_iommu_tables *);
/* Fixed offline DI301 profile: translate/access master9 only. The same global
 * handoff/exclusion and permanent retained lifetime as G2D are required. */
int a733_iommu_init_deinterlace(struct a733_iommu *,const struct a733_iommu_ops *,
    void *,const struct a733_iommu_tables *);
int a733_iommu_init_display(struct a733_iommu *,const struct a733_iommu_ops *,
    void *,const struct a733_iommu_tables *);
int a733_iommu_open(struct a733_iommu *, uint64_t deadline_us);
/* At most one MMIO/cache/admission operation, with pre/post deadline checks.
 * Source flush holds its request for at least 10us, then clears it. This is
 * an initial-install sequence, never evidence allowing table reclamation.
 */
int a733_iommu_poll(struct a733_iommu *);
/* Non-destructive fault capture, allowed from ACTIVE or retained FAULT.
 * Reading fault registers requires retained powered-master admission.
 * Fault detection ends in FAULT; no recovery/reset is attempted.
 */
int a733_iommu_scan(struct a733_iommu *, uint64_t deadline_us);
#endif
