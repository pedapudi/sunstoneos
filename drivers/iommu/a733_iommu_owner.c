/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Register sequence and table formats adapted from the A733 Linux IOMMU driver (sunxi-iommu-v2.c, sunxi-iommu-pgtable.c).
 * Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved.
 * Copyright (c) 2023, ouyangkun <ouyangkun@allwinnertech.com>
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License. */
#include "a733_iommu_owner.h"
#include "dma_address_width.h"
#include <string.h>

#define PAGE 4096u
#define WINDOW (UINT64_C(1) << 22)
#define DIRECTORY_WORDS 4096u
#define L2_WORDS 1024u
#define BANK 0x10000u
/* Bank registers (sunxi-iommu-v2.c 41-79). */
#define ENABLE 0x14u
#define AUTO_GATING 0x1cu
#define AUTO_GATING_VALUE 0x7fff0003u
#define TTB 0x20u
#define PREFETCH 0x2cu
#define PREFETCH_VALUE 0x3007fu /* the A733 platform data's tlb_prefetch, as a733_iommu.c programs it */
#define FLUSH 0x30u
#define TLB_MODE 0x40u
#define TLB_START 0x44u
#define TLB_END 0x48u
#define TLB_ENABLE 0x54u
#define PC_MODE 0x60u
#define PC_START 0x68u
#define PC_ENABLE 0x6cu
#define PC_END 0x70u
#define INT_ENABLE 0x80u
#define INT_STATUS 0x88u
#define L1_STATUS 0xb0u
#define L2_STATUS 0xb4u
/* Micro (per-master) registers, relative to the master's micro block. */
#define MIC_BYPASS 0x04u
#define MIC_FLUSH 0x18u
#define MIC_INT_ENABLE 0x30u
#define MIC_INT_STATUS 0x38u

enum { STEP_IO = 1, STEP_READBACK, STEP_TABLE, STEP_PUBLISH, STEP_CLOCK, STEP_WAIT };

static int fail(struct a733_iommu_owner *o, unsigned step, uint32_t word)
{
    if(!o->fault_step) { o->fault_step = step; o->fault_word = word; }
    o->state = A733_IOMMU_OWNER_FAULT;
    return -1;
}
static int rd(struct a733_iommu_owner *o, uint32_t off, uint32_t *v)
{
    return o->ops.read(o->context, off, v) == 1 ? 1 : fail(o, STEP_IO, off);
}
static int wr(struct a733_iommu_owner *o, uint32_t off, uint32_t v)
{
    return o->ops.write(o->context, off, v) == 1 ? 1 : fail(o, STEP_IO, off);
}
/* A write with readback of the whole word. */
static int set(struct a733_iommu_owner *o, uint32_t off, uint32_t v)
{
    uint32_t r;
    if(wr(o, off, v) != 1 || rd(o, off, &r) != 1) return -1;
    return r == v ? 1 : fail(o, STEP_READBACK, off);
}
static int both(struct a733_iommu_owner *o, uint32_t off, uint32_t v)
{
    return set(o, off, v) == 1 && set(o, BANK + off, v) == 1 ? 1 : -1;
}
/* The vendor's udelay(10) after a flush or invalidation request; one more microsecond for the clock's rounding. */
static int wait10(struct a733_iommu_owner *o)
{
    uint64_t start, now;
    if(o->ops.now_us(o->context, &start) != 1) return fail(o, STEP_WAIT, 0);
    do if(o->ops.now_us(o->context, &now) != 1 || now < start) return fail(o, STEP_WAIT, 1);
    while(now - start < 11);
    return 1;
}
static uint32_t address(uint64_t pa) { return ((uint32_t)pa & 0xfffff000u) | (uint32_t)((pa >> 24) & 0x300u); }
static uint64_t physical_of(uint32_t entry) { return (uint64_t)(entry & 0xfffff000u) | (uint64_t)(entry & 0x300u) << 24; }
static uint32_t leaf(uint64_t pa, unsigned access)
{
    return address(pa) | 2u | ((access & A733_IOMMU_READ) ? 4u : 0) | ((access & A733_IOMMU_WRITE) ? 8u : 0);
}
static int real(uint16_t masters) { return masters && !(masters & ~A733_IOMMU_REAL_MASTERS); }
/* The master's micro set: IDs 0..3 at bank 0 local 0..3, IDs 6..11 at bank 1 local 0..5, 0x200 bytes each from
 * 0x400 in their bank (sunxi-iommu-v2.c IOMMU_MIC_*; drivers/iommu/a733_iommu.c lists the same offsets). */
