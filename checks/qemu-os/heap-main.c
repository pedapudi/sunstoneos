/* Test application of checks/qemu-os-boot.py for a heap that comes from the application memory ledger
 * (docs/designs/application-memory-layout.md, step 1). The manifest asks for a heap above the linked limit, so
 * the heap is the last large region and the C library's sbrk serves it. The check fills the memory behind the
 * region with a pattern before the boot, so a heap that is not zeroed shows.
 *
 * Its input region holds a mode (u64):
 *   mode 0: take memory from sbrk, check that it is zero, write it, shrink and grow it again, run out of it, and
 *           finish with the result {tag, base, bytes, peak, fold}.
 *   mode 1: start a task that runs in no application space and reads the heap: it must fault.
 *   mode 2: read the byte after the region from the application's own space: it must fault.
 *   mode 3: let a worker task of the application, attached to its space, write the heap that the main task reads. */
#include "heap_test-app.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stddef.h>
#include <sys/reent.h>
#include <sys/types.h>

extern void *_sbrk(ptrdiff_t change);
extern size_t runtime_heap_peak(void);
/* The C library is not linked into the virtual board; sbrk reaches its reentrancy structure through this pointer. */
static struct _reent test_reent;
struct _reent *_impure_ptr = &test_reent;

#define TAG_OK UINT64_C(0x48454150204f4b)       /* "HEAP OK" */
#define TAG_BAD UINT64_C(0x48454150204241)      /* "HEAP BA" */
#define MIB (1u << 20)

static StaticTask_t intruder_state, worker_state;
static StackType_t intruder_stack[512], worker_stack[512];
static volatile uint8_t *heap_base;
static size_t heap_bytes;
static TaskHandle_t main_handle;

static void intruder(void *unused)
{
    (void)unused;
    (void)*heap_base;                             /* no application space: a permission fault at EL0 */
    ss_fail(0xe02);
}

static void worker(void *unused)
{
    (void)unused;
    volatile uint64_t *words = (volatile uint64_t *)(heap_base + 2 * MIB);
    for (unsigned i = 0; i < 4096; ++i) words[i] = UINT64_C(0x9e3779b97f4a7c15) * (i + 1);
    xTaskNotifyGive(main_handle);
    for (;;) vTaskDelay(1000);
}

static void finish(uint64_t tag, uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    const uint64_t result[5] = {tag, a, b, c, d};
    ss_finish(result, sizeof(result));
}

void heap_test_main(const struct ss_grant *grant)
{
    const uint64_t mode = ((const uint64_t *)grant->regions[HEAP_TEST_REGION_INPUT].base)[0];
    size_t capacity;
    uint8_t *base = ss_large_base(0, &capacity);             /* the heap is the only large record */
    if (!base || ss_large_wait(0, 1000000) != 1) ss_fail(0xe01);
    heap_base = base;
    heap_bytes = capacity;
    if (mode == 1) {
        xTaskCreateStatic(intruder, "intruder", 512, NULL, 3, intruder_stack, &intruder_state);
        for (;;) vTaskDelay(10);
    }
    if (mode == 2) {
        (void)*(volatile uint8_t *)(base + capacity);                               /* the byte after the region */
        ss_fail(0xe03);
    }
    if (mode == 3) {
        main_handle = xTaskGetCurrentTaskHandle();
        TaskHandle_t w = xTaskCreateStaticAffinitySet(worker, "worker", 512, NULL, 3, worker_stack, &worker_state,
                                                      (UBaseType_t)(grant->worker_cores & -grant->worker_cores));
        ss_large_attach(w);
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
        volatile const uint64_t *words = (volatile const uint64_t *)(base + 2 * MIB);
        uint64_t fold = 0;
        for (unsigned i = 0; i < 4096; ++i) fold ^= words[i];
        finish(TAG_OK, (uintptr_t)base, capacity, 0, fold);
    }

    /* The allocator's view: the first sbrk(0) is the region's base. */
    if ((uint8_t *)_sbrk(0) != base) finish(TAG_BAD, 1, (uintptr_t)_sbrk(0), 0, 0);
    volatile uint64_t *a = _sbrk(MIB);
    if ((uint8_t *)a != base) finish(TAG_BAD, 2, (uintptr_t)a, 0, 0);
    uint64_t nonzero = 0;
    for (size_t i = 0; i < MIB / 8; ++i) nonzero |= a[i];
    if (nonzero) finish(TAG_BAD, 3, nonzero, 0, 0);          /* stale memory reached the application */
    for (size_t i = 0; i < MIB / 8; ++i) a[i] = ~(uint64_t)i;
    /* Shrink by half and grow again: what comes back is zero, not the old contents. */
    if ((uint8_t *)_sbrk(-(ptrdiff_t)(MIB / 2)) != base + MIB) finish(TAG_BAD, 4, 0, 0, 0);
    volatile uint64_t *b = _sbrk(MIB / 2);
    if ((uint8_t *)b != base + MIB / 2) finish(TAG_BAD, 5, (uintptr_t)b, 0, 0);
    nonzero = 0;
    for (size_t i = 0; i < MIB / 16; ++i) nonzero |= b[i];
    if (nonzero) finish(TAG_BAD, 6, nonzero, 0, 0);
    /* Take the rest in one step, then run out. */
    uint64_t fold = 0;
    size_t rest = capacity - MIB;
    volatile uint64_t *c = _sbrk((ptrdiff_t)rest);
    if ((uint8_t *)c != base + MIB) finish(TAG_BAD, 7, (uintptr_t)c, 0, 0);
    for (size_t i = 0; i < rest / 8; i += 4096) fold |= c[i];
    if (fold) finish(TAG_BAD, 8, fold, 0, 0);
    test_reent._errno = 0;
    if (_sbrk(16) != (void *)-1) finish(TAG_BAD, 9, 0, 0, 0);
    finish(TAG_OK, (uintptr_t)base, capacity, runtime_heap_peak(), (uint64_t)test_reent._errno);
}
