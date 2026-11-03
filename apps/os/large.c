/* Admission of the application's large regions through the ledger (docs/designs/large-app-regions.md).
 * It runs once, on one core, before the scheduler starts. The records come from the section
 * "sunstone_large_regions"; the usable upper memory comes from the board: the ranges that the DRAM
 * decode check passed. */
#include "app_large.h"
#include "../board/application.h"
#include "sunstone_crumb.h"
#include "application.h"
#include "sunstone_app.h"
#include "os_space.h"
#include "app_region.h"
#include "sunstone_notify.h"
#include <stdint.h>
#include <string.h>

extern const struct ss_large_region __start_sunstone_large_regions[] __attribute__((weak));
extern const struct ss_large_region __stop_sunstone_large_regions[] __attribute__((weak));

/* The usable upper memory is what the installed kernel tables map as the EL1-only non-global block: the 2 MiB blocks
 * of level-1 slot 1 from 0x48000000 (the first 128 MiB are the window every space shares) and each level-1 slot from
 * 0x80000000. The table builder maps them only when the boot's DRAM decode check passed
 * (platform/a733-smp-cached/memory.c), so an image whose check fails has no usable range. */
#define KERNEL_BLOCK(pa) ((pa) | APP_PTE_VALID | APP_PTE_ATTR_WB | APP_PTE_SH_INNER | APP_PTE_AF | APP_PTE_NG | \
                          APP_PTE_PXN | APP_PTE_UXN)
static int add_range(struct app_region_range *out, unsigned *count, unsigned capacity, uint64_t base, uint64_t bytes)
{
    if (*count && out[*count - 1].base + out[*count - 1].bytes == base) out[*count - 1].bytes += bytes;
    else if (*count < capacity) out[(*count)++] = (struct app_region_range){base, bytes};
    else return 0;
    return 1;
}
static unsigned usable_ranges(const uint64_t *base_l1, const uint64_t *base_l2, struct app_region_range *out, unsigned capacity)
{
    unsigned count = 0;
    for (uint64_t pa = APP_REGION_FLOOR; pa < APP_REGION_BASE; pa += APP_REGION_BLOCK)
        if (base_l2[(pa - UINT64_C(0x40000000)) / APP_REGION_BLOCK] == KERNEL_BLOCK(pa) &&
            !add_range(out, &count, capacity, pa, APP_REGION_BLOCK)) return 0;
    for (unsigned slot = 2; slot < 13; ++slot) {
        uint64_t block = (UINT64_C(1) << 30) * slot;
        if (base_l1[slot] == KERNEL_BLOCK(block) && !add_range(out, &count, capacity, block, UINT64_C(1) << 30)) return 0;
    }
    return count;
}
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
extern const unsigned os_ledger_capacity;
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
static void *main_stack;
static uint64_t kernel_base, kernel_bytes;
int os_large_kernel_reserved(uint64_t base, uint64_t bytes) { return admitted && kernel_bytes && kernel_base == base && kernel_bytes == bytes; }
void *os_large_main_stack(void) { return main_stack; }

static void install_space(uint64_t kernel_ttbr);
/* The eight-core images walk their tables without the data caches (TCR_EL1 IRGN0 = ORGN0 = 0,
 * include/a733_cpu_coherence.h), so a descriptor the CPU wrote stays invisible to the walker until its line reaches
 * memory. Clean and invalidate every line of the built tables before any task uses the space; a space built long
 * before its first use would have been evicted by then, and one built just before it is not. */
static void clean_tables(const void *base, size_t bytes)
{
    for (uintptr_t a = (uintptr_t)base & ~(uintptr_t)63; a < (uintptr_t)base + bytes; a += 64)
        __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}
unsigned os_large_count(void) { return slot_count; }
const struct large_slot *os_large_slot(unsigned index) { return index < slot_count ? &slots[index] : 0; }
const struct app_space *os_large_space(void) { return admitted ? &space : 0; }

/* A refusal leaves the reason in the failure record (the value is 0x2000 + the number of the refusing step). */
int os_large_prepare(void);
static int large_failed(unsigned step)
{
    board_fail_note(SUNSTONE_CRUMB_FAIL_SITE(BOARD), 0x2000u + step, (const void *)os_large_prepare);
    return 0;
}
int os_large_prepare(void)
{
    unsigned count = __start_sunstone_large_regions ? (unsigned)(__stop_sunstone_large_regions - __start_sunstone_large_regions) : 0;
    /* The manifest's own regions that the build placed in the slot 1 area belong to this application's space. */
    const struct ss_manifest *manifest = ss_application.manifest;
    unsigned placed = 0;
    for (unsigned i = 0; manifest && i < manifest->region_count; ++i)
        placed += manifest->regions[i].address >= APP_REGION_FLOOR;
    if (admitted) return large_failed(1);
    uint64_t ttbr;
    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(ttbr));
    /* The table's pages may be unmapped at EL1, so its entries come from the platform's copy (os_space.h). */
    static uint64_t base_l1[512], base_l2[512];
    os_kernel_l1_head(base_l1);
    os_kernel_l2_slot1(base_l2);
    struct app_region_range usable[APP_REGION_SLOTS];
    unsigned ranges = usable_ranges(base_l1, base_l2, usable, APP_REGION_SLOTS);
    /* An application with upper memory to use also takes its main stack from it, with a guard block below. */
    if (!count && !placed && !(manifest && ranges)) return 1;
    uint64_t mmfr0, tcr;
    __asm__ volatile("mrs %0, id_aa64mmfr0_el1\n mrs %1, tcr_el1" : "=r"(mmfr0), "=r"(tcr));
    app_asid_pool_init(&asids, mmfr0, tcr);
    owner_asid = app_asid_alloc(&asids);
    if (!owner_asid || !ranges || count > os_large_capacity ||
        app_region_ledger_init(&ledger, usable_storage, APP_REGION_SLOTS, os_large_entry_storage, os_ledger_capacity, usable, ranges)) return large_failed(2 + (!owner_asid ? 0 : !ranges ? 1 : 2));
