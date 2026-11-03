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
#include "app_large.h"
#include "umem.h"
#include "aarch64_device_memory.h"
#include "device-ranges.h"
#ifdef SUNSTONE_A733_OS_NPU
#include "npu.h"
#endif
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
/* The ledger of the unified memory allocator (docs/designs/unified-memory.md). Its first entries come from this array;
 * it carves the pages for the rest from the memory it manages. The chunk policy bytes cover the DRAM the ledger can
 * hold, 2 MiB each, from the first byte of usable memory to the end of the installed memory. */
#define UMEM_FIRST_CELLS 64u
static uint8_t umem_cells[UMEM_FIRST_CELLS * 128] __attribute__((aligned(16)));
static struct umem ledger;
static uint8_t chunk_policy[(APP_REGION_LIMIT - APP_REGION_FLOOR) / UMEM_CHUNK];
/* The application's regions as the space builder reads them: a snapshot of the owner's extents from the ledger. */
static struct app_region_ledger view;
/* One space: the root and a level-2 table per upper slot, the most it can need (app_region.h). */
static uint64_t arena_pages[APP_SPACE_PAGES][512] __attribute__((aligned(4096)));
static struct app_table_arena arena;
static struct large_slot *const slots = os_large_slot_storage;
static struct app_space space;
static unsigned slot_count;
static int admitted, ledger_ready;
static void *main_stack;
/* An EL0 translation of address in the application's own tables, for a task that is not in them (the NPU service's tasks run
 * in the kernel's table, where upper memory is EL1-only). Without an application space it is the current regime's. */
int os_large_translate_el0(uintptr_t address, int write, struct aarch64_translation *out)
{
    if (!admitted) return aarch64_device_translate_el0(address, write, out);
    uint64_t physical;
    uint64_t d = app_space_el0_descriptor(&space, address, write, &physical);
    if (!d || !out) return 0;
    const unsigned index = (unsigned)(d >> 2) & 7;
    *out = (struct aarch64_translation){physical, index == 1 ? 0xffu : index == 2 ? 0x44u : 0u, (unsigned)(d >> 8) & 3};
    return 1;
}
void *os_large_main_stack(void) { return main_stack; }

/* The ledger's allocation interface for large_admit_with, and the one-call forms the admission below uses. Every extent
 * is a whole number of 2 MiB blocks on a block boundary, as the space builder maps blocks. Admission runs before the
 * scheduler on one core, so the ledger takes no lock; the memory is raw because the loader fills a region, the heap
 * allocator zeroes what it hands out, and a region with a source is overwritten up to the source's length. */
static void ledger_lock(void *ctx);
static void ledger_unlock(void *ctx);
static const struct umem_hooks umem_boot_hooks = {.lock = ledger_lock, .unlock = ledger_unlock};
static int region_take(void *ctx, uint16_t owner, uint64_t bytes, unsigned access, uint64_t *base, uint64_t *rounded)
{
    (void)ctx;
    struct umem_buf b;
    struct umem_request r = {.bytes = (bytes + APP_REGION_BLOCK - 1) & ~(APP_REGION_BLOCK - 1), .align = APP_REGION_BLOCK,
                             .owner = owner, .flags = UMEM_RAW, .access = (uint8_t)access};
    if (!bytes || bytes > APP_REGION_LIMIT) return APP_REGION_INVALID;
    int s = umem_alloc(&ledger, &r, &b);
    if (s) return s == UMEM_NO_SPACE ? APP_REGION_NO_SPACE : APP_REGION_INVALID;
    *base = b.base; *rounded = b.bytes;
    return APP_REGION_OK;
}
static int region_take_at(uint16_t owner, uint64_t base, uint64_t bytes, unsigned access)
{
    struct umem_buf b;
    struct umem_request r = {.bytes = (bytes + APP_REGION_BLOCK - 1) & ~(APP_REGION_BLOCK - 1), .align = APP_REGION_BLOCK,
                             .base = base, .owner = owner, .flags = UMEM_RAW | UMEM_AT, .access = (uint8_t)access};
    if (!bytes || bytes > APP_REGION_LIMIT || (base & (APP_REGION_BLOCK - 1))) return APP_REGION_INVALID;
    int s = umem_alloc(&ledger, &r, &b);
    return s == 0 ? APP_REGION_OK : s == UMEM_NO_SPACE ? APP_REGION_NO_SPACE : APP_REGION_INVALID;
}
static int region_give(void *ctx, uint16_t owner, uint64_t base)
{
    (void)ctx;
    struct umem_buf b;
    struct umem_extent e;
    if (umem_lookup(&ledger, base, &b) || umem_describe(&ledger, &b, &e, 0) || e.owner != owner) return APP_REGION_NOT_OWNED;
    return umem_free(&ledger, &b) ? APP_REGION_NOT_OWNED : APP_REGION_OK;
}
static const struct large_allocator region_allocator = {0, region_take, region_give};
/* The owner's extents, in the form the space builder reads. The ledger's own storage is not a table of regions. */
struct export_state { unsigned count; };
static int export_one(void *ctx, const struct umem_extent *e)
{
    struct export_state *x = ctx;
    if (x->count >= os_ledger_capacity) return 1;
    os_large_entry_storage[x->count++] = (struct app_region_entry){e->base, e->bytes, e->owner, e->access};
    return 0;
}
static int export_regions(uint16_t owner)
{
    struct export_state x = {0};
    view = (struct app_region_ledger){.entry = os_large_entry_storage, .capacity = os_ledger_capacity};
    memset(os_large_entry_storage, 0, sizeof *os_large_entry_storage * os_ledger_capacity);
    return umem_for_each(&ledger, owner, export_one, &x) == x.count;
}

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
    struct umem_range umem_ranges[APP_REGION_SLOTS];
    for (unsigned i = 0; i < ranges; ++i) umem_ranges[i] = (struct umem_range){usable[i].base, usable[i].bytes};
    if (!owner_asid || !ranges || count > os_large_capacity ||
        umem_init(&ledger, &umem_boot_hooks, umem_cells, sizeof umem_cells / umem_cell_bytes(), chunk_policy, APP_REGION_FLOOR,
                  sizeof chunk_policy, umem_ranges, ranges)) return large_failed(2 + (!owner_asid ? 0 : !ranges ? 1 : 2));
    ledger_ready = 1;
    for (unsigned i = 0; manifest && i < manifest->region_count; ++i) {
        const struct ss_region *r = &manifest->regions[i];
        if (r->address < APP_REGION_FLOOR) continue;
        if (region_take_at(owner_asid, r->address, r->bytes, APP_REGION_READ | (r->access & SS_ACCESS_WRITE ? APP_REGION_WRITE : 0))) return large_failed(6);
    }
    if (large_admit_with(&region_allocator, owner_asid, __start_sunstone_large_regions, count, slots, os_large_capacity)) return large_failed(7);
