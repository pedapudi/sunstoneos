/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_A733_MAPPING_CHECK_H
#define SUNSTONE_A733_MAPPING_CHECK_H
/* Stage-1 mapping checks over a translation callback.
 *
 * Native code passes aarch64_device_translate(), which issues AT S1E1R or
 * AT S1E1W and decodes PAR_EL1. Host checks pass a model with the same
 * signature. A rule states which translations are made, which MAIR attribute
 * values are accepted with which shareability values, whether read and write
 * shareability must match, and whether the physical address is compared.
 * Callers keep their extent, role and pool checks; the rule replaces only the
 * per-address translation test. */
#include "aarch64_device_memory.h"
#include <stddef.h>
#include <stdint.h>

typedef int (*a733_translate_fn)(uintptr_t address, int write, struct aarch64_translation *result);

/* MAIR attribute values that the project's page tables use. */
#define A733_MAIR_DEVICE_NGNRNE 0x00u
#define A733_MAIR_DEVICE_NGNRE 0x04u
#define A733_MAIR_NORMAL_NC 0x44u
#define A733_MAIR_NORMAL_WB 0xffu

/* Accepted PAR_EL1.SH values as a mask: bit n accepts SH == n. */
#define A733_SH_NON (1u << 0)
#define A733_SH_OUTER (1u << 2)
#define A733_SH_INNER (1u << 3)
#define A733_SH_ANY 0xfu

enum a733_mapping_access {
    A733_MAPPING_READ = 1, A733_MAPPING_WRITE = 2, A733_MAPPING_READ_WRITE = 3
};
struct a733_mapping_accept {
    uint8_t attributes;   /* exact MAIR attribute value */
    uint8_t shareability; /* A733_SH_* mask accepted with this attribute */
};
struct a733_mapping_rule {
    enum a733_mapping_access access;
    unsigned count;                        /* 1 or 2 entries in accept */
    struct a733_mapping_accept accept[2];
    unsigned same_shareability;            /* READ_WRITE: read SH equals write SH */
    unsigned compare_physical;             /* physical address must match */
};

/* One translation result satisfies the rule's attribute and shareability
 * entries and, when compare_physical is set, translates to `physical`. */
static inline int a733_mapping_translation_ok(const struct aarch64_translation *t, uint64_t physical,
                                              const struct a733_mapping_rule *rule)
{
    if (rule->compare_physical && t->physical != physical) return 0;
    if (t->shareability > 3) return 0;
    for (unsigned i = 0; i < rule->count && i < 2; ++i)
        if (t->attributes == rule->accept[i].attributes &&
            (rule->accept[i].shareability >> t->shareability & 1u)) return 1;
    return 0;
}

/* Translate one address as the rule requires: read before write for
 * READ_WRITE. Both translations must pass, carry the same attribute value,
 * and, when same_shareability is set, the same shareability. */
static inline int a733_mapping_check(a733_translate_fn translate, uintptr_t cpu, uint64_t physical,
                                     const struct a733_mapping_rule *rule)
{
    struct aarch64_translation r = {0}, w = {0};
    unsigned access = rule->access;
    if (!translate || !access || access > A733_MAPPING_READ_WRITE) return 0;
    if ((access & A733_MAPPING_READ) &&
        (!translate(cpu, 0, &r) || !a733_mapping_translation_ok(&r, physical, rule))) return 0;
    if ((access & A733_MAPPING_WRITE) &&
        (!translate(cpu, 1, &w) || !a733_mapping_translation_ok(&w, physical, rule))) return 0;
    if (access == A733_MAPPING_READ_WRITE &&
        (r.attributes != w.attributes || (rule->same_shareability && r.shareability != w.shareability)))
        return 0;
    return 1;
}

/* Check cpu, cpu + step, cpu + 2 * step, ... below cpu + bytes, each against
 * physical plus the same offset. The walk starts at cpu itself, so with an
 * unaligned cpu and step 4096 the last partial page is checked only when an
 * address of the walk falls in it. Refuses zero bytes, zero step and a range
 * whose exclusive end does not fit. */
static inline int a733_mapping_check_stride(a733_translate_fn translate, uintptr_t cpu, uint64_t physical,
                                            size_t bytes, size_t step, const struct a733_mapping_rule *rule)
{
    if (!bytes || !step || bytes > UINTPTR_MAX - cpu) return 0;
    for (size_t offset = 0;; offset += step) {
        if (!a733_mapping_check(translate, cpu + offset, physical + offset, rule)) return 0;
        if (bytes - offset <= step) return 1;
    }
}

/* Check every 4 KiB page that holds a byte of [cpu, cpu + bytes): cpu itself,
 * then each following page base through the page of the last byte. Every
 * address of one 4 KiB page translates with the same attributes, so checking
 * cpu instead of its page base gives the same result. The range may end at the
 * top byte. Refuses zero bytes and a range whose last byte does not fit. */
static inline int a733_mapping_check_pages(a733_translate_fn translate, uintptr_t cpu, uint64_t physical,
                                           size_t bytes, const struct a733_mapping_rule *rule)
{
    if (!bytes || bytes - 1 > UINTPTR_MAX - cpu) return 0;
    uintptr_t last = cpu + (bytes - 1);
    for (uintptr_t address = cpu;; address = (address & ~(uintptr_t)4095) + 4096) {
        if (!a733_mapping_check(translate, address, physical + (address - cpu), rule)) return 0;
        if ((address >> 12) == (last >> 12)) return 1;
    }
}
#endif
