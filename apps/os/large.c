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
extern const uint64_t a733_os_kernel_l1[512] __attribute__((weak));

/* The allocator hook of the C library (runtime/newlib.c); absent in an image without one. */
extern int runtime_heap_attach(uintptr_t base, size_t bytes) __attribute__((weak));

/* The board's report of an admitted region, for consoles that show it; the default is silent. */
__attribute__((weak)) void os_large_report(const struct large_slot *slot, const struct app_space *space)
{ (void)slot; (void)space; }

/* The application's ASID, taken once from the pool and never given back (include/app_region.h). */
static struct app_asid_pool asids;
static uint16_t owner_asid;
/* Sized by the image's generated tables from the manifest. */
extern const unsigned os_large_capacity;
extern struct large_slot os_large_slot_storage[];
extern struct app_region_entry os_large_entry_storage[];
static struct app_region_ledger ledger;
static struct app_region_range usable_storage[APP_REGION_SLOTS];
/* One space: the root and a level-2 table per upper slot, the most it can need (app_region.h). */
static uint64_t arena_pages[APP_SPACE_PAGES][512] __attribute__((aligned(4096)));
static struct app_table_arena arena;
static struct large_slot *const slots = os_large_slot_storage;
static struct app_space space;
static unsigned slot_count;
static int admitted;

static void install_space(uint64_t kernel_ttbr);
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
    /* On the board the kernel's table lies in the monitor interval, which its own tables leave invalid at EL1, so
     * EL1 reads the copy that the table builder made. The virtual board maps its table and has no copy. */
    const uint64_t *base_l1 = a733_os_kernel_l1 ? a733_os_kernel_l1 : (const uint64_t *)(uintptr_t)(ttbr & APP_PTE_ADDR_MASK);
    struct app_region_range usable[APP_REGION_SLOTS];
    unsigned ranges = usable_ranges(base_l1, usable, APP_REGION_SLOTS);
    uint64_t mmfr0, tcr;
    __asm__ volatile("mrs %0, id_aa64mmfr0_el1\n mrs %1, tcr_el1" : "=r"(mmfr0), "=r"(tcr));
    app_asid_pool_init(&asids, mmfr0, tcr);
    owner_asid = app_asid_alloc(&asids);
    if (!owner_asid || !ranges || count > os_large_capacity ||
        app_region_ledger_init(&ledger, usable_storage, APP_REGION_SLOTS, os_large_entry_storage, os_large_capacity, usable, ranges)) return 0;
    if (large_admit(&ledger, owner_asid, __start_sunstone_large_regions, count, slots, os_large_capacity)) return 0;
    arena = (struct app_table_arena){arena_pages, (uintptr_t)arena_pages, APP_SPACE_PAGES, 0};
    int built = app_space_build(&space, &arena, base_l1, &ledger, owner_asid) == 0;
    /* The heap record, when the manifest has one, becomes the C library's heap; an image whose library cannot
     * take it refuses the application rather than run with a heap of another size. */
    for (unsigned i = 0; built && i < count; ++i)
        if ((slots[i].flags & SS_LARGE_HEAP) && !(runtime_heap_attach && runtime_heap_attach((uintptr_t)slots[i].base, (size_t)slots[i].bytes)))
            built = 0;
    if (!built) {
        for (unsigned i = 0; i < count; ++i) if (slots[i].bytes) app_region_release(&ledger, owner_asid, slots[i].base);
        memset(slots, 0, sizeof *slots * count);
        return 0;
    }
    slot_count = count;
    admitted = 1;
    install_space((uint64_t)ttbr);
    for (unsigned i = 0; i < count; ++i) os_large_report(&slots[i], &space);
    return 1;
}

/* ---- The task-switch hook's state, the SDK calls and the waiters ---- */
#include "FreeRTOS.h"
#include "task.h"
#include "os_space.h"
#include "sunstone_app.h"
#include <stdatomic.h>