#ifdef SUNSTONE_A733_OS_NPU
    /* The NPU's reservation comes from what the regions left: the largest extent it can address, trimmed to what the runtime
     * places. It belongs to the application and is mapped in its space. A refusal is recorded and leaves the NPU unprepared
     * (os_npu_start refuses); it does not stop the application. */
    if (!os_npu_pool_prepare(&ledger, owner_asid)) return large_failed(9);
#endif
    /* The main stack: a 2 MiB block that the guard owner holds below it, so that an overflow is an EL0 fault in memory
     * no space maps. The stack is at the bottom of its own block. Where no extent is free the static stack stays. */
    if (manifest) {
        uint64_t base, bytes;
        if (region_take(0, APP_REGION_GUARD_OWNER, 2 * APP_REGION_BLOCK, APP_REGION_READ, &base, &bytes) == 0) {
            (void)region_give(0, APP_REGION_GUARD_OWNER, base);
            if (region_take_at(APP_REGION_GUARD_OWNER, base, APP_REGION_BLOCK, APP_REGION_READ) == 0 &&
                region_take_at(owner_asid, base + APP_REGION_BLOCK, manifest->stack_bytes, APP_REGION_READ | APP_REGION_WRITE) == 0)
                main_stack = (void *)(uintptr_t)(base + APP_REGION_BLOCK);
            else
                (void)region_give(0, APP_REGION_GUARD_OWNER, base);     /* give back what the pair took */
        }
    }
    arena = (struct app_table_arena){arena_pages, (uintptr_t)arena_pages, APP_SPACE_PAGES, 0};
    int built = export_regions(owner_asid) && app_space_build_l2(&space, &arena, base_l1, base_l2, &view, owner_asid, owner_asid, 0) == 0;
    /* The heap record, when the manifest has one, becomes the C library's heap; an image whose library cannot
     * take it refuses the application rather than run with a heap of another size. */
    for (unsigned i = 0; built && i < count; ++i)
        if ((slots[i].flags & SS_LARGE_HEAP) && !(runtime_heap_attach && runtime_heap_attach((uintptr_t)slots[i].base, (size_t)slots[i].bytes)))
            built = 0;
    if (!built) {
        for (unsigned i = 0; i < count; ++i) if (slots[i].bytes) region_give(0, owner_asid, slots[i].base);
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

/* The ledger's lock: the kernel's critical section, which also works before the scheduler starts. Run-time allocations (the NPU
 * service's tables, a published device range) take it; admission takes it too, uncontended. */
static void ledger_lock(void *ctx) { (void)ctx; taskENTER_CRITICAL(); }
static void ledger_unlock(void *ctx) { (void)ctx; taskEXIT_CRITICAL(); }
struct umem *os_large_umem(void) { return admitted || ledger_ready ? &ledger : 0; }

/* ---- The device ranges the EL3 terminal cleans (apps/os/device-ranges.h) ---- */
uint64_t os_device_ranges_head;
static struct os_device_range_page *range_tail;
int os_device_range_publish(uint64_t base, uint64_t bytes)
{
    if (!ledger_ready || !bytes || ((base | bytes) & 63) || base < UINT64_C(0x42200000) || base + bytes < base ||
        base + bytes > APP_REGION_LIMIT) return 0;
    taskENTER_CRITICAL();
    int ok = 1;
    if (!range_tail || range_tail->count == OS_DEVICE_RANGES_PER_PAGE) {
        struct umem_buf b;
        struct umem_request r = {.bytes = sizeof(struct os_device_range_page), .align = 4096, .owner = UMEM_OWNER_KERNEL, .flags = UMEM_RAW};
        if (umem_alloc(&ledger, &r, &b) == 0) {
            struct os_device_range_page *page = (struct os_device_range_page *)(uintptr_t)b.base;
            memset(page, 0, sizeof *page);
            if (range_tail) range_tail->next = b.base; else os_device_ranges_head = b.base;
            range_tail = page;
        } else ok = 0;
    }
    if (ok) {
        range_tail->range[range_tail->count] = (struct os_device_range){base, bytes};
        __atomic_store_n(&range_tail->count, range_tail->count + 1, __ATOMIC_RELEASE);
        __asm__ volatile("dsb sy" ::: "memory");
    }
    taskEXIT_CRITICAL();
    return ok;
}

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