static uint32_t micro(unsigned id) { return id < 4 ? 0x400u + id * 0x200u : BANK + 0x400u + (id - 6) * 0x200u; }

/* The CPU alias of each second-level table, beside the directory (sunxi-iommu-pgtable.c keeps the same pairing). */
static uint32_t **aliases(const struct a733_iommu_owner *o)
{
    return (uint32_t **)(void *)((uint8_t *)o->directory + A733_IOMMU_OWNER_DIRECTORY_BYTES);
}
#define ALIAS_BYTES (DIRECTORY_WORDS * sizeof(uint32_t *))

int a733_iommu_owner_init(struct a733_iommu_owner *o, const struct a733_iommu_owner_ops *ops, void *context)
{
    if(!o || !ops || !ops->now_us || !ops->read || !ops->write || !ops->table || !ops->release || !ops->publish ||
       !ops->clock || o->initialized) return -1;
    memset(o, 0, sizeof(*o));
    o->ops = *ops; o->context = context; o->initialized = 1;
    return 1;
}
static int span_valid(const struct a733_iommu_span *s)
{
    return s->bytes && !(s->iova & (PAGE - 1)) && !(s->bytes & (PAGE - 1)) && !(s->physical & (PAGE - 1)) &&
        s->iova >= PAGE && s->iova < A733_IOMMU_IOVA_LIMIT && s->bytes <= A733_IOMMU_IOVA_LIMIT - s->iova &&
        s->physical >= UINT64_C(0x40000000) && s->physical < A733_IOMMU_PHYSICAL_LIMIT &&
        s->bytes <= A733_IOMMU_PHYSICAL_LIMIT - s->physical && s->access && !(s->access & ~3u);
}
/* The leaf entry of iova, or null when its table does not exist. */
static uint32_t *entry_of(const struct a733_iommu_owner *o, uint64_t iova)
{
    if(!o->directory) return NULL;
    uint32_t *t = aliases(o)[iova >> 22];
    return t ? &t[(iova >> 12) & (L2_WORDS - 1)] : NULL;
}
/* Every page of every span is unmapped, and the spans do not overlap one another. */
static int spans_free(const struct a733_iommu_owner *o, const struct a733_iommu_span *s, unsigned count)
{
    if(!s || !count) return 0;
    for(unsigned i = 0; i < count; ++i) {
        if(!span_valid(&s[i])) return 0;
        for(unsigned j = 0; j < i; ++j)
            if(s[i].iova < s[j].iova + s[j].bytes && s[j].iova < s[i].iova + s[i].bytes) return 0;
        for(uint64_t a = s[i].iova; a < s[i].iova + s[i].bytes; a += PAGE) {
            const uint32_t *e = entry_of(o, a);
            if(e && *e) return 0;
        }
    }
    return 1;
}
static int write_spans(struct a733_iommu_owner *o, const struct a733_iommu_span *s, unsigned count)
{
    for(unsigned i = 0; i < count; ++i)
        for(uint64_t off = 0; off < s[i].bytes; off += PAGE) {
            const uint64_t iova = s[i].iova + off;
            uint32_t **alias = &aliases(o)[iova >> 22];
            if(!*alias) {
                uint32_t *cpu; uint64_t pa;
                if(o->ops.table(o->context, A733_IOMMU_OWNER_L2_BYTES, A733_IOMMU_OWNER_L2_BYTES, &cpu, &pa) != 1 ||
                   !cpu || (pa & (PAGE - 1)) || pa < UINT64_C(0x40000000) || pa >= A733_IOMMU_PHYSICAL_LIMIT)
                    return fail(o, STEP_TABLE, (uint32_t)(iova >> 22));
                memset(cpu, 0, A733_IOMMU_OWNER_L2_BYTES);
                *alias = cpu; ++o->tables;
                /* The directory entry follows its zeroed table; publish orders both before the device sees them. */
                o->directory[iova >> 22] = address(pa) | 1u;
            }
            (*alias)[(iova >> 12) & (L2_WORDS - 1)] = leaf(s[i].physical + off, s[i].access);
            ++o->entries;
        }
    return o->ops.publish(o->context) == 1 ? 1 : fail(o, STEP_PUBLISH, 0);
}
/* Range invalidation of the TLB and the page-walk cache over [iova, iova + bytes) in both banks: start and end, then
 * enable held 10 us and cleared (sunxi_tlb_invalid_locked and sunxi_ptw_cache_invalid_locked for version 14 and
 * later; WRITE_EQUAL_REG writes each step to both banks before the one wait). */
