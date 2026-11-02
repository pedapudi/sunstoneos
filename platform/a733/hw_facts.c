/* SPDX-License-Identifier: GPL-2.0-only */
/* Capture of the hardware facts table (include/a733_hw_facts.h, docs/hardware/a733-facts.md).
 *
 * Every core runs hwf_capture at EL1 before the scheduler starts and records
 * its own registers. Core 0 also records the distributor, the shared peripheral interrupts and the
 * NPU admission words. The capture changes no clock, voltage or power domain. It reads registers and
 * makes exactly these writes, each of a value that every image writes in the same boot:
 *   CSSELR_EL1      selects a cache for CCSIDR_EL1; the value on entry is restored.
 *   ICC_PMR_EL1     0xff and the FreeRTOS API mask, the two values the port writes; the value on entry
 *                   is restored.
 *   GICR_IPRIORITYR priority bytes of PPI 27 and PPI 30 on core 0, with the values the platform writes
 *                   next; the value on entry is restored.
 * No other register is written. The device words are read only after the gates that the runner checks. */
#include <stddef.h>
#include "a733_hw_facts.h"
#include "irq_table.h"

struct spec { uint16_t id; uint32_t aux; };
static const struct spec shared_specs[] = {
#define HWF_SHARED(id, aux) {id, aux},
#define HWF_CORE(id, aux)
#include "a733_hw_facts_table.inc"
#undef HWF_SHARED
#undef HWF_CORE
};
static const struct spec core_specs[] = {
#define HWF_SHARED(id, aux)
#define HWF_CORE(id, aux) {id, aux},
#include "a733_hw_facts_table.inc"
#undef HWF_SHARED
#undef HWF_CORE
};
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
_Static_assert(COUNT(shared_specs) <= HWF_SHARED_SLOTS && COUNT(core_specs) <= HWF_CORE_SLOTS, "slot count");

#define MRS(name) ({ uint64_t v_; __asm__ volatile("mrs %0, " #name : "=r"(v_) :: "memory"); v_; })
static inline uint32_t r32(uintptr_t a) { return *(volatile const uint32_t *)a; }
static inline uint64_t r64(uintptr_t a) { return *(volatile const uint64_t *)a; }

static uint64_t pmr_read(void) { return MRS(ICC_PMR_EL1); }
static void pmr_write(uint64_t v) { __asm__ volatile("msr ICC_PMR_EL1, %0\n isb" :: "r"(v) : "memory"); }
static uint64_t ccsidr(unsigned select)
{
    __asm__ volatile("msr csselr_el1, %0\n isb" :: "r"((uint64_t)select) : "memory");
    return MRS(ccsidr_el1);
}
static uint64_t id_register(unsigned id)
{
    switch (id) {
    case 0x0010: return MRS(id_aa64pfr0_el1);
    case 0x0011: return MRS(id_aa64pfr1_el1);
    case 0x0012: return MRS(id_aa64dfr0_el1);
    case 0x0013: return MRS(S3_0_C0_C5_1);
    case 0x0014: return MRS(S3_0_C0_C5_4);
    case 0x0015: return MRS(id_aa64isar0_el1);
    case 0x0016: return MRS(id_aa64isar1_el1);
    case 0x0017: return MRS(S3_0_C0_C6_2);
    case 0x0018: return MRS(id_aa64mmfr0_el1);
    case 0x0019: return MRS(id_aa64mmfr1_el1);
    case 0x001a: return MRS(id_aa64mmfr2_el1);
    case 0x001b: return MRS(S3_0_C0_C7_3);
    case 0x001c: return MRS(S3_0_C0_C4_4);
    default: return MRS(pmcr_el0);
    }
}
static void put(struct hwf_slot *slot, unsigned id, unsigned core, unsigned flags, uint32_t aux, uint64_t value)
{
    slot->id = (uint16_t)id; slot->core = (uint8_t)core; slot->aux = aux; slot->value = value;
    slot->flags = (uint8_t)flags;
}