uint64_t os_space_kernel_ttbr;
uint8_t os_space_active;
static uint64_t owner_ttbr;          /* the application's tag points here */

/* Tasks of the application that wait for a region: a task waits on one region at a time, so the list holds
 * one entry per task of the application (the main task and its workers), sized by the generated tables. */
extern const unsigned os_large_waiter_capacity;
extern TaskHandle_t os_large_waiter_task[];
extern unsigned os_large_waiter_region[];

struct large_slot *os_large_slot_mutable(unsigned index) { return index < slot_count ? &slots[index] : 0; }
const struct ss_large_region *os_large_record(unsigned index)
{ return index < slot_count ? &__start_sunstone_large_regions[index] : 0; }

/* The space's tag value; the hook installs it at the switch to a tagged task. */
static void install_space(uint64_t kernel_ttbr)
{
    os_space_kernel_ttbr = kernel_ttbr;
    owner_ttbr = app_space_ttbr0(&space);
    __atomic_store_n(&os_space_active, 1, __ATOMIC_RELEASE);
}
const uint64_t *os_large_tag(void) { return admitted ? &owner_ttbr : 0; }

void ss_large_attach(void *task)
{
    if (admitted && task) vTaskSetApplicationTaskTag((TaskHandle_t)task, (TaskHookFunction_t)(uintptr_t)&owner_ttbr);
}

static unsigned state_of(unsigned i) { return __atomic_load_n(&slots[i].state, __ATOMIC_ACQUIRE); }

void *ss_large_base(unsigned index, size_t *bytes)
{
    if (index >= slot_count || state_of(index) != SS_LARGE_READY) return 0;
    if (bytes) *bytes = (size_t)slots[index].bytes;
    return (void *)(uintptr_t)slots[index].base;
}

int ss_large_status(unsigned index, struct ss_large_status *out)
{
    if (index >= slot_count || !out) return -1;
    out->state = state_of(index);
    out->error = slots[index].error;
    out->loaded = __atomic_load_n(&slots[index].loaded, __ATOMIC_RELAXED);
    out->total = slots[index].total;
    uint64_t end = slots[index].finished_us ? slots[index].finished_us : 0;
    out->elapsed_us = end ? end - slots[index].started_us : 0;
    return 0;
}

void os_large_finish(unsigned index, unsigned state, uint32_t error)
{
    if (index >= slot_count) return;
    slots[index].error = error;
    __atomic_store_n(&slots[index].state, state, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < os_large_waiter_capacity; ++i)
        if (os_large_waiter_task[i] && os_large_waiter_region[i] == index) xTaskNotifyGive(os_large_waiter_task[i]);
}

int ss_large_wait(unsigned index, uint64_t timeout_us)
{
    if (index >= slot_count) return -1;
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    unsigned entry = os_large_waiter_capacity;
    taskENTER_CRITICAL();
    for (unsigned i = 0; i < os_large_waiter_capacity && entry == os_large_waiter_capacity; ++i)
        if (!os_large_waiter_task[i]) { os_large_waiter_task[i] = self; os_large_waiter_region[i] = index; entry = i; }
    taskEXIT_CRITICAL();
    if (entry == os_large_waiter_capacity) return -1;            /* more waiting tasks than the application has */
    TickType_t ticks = (TickType_t)((timeout_us + 999) / 1000), start = xTaskGetTickCount();
    int result = 0;
    for (;;) {
        unsigned state = state_of(index);
        if (state == SS_LARGE_READY) { result = 1; break; }
        if (state == SS_LARGE_FAILED) { result = -1; break; }
        TickType_t used = xTaskGetTickCount() - start;
        if (used >= ticks) break;
        (void)ulTaskNotifyTake(pdTRUE, ticks - used);
    }
    taskENTER_CRITICAL();
    os_large_waiter_task[entry] = 0;
    taskEXIT_CRITICAL();
    return result;
}