static int pulse(struct a733_iommu_owner *o, uint32_t start, uint32_t end, uint32_t enable, uint32_t first, uint32_t last)
{
    for(unsigned b = 0; b < 2; ++b)
        if(set(o, b * BANK + start, first) != 1 || set(o, b * BANK + end, last) != 1) return -1;
    if(wr(o, enable, 1) != 1 || wr(o, BANK + enable, 1) != 1 || wait10(o) != 1) return -1;
    return both(o, enable, 0);
}
/* Map (sunxi_iommu_iotlb_sync_map through sunxi_zap_tlb, version 14 and later): the TLB from the first page to eight
 * pages past the end, and the page-walk cache over the windows the span touches. Unmap (sunxi_iommu_unmap): the TLB and
 * the page-walk cache over the span. The vendor invalidates before it clears the entries; the owner clears first, so
 * no walk can cache a cleared entry after the invalidation. */
static int invalidate(struct a733_iommu_owner *o, uint64_t iova, uint64_t bytes, uint64_t tail)
{
    const uint64_t end = iova + bytes;
    return pulse(o, TLB_START, TLB_END, TLB_ENABLE, (uint32_t)(iova >> 12), (uint32_t)((end + tail) >> 12)) == 1 &&
        pulse(o, PC_START, PC_END, PC_ENABLE, (uint32_t)(iova >> 22), (uint32_t)(end >> 22)) == 1 ? 1 : -1;
}
#define MAP_TAIL (8u * PAGE)
/* sunxi_tlb_flush: per bank, the global flush request and the listed masters' micro flush requests; then one 10 us
 * wait and the global requests cleared. */
static int flush(struct a733_iommu_owner *o, uint16_t masters)
{
    for(unsigned b = 0; b < 2; ++b) {
        if(wr(o, b * BANK + FLUSH, 3) != 1) return -1;
        for(unsigned id = b ? 6 : 0; id < (b ? A733_IOMMU_OWNER_MASTERS : 4); ++id)
            if((masters >> id & 1) && wr(o, micro(id) + MIC_FLUSH, 1) != 1) return -1;
    }
    return wait10(o) == 1 && set(o, FLUSH, 0) == 1 && set(o, BANK + FLUSH, 0) == 1 ? 1 : -1;
}
/* Interrupt enables off (faults are read by scan), then bypass: 0 for the masters in translate, 1 for the rest of
 * masters, each in master order (__sunxi_iommu_enable_interrupt_unlocked, then the hw_init bypass loop). */
static int masters_set(struct a733_iommu_owner *o, uint16_t masters, uint16_t translate)
{
    for(unsigned id = 0; id < A733_IOMMU_OWNER_MASTERS; ++id)
        if((masters >> id & 1) && set(o, micro(id) + MIC_INT_ENABLE, 0) != 1) return -1;
    for(unsigned id = 0; id < A733_IOMMU_OWNER_MASTERS; ++id)
        if((masters >> id & 1) && set(o, micro(id) + MIC_BYPASS, (translate >> id & 1) ? 0 : 1) != 1) return -1;
    return 1;
}
/* The entry record: both banks' enable, interrupt and walk status words, and the micro status and bypass words of the
 * masters the first attach touches. A bank already enabled or any status word set refuses the attach. Nothing is
 * written. */
static int inherited(struct a733_iommu_owner *o, uint16_t touched)
{
    struct a733_iommu_owner_entry *e = &o->entry;
    memset(e, 0, sizeof(*e));
    for(unsigned b = 0; b < 2; ++b) {
        const uint32_t base = b * BANK;
        if(rd(o, base + ENABLE, &e->enable[b]) != 1 || rd(o, base + INT_STATUS, &e->status[b]) != 1 ||
           rd(o, base + L1_STATUS, &e->l1[b]) != 1 || rd(o, base + L2_STATUS, &e->l2[b]) != 1) return -1;
    }
    for(unsigned id = 0; id < A733_IOMMU_OWNER_MASTERS; ++id)
        if((touched >> id & 1) && (rd(o, micro(id) + MIC_INT_STATUS, &e->micro_status[id]) != 1 ||
                                   rd(o, micro(id) + MIC_BYPASS, &e->bypass[id]) != 1)) return -1;
    e->recorded = 1;
    uint32_t any = 0;
    for(unsigned b = 0; b < 2; ++b) any |= e->enable[b] | e->status[b] | e->l1[b] | e->l2[b];
    for(unsigned id = 0; id < A733_IOMMU_OWNER_MASTERS; ++id) any |= e->micro_status[id];
    return any ? 0 : 1;
}
/* sunxi_iommu_hw_init: the directory in both banks' TTB, prefetch, range invalidation modes, interrupts, the
 * masters' bypass, a full flush, auto gating, then enable. */
