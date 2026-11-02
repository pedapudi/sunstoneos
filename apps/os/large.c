/* Admission of the application's large regions through the ledger (docs/designs/large-app-regions.md).
 * It runs once, on one core, before the scheduler starts. The records come from the section
 * "sunstone_large_regions"; the usable upper memory comes from the board: the ranges that the DRAM
 * decode check passed. */
#include "app_large.h"
#include "app_region.h"
#include <stdint.h>
#include <string.h>

extern const struct ss_large_region __start_sunstone_large_regions[] __attribute__((weak));
extern const struct ss_large_region __stop_sunstone_large_regions[] __attribute__((weak));

/* The usable upper memory is what the installed kernel table maps: each level-1 slot from 0x80000000 that holds
 * the EL1-only non-global block. The table builder maps them only when the boot's DRAM decode check passed
 * (platform/a733-smp-cached/memory.c), so an image whose check fails has no usable range. */
static unsigned usable_ranges(const uint64_t *base_l1, struct app_region_range *out, unsigned capacity)
{
    unsigned count = 0;
    for (unsigned slot = 2; slot < 13; ++slot) {
        uint64_t block = (UINT64_C(1) << 30) * slot;
        int mapped = base_l1[slot] == (block | APP_PTE_VALID | APP_PTE_ATTR_WB | APP_PTE_SH_INNER | APP_PTE_AF | APP_PTE_NG |
                                       APP_PTE_PXN | APP_PTE_UXN);
        if (!mapped) continue;
        if (count && out[count - 1].base + out[count - 1].bytes == block) out[count - 1].bytes += UINT64_C(1) << 30;
        else if (count < capacity) out[count++] = (struct app_region_range){block, UINT64_C(1) << 30};
        else return 0;
    }
    return count;
}
/* The board's report of an admitted region, for consoles that show it; the default is silent. */
__attribute__((weak)) void os_large_report(const struct large_slot *slot, const struct app_space *space)
{ (void)slot; (void)space; }

#define OS_LARGE_OWNER 1u
static struct app_region_ledger ledger;
static uint64_t arena_pages[8][512] __attribute__((aligned(4096)));
static struct app_table_arena arena;
static struct large_slot slots[SS_LARGE_REGIONS];
static struct app_space space;
static unsigned slot_count;
static int admitted;

unsigned os_large_count(void) { return slot_count; }
const struct large_slot *os_large_slot(unsigned index) { return index < slot_count ? &slots[index] : 0; }
const struct app_space *os_large_space(void) { return admitted ? &space : 0; }

int os_large_prepare(void)
{
    unsigned count = __start_sunstone_large_regions ? (unsigned)(__stop_sunstone_large_regions - __start_sunstone_large_regions) : 0;
    if (!count) return 1;
    if (admitted) return 0;
    uint64_t ttbr;
    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(ttbr));
    const uint64_t *base_l1 = (const uint64_t *)(uintptr_t)(ttbr & APP_PTE_ADDR_MASK);
    struct app_region_range usable[APP_REGION_USABLE_MAX];
    unsigned ranges = usable_ranges(base_l1, usable, APP_REGION_USABLE_MAX);
    if (!ranges || ranges > APP_REGION_USABLE_MAX || app_region_ledger_init(&ledger, usable, ranges)) return 0;
    if (large_admit(&ledger, OS_LARGE_OWNER, __start_sunstone_large_regions, count, slots)) return 0;
    arena = (struct app_table_arena){arena_pages, (uintptr_t)arena_pages, 8, 0};
    if (app_space_build(&space, &arena, base_l1, &ledger, OS_LARGE_OWNER)) {
        for (unsigned i = 0; i < SS_LARGE_REGIONS; ++i) if (slots[i].bytes) app_region_release(&ledger, OS_LARGE_OWNER, slots[i].base);
        memset(slots, 0, sizeof slots);
        return 0;
    }
    slot_count = count;
    admitted = 1;
    for (unsigned i = 0; i < count; ++i) os_large_report(&slots[i], &space);
    return 1;
}