#ifdef OS_NPU_POOL_BASE
    /* The NPU pool of an image that places it in the slot 1 area: held for the kernel before any application region, so
     * that a manifest region, the heap or the stack cannot take it, and no application space maps it. */
    if (app_region_acquire_at(&ledger, APP_REGION_KERNEL_OWNER, OS_NPU_POOL_BASE, OS_NPU_POOL_BYTES,
                              APP_REGION_READ | APP_REGION_WRITE)) return large_failed(5);
    kernel_base = OS_NPU_POOL_BASE; kernel_bytes = OS_NPU_POOL_BYTES;
#endif
    for (unsigned i = 0; manifest && i < manifest->region_count; ++i) {
        const struct ss_region *r = &manifest->regions[i];
        if (r->address < APP_REGION_FLOOR) continue;
        if (app_region_acquire_at(&ledger, owner_asid, r->address, r->bytes,
                                  APP_REGION_READ | (r->access & SS_ACCESS_WRITE ? APP_REGION_WRITE : 0))) return large_failed(6);
    }
    if (large_admit(&ledger, owner_asid, __start_sunstone_large_regions, count, slots, os_large_capacity)) return large_failed(7);
    /* The main stack: a 2 MiB block that the guard owner holds below it, so that an overflow is an EL0 fault in memory
     * no space maps. The stack is at the bottom of its own block. Where no extent is free the static stack stays. */
    if (manifest) {
        uint64_t base, bytes;
        if (app_region_acquire(&ledger, APP_REGION_GUARD_OWNER, 2 * APP_REGION_BLOCK, APP_REGION_READ, &base, &bytes) == 0) {
            (void)app_region_release(&ledger, APP_REGION_GUARD_OWNER, base);
            if (app_region_acquire_at(&ledger, APP_REGION_GUARD_OWNER, base, APP_REGION_BLOCK, APP_REGION_READ) == 0 &&
                app_region_acquire_at(&ledger, owner_asid, base + APP_REGION_BLOCK, manifest->stack_bytes,
                                      APP_REGION_READ | APP_REGION_WRITE) == 0)
                main_stack = (void *)(uintptr_t)(base + APP_REGION_BLOCK);
            else if (ledger.entry)
                for (unsigned i = 0; i < ledger.capacity; ++i)        /* give back what the pair took */
                    if (ledger.entry[i].owner == APP_REGION_GUARD_OWNER) ledger.entry[i].owner = 0;
        }
    }
    arena = (struct app_table_arena){arena_pages, (uintptr_t)arena_pages, APP_SPACE_PAGES, 0};
    int built = app_space_build_l2(&space, &arena, base_l1, base_l2, &ledger, owner_asid, owner_asid, 0) == 0;
    /* The heap record, when the manifest has one, becomes the C library's heap; an image whose library cannot
     * take it refuses the application rather than run with a heap of another size. */
    for (unsigned i = 0; built && i < count; ++i)
        if ((slots[i].flags & SS_LARGE_HEAP) && !(runtime_heap_attach && runtime_heap_attach((uintptr_t)slots[i].base, (size_t)slots[i].bytes)))
            built = 0;
    if (!built) {
        for (unsigned i = 0; i < count; ++i) if (slots[i].bytes) app_region_release(&ledger, owner_asid, slots[i].base);
        memset(slots, 0, sizeof *slots * count);
        return large_failed(8);
    }
    clean_tables(arena_pages, sizeof arena_pages);
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
#define WAITER_GAVE 0x80000000u       /* in os_large_waiter_region: the loader gave this waiter a notification */

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
    /* Every task of the application carries the tag, with or without a space: it tells the runtime that index 0 of
     * the task is the application's, and the task-switch hook acts on it only once a space is installed. */
    if (task) vTaskSetApplicationTaskTag((TaskHandle_t)task, (TaskHookFunction_t)(uintptr_t)&owner_ttbr);
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
    out->copy_us = slots[index].copy_us; out->digest_us = slots[index].digest_us;
    return 0;
}

void os_large_finish(unsigned index, unsigned state, uint32_t error)
{
    if (index >= slot_count) return;
    slots[index].error = error;
    __atomic_store_n(&slots[index].state, state, __ATOMIC_RELEASE);
    taskENTER_CRITICAL();           /* a waiter leaves its entry in the same critical section */
    for (unsigned i = 0; i < os_large_waiter_capacity; ++i)
        if (os_large_waiter_task[i] && (os_large_waiter_region[i] & ~WAITER_GAVE) == index) {
            os_large_waiter_region[i] |= WAITER_GAVE;
            xTaskNotifyGiveIndexed(os_large_waiter_task[i], SS_NOTIFY_DEADLINE);
        }
    taskEXIT_CRITICAL();
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
        (void)ulTaskNotifyTakeIndexed(SS_NOTIFY_DEADLINE, pdTRUE, ticks - used);
    }
    taskENTER_CRITICAL();
    os_large_waiter_task[entry] = 0;
    /* A notification the loader gave after the wait returned would end the application's next wait on index 0. */
    if (os_large_waiter_region[entry] & WAITER_GAVE) (void)ulTaskNotifyTakeIndexed(SS_NOTIFY_DEADLINE, pdTRUE, 0);
    taskEXIT_CRITICAL();
    return result;
}