static int install(struct a733_iommu_owner *o, uint16_t touched, uint16_t masters)
{
    return both(o, TTB, (uint32_t)(o->directory_physical >> 14)) == 1 && both(o, PREFETCH, PREFETCH_VALUE) == 1 &&
        both(o, TLB_MODE, 1) == 1 && both(o, PC_MODE, 1) == 1 && both(o, INT_ENABLE, 0) == 1 &&
        masters_set(o, touched, masters) == 1 && flush(o, touched) == 1 &&
        both(o, AUTO_GATING, AUTO_GATING_VALUE) == 1 && both(o, ENABLE, 1) == 1 ? 1 : -1;
}
static void release_tables(struct a733_iommu_owner *o)
{
    if(!o->directory) return;
    for(unsigned i = 0; i < DIRECTORY_WORDS; ++i)
        if(aliases(o)[i]) o->ops.release(o->context, aliases(o)[i], physical_of(o->directory[i]), A733_IOMMU_OWNER_L2_BYTES);
    o->ops.release(o->context, o->directory, o->directory_physical, A733_IOMMU_OWNER_DIRECTORY_BYTES + ALIAS_BYTES);
    o->directory = NULL; o->directory_physical = 0; o->tables = 0; o->entries = 0;
}
int a733_iommu_owner_attach(struct a733_iommu_owner *o, uint16_t masters, const struct a733_iommu_span *s, unsigned count)
{
    if(!o || !o->initialized || o->state == A733_IOMMU_OWNER_FAULT || !real(masters) || (masters & o->attached) ||
       !spans_free(o, s, count)) return -1;
    o->refused_word = 0;
    if(o->state == A733_IOMMU_OWNER_ACTIVE) {
        /* sunxi_iommu_map, its sync, then the new masters' interrupt enable and bypass
         * (sunxi_enable_device_iommu and sunxi_iommu_master_ready): no flush of the banks other masters use. */
        if(write_spans(o, s, count) != 1) return -1;
        for(unsigned i = 0; i < count; ++i) if(invalidate(o, s[i].iova, s[i].bytes, MAP_TAIL) != 1) return -1;
        if(masters_set(o, masters, masters) != 1) return -1;
        o->attached |= masters; o->translated |= masters;
        return 1;
    }
    /* First attach: the clock lease, the inherited state, then one directory. The always-bypassed masters (USB,
     * which has no power domain and stays live) are set to bypass with the attached ones translated. */
    const uint16_t touched = (uint16_t)(masters | A733_IOMMU_OWNER_BYPASSED);
    if(o->ops.clock(o->context, 1) != 1) { o->refused_word = 1; return -1; }
    o->clock_held = 1;
    /* Attached from here, so a failure below leaves detach able to bypass, disable and release. */
    o->attached = masters; o->translated = masters;
    const int quiet = inherited(o, touched);
    if(quiet != 1) {
        if(quiet == 0) {
            o->refused_word = 2; o->attached = 0; o->translated = 0;
            (void)o->ops.clock(o->context, 0); o->clock_held = 0;
        }
        return -1;
    }
    uint32_t *cpu; uint64_t pa;
    if(o->ops.table(o->context, A733_IOMMU_OWNER_DIRECTORY_BYTES + ALIAS_BYTES, A733_IOMMU_OWNER_DIRECTORY_BYTES, &cpu, &pa) != 1 ||
       !cpu || (pa & (A733_IOMMU_OWNER_DIRECTORY_BYTES - 1)) || pa < UINT64_C(0x40000000) || pa >= A733_IOMMU_PHYSICAL_LIMIT)
        return fail(o, STEP_TABLE, 0xfff);
    memset(cpu, 0, A733_IOMMU_OWNER_DIRECTORY_BYTES + ALIAS_BYTES);
    o->directory = cpu; o->directory_physical = pa;
    if(write_spans(o, s, count) != 1 || install(o, touched, masters) != 1) return -1;
    o->state = A733_IOMMU_OWNER_ACTIVE;
    return 1;
}
int a733_iommu_owner_map(struct a733_iommu_owner *o, const struct a733_iommu_span *s, unsigned count)
{
    if(!o || o->state != A733_IOMMU_OWNER_ACTIVE || !spans_free(o, s, count)) return -1;
    if(write_spans(o, s, count) != 1) return -1;
    for(unsigned i = 0; i < count; ++i) if(invalidate(o, s[i].iova, s[i].bytes, MAP_TAIL) != 1) return -1;
    return 1;
}
int a733_iommu_owner_unmap(struct a733_iommu_owner *o, const struct a733_iommu_span *s, unsigned count)
{
    if(!o || o->state != A733_IOMMU_OWNER_ACTIVE || !s || !count) return -1;
    for(unsigned i = 0; i < count; ++i) {
        if(!span_valid(&s[i])) return -1;
        for(uint64_t off = 0; off < s[i].bytes; off += PAGE) {
            const uint32_t *e = entry_of(o, s[i].iova + off);
            if(!e || *e != leaf(s[i].physical + off, s[i].access)) return -1;
        }
    }
    for(unsigned i = 0; i < count; ++i)
        for(uint64_t off = 0; off < s[i].bytes; off += PAGE) { *entry_of(o, s[i].iova + off) = 0; --o->entries; }
    if(o->ops.publish(o->context) != 1) return fail(o, STEP_PUBLISH, 1);
    for(unsigned i = 0; i < count; ++i) if(invalidate(o, s[i].iova, s[i].bytes, 0) != 1) return -1;
    return 1;
}
int a733_iommu_owner_detach(struct a733_iommu_owner *o, uint16_t masters)
{
    if(!o || o->state == A733_IOMMU_OWNER_IDLE || !masters || (masters & ~o->attached)) return -1;
    for(unsigned id = 0; id < A733_IOMMU_OWNER_MASTERS; ++id)
        if((masters >> id & 1) && set(o, micro(id) + MIC_BYPASS, 1) != 1) return -1;
    o->attached &= (uint16_t)~masters; o->translated &= (uint16_t)~masters;
    /* The vendor's detach changes no register; returning a drained master to bypass is the owner's. */
    if(o->attached) return 1;
    /* The last master: flush every master the first attach touched, disable both banks, then release the tables and the
     * clock lease. The vendor has no disable path (its suspend gates the clocks). A bank that does not read back
     * disabled keeps its tables and the lease, since it may still walk them. */
    if(flush(o, (uint16_t)(masters | A733_IOMMU_OWNER_BYPASSED)) != 1 || both(o, ENABLE, 0) != 1) return -1;
    release_tables(o);
    if(o->ops.clock(o->context, 0) != 1) return fail(o, STEP_CLOCK, 0);
    o->clock_held = 0;
    if(o->state == A733_IOMMU_OWNER_ACTIVE) o->state = A733_IOMMU_OWNER_IDLE;
    return 1;
}
int a733_iommu_owner_scan(struct a733_iommu_owner *o, unsigned id)
{
    if(!o || o->state == A733_IOMMU_OWNER_IDLE || id >= A733_IOMMU_OWNER_MASTERS || !(o->attached >> id & 1)) return -1;
    struct a733_iommu_owner_fault f = {0};
    for(unsigned b = 0; b < 2; ++b)
        if(rd(o, b * BANK + INT_STATUS, &f.bank_status[b]) != 1 || rd(o, b * BANK + L1_STATUS, &f.l1_status[b]) != 1 ||
           rd(o, b * BANK + L2_STATUS, &f.l2_status[b]) != 1) return -1;
    const uint32_t m = micro(id);
    if(rd(o, m + MIC_INT_STATUS, &f.micro_status) != 1 || rd(o, m + MIC_INT_STATUS + 4, &f.micro_low) != 1 ||
       rd(o, m + MIC_INT_STATUS + 8, &f.micro_high) != 1 || rd(o, m + MIC_INT_STATUS + 12, &f.micro_data) != 1) return -1;
    o->faults[id] = f;
    const int quiet = !(f.bank_status[0] | f.bank_status[1] | f.l1_status[0] | f.l1_status[1] | f.l2_status[0] |
                        f.l2_status[1] | f.micro_status);
    if(!quiet) o->faulted |= (uint16_t)(1u << id);
    return quiet;
}
int a733_iommu_owner_translate(const struct a733_iommu_owner *o, uint64_t iova, unsigned access, uint64_t *pa)
{
    if(!o || !pa || iova >= (UINT64_C(1) << 34) || !access || (access & ~3u) || !o->directory) return -1;
    const uint32_t d = o->directory[iova >> 22];
    if(!(d & 1)) return -1;
    const uint32_t *t = aliases(o)[iova >> 22];
    if(!t || physical_of(d) == 0) return -1;
    const uint32_t e = t[(iova >> 12) & (L2_WORDS - 1)];
    const unsigned granted = ((e & 4u) ? A733_IOMMU_READ : 0) | ((e & 8u) ? A733_IOMMU_WRITE : 0);
    if(!(e & 2u) || (granted & access) != access) return -1;
    *pa = physical_of(e) | (iova & (PAGE - 1));
    return 1;
}