static void capture_core(unsigned core)
{
    struct hwf_slot *slots = hwf_table.core[core];
    const uintptr_t rd = hwf_platform.redistributor(core), sgi = rd + 0x10000;
    const uint64_t csselr_entry = MRS(csselr_el1);
    const uint64_t pmr_entry = pmr_read();
    uint64_t pmr_ff = 0, pmr_api = 0, pmr_restored = 0;
    /* Reads first, in table order; the PMR writes follow the inherited reads. */
    for (unsigned i = 0; i < COUNT(core_specs); ++i) {
        const unsigned id = core_specs[i].id, aux = core_specs[i].aux;
        uint64_t v = 0; uint32_t out_aux = 0;
        switch (id) {
        case 0x0001: v = MRS(mpidr_el1); break;
        case 0x0002: v = MRS(midr_el1); break;
        case 0x0003: v = MRS(S3_0_C0_C0_6); break;
        case 0x0004: v = MRS(cntfrq_el0); break;
        case 0x0005: v = MRS(ctr_el0); break;
        case 0x0006: v = MRS(clidr_el1); break;
        case 0x0007: v = MRS(dczid_el0); break;
        case 0x0008: v = ccsidr(aux); break;
        case 0x0030: v = MRS(ICC_CTLR_EL1); break;
        case 0x0031: v = pmr_entry; break;
        case 0x0032: v = MRS(ICC_RPR_EL1); break;
        case 0x0033: v = MRS(ICC_SRE_EL1); break;
        case 0x0034: v = MRS(ICC_IGRPEN1_EL1); break;
        case 0x0035: v = MRS(ICC_BPR1_EL1); break;
        case 0x0036: v = MRS(ICC_IGRPEN0_EL1); break;
        case 0x0037: v = MRS(ICC_BPR0_EL1); break;
        case 0x0038:
            out_aux = 0xff; pmr_write(out_aux); v = pmr_ff = pmr_read(); break;
        case 0x0039:
            out_aux = hwf_platform.api_mask; pmr_write(out_aux); v = pmr_api = pmr_read();
            pmr_write(pmr_entry); pmr_restored = pmr_read(); break;
        case 0x0040: v = r64(rd + 0x08); break;
        case 0x0041: v = r32(rd + 0x14); break;
        case 0x0042: v = r32(rd + 0x04); break;
        case 0x0043: v = r32(sgi + 0x80); break;
        case 0x0044: v = r32(sgi + 0xd00); break;
        case 0x0045: v = r32(sgi + 0x100); break;
        case 0x0046: v = r32(sgi + 0x200); break;
        case 0x0047: v = r32(sgi + 0x300); break;
        case 0x0048: v = r32(sgi + 0xc00); break;
        case 0x0049: v = r32(sgi + 0xc04); break;
        case 0x004a: v = r32(sgi + 0x400 + 4u * aux); break;
        default: v = id_register(id); break;
        }
        put(&slots[i], id, core, HWF_VALID, out_aux, v);
    }
    __asm__ volatile("msr csselr_el1, %0\n isb" :: "r"(csselr_entry) : "memory");
    (void)pmr_ff; (void)pmr_api;
    if (pmr_restored != pmr_entry)
        for (unsigned i = 0; i < COUNT(core_specs); ++i)
            if (core_specs[i].id == 0x0038 || core_specs[i].id == 0x0039) slots[i].flags |= HWF_ANOMALY;
}

static uint32_t route_requested(unsigned intid)
{
    for (unsigned i = 0; i < a733_irq_route_count; ++i)
        if (a733_irq_routes[i].intid == intid)
            return 1u << 16 | (uint32_t)a733_irq_routes[i].core << 8 | a733_irq_routes[i].priority;
    return 0;
}

static void capture_shared(unsigned core)
{
    const uintptr_t d = hwf_platform.distributor, sgi = hwf_platform.redistributor(core) + 0x10000;
    const unsigned lines = ((r32(d + 4) & 31u) + 1u) * 32u;
    const uint32_t gates[3] = {hwf_platform.read32(0x070101ac), hwf_platform.read32(0x020025c0), hwf_platform.read32(0x020025e0)};
    const int ppu_open = (gates[0] & 1u) && (gates[1] & (1u << 6)) && (gates[2] & (1u << 18));
    for (unsigned i = 0; i < COUNT(shared_specs); ++i) {
        const unsigned id = shared_specs[i].id, aux = shared_specs[i].aux;
        struct hwf_slot *slot = &hwf_table.shared[i];
        uint64_t v = 0; uint32_t out_aux = 0, flags = HWF_VALID;
        switch (id) {
        case 0x0100: v = r32(d); break;
        case 0x0101: v = r32(d + 4); break;
        case 0x0102: v = r32(d + 8); break;
        case 0x0103: v = r32(d + 0xc); break;
        case 0x0104: v = r32(d + 0xffe8); break;
        case 0x0110:
            if (aux >= lines) { flags = HWF_SKIPPED; break; }
            v = (r32(d + 0x400 + (aux & ~3u)) >> (8 * (aux & 3u))) & 0xffu;
            v |= (uint64_t)((r32(d + 0x80 + 4 * (aux / 32)) >> (aux % 32)) & 1u) << 8;
            v |= (uint64_t)((r32(d + 0xd00 + 4 * (aux / 32)) >> (aux % 32)) & 1u) << 9;
            v |= (uint64_t)((r32(d + 0x100 + 4 * (aux / 32)) >> (aux % 32)) & 1u) << 10;
            v |= (uint64_t)((r32(d + 0x200 + 4 * (aux / 32)) >> (aux % 32)) & 1u) << 11;
            v |= (uint64_t)((r32(d + 0x300 + 4 * (aux / 32)) >> (aux % 32)) & 1u) << 12;
            v |= (uint64_t)((r32(d + 0xc00 + 4 * (aux / 16)) >> (2 * (aux % 16))) & 3u) << 13;
            break;
        case 0x0111:
            if (aux >= lines) flags = HWF_SKIPPED; else v = r64(d + 0x6000 + 8u * aux);
            break;
        case 0x0112: v = route_requested(aux); break;
        case 0x0130:
            if ((aux == 0x07064000 || aux == 0x07064008) && !ppu_open) { flags = HWF_SKIPPED; break; }
            v = hwf_platform.read32(aux); break;
        case 0x0140: {
            /* The priority byte of a PPI on this core: read, write the value the platform writes next,
             * read back, restore. */
            volatile uint8_t *byte = (volatile uint8_t *)(sgi + 0x400 + aux);
            const uint8_t entry = *byte, wrote = aux == 27 ? 0xd8 : (uint8_t)hwf_platform.ppi30_priority;
            *byte = wrote; __asm__ volatile("dsb sy" ::: "memory");
            v = *byte; out_aux = wrote;
            *byte = entry; __asm__ volatile("dsb sy" ::: "memory");
            if (*byte != entry) flags |= HWF_ANOMALY;
            break; }
        default: flags = HWF_SKIPPED; break;
        }
        put(slot, id, 0, flags, out_aux, flags & HWF_SKIPPED ? 0 : v);
    }
}

void hwf_capture(unsigned core)
{
    if (core >= HWF_CORES) return;
    capture_core(core);
    if (!core) {
        hwf_table.header.platform = hwf_platform.platform;
        capture_shared(core);
    }
    __atomic_fetch_or(&hwf_table.header.done_mask, 1u << core, __ATOMIC_RELEASE);
}

/* Stub handler for the interrupt routes that the facts image installs only so that their Non-secure views
 * can be read; the interrupts stay disabled. */
void os_hw_facts_irq(void) {}
