/* heap-test: the C library's heap comes from upper memory (docs/designs/application-memory-layout.md, step 1).
 * The manifest asks for 1 GiB less 1 MiB of heap; the OS adds its 1 MiB share, so the heap is the ledger's first
 * 1 GiB region, at 0x80000000. No card file is involved.
 *
 * HEAP_TEST_MODE selects what the image does (the CMake cache variable SUNSTONE_HEAP_TEST_MODE):
 *   0  take 512 MiB with malloc, check that it lies in the region and reads as zero, write a pattern into every
 *      word, read it back, run the allocator out of memory, free and allocate again, and report the result.
 *   1  start a task that runs in no application space and reads the first byte of the heap: the OS ends the
 *      session with a permission fault at 0x80000000.
 *   2  read the first byte after the region from the application's own space: a permission fault at 0xc0000000.
 *   3  let a worker task attached to the application's space write the heap, and the main task read it back.
 * The result is deterministic: it holds no time or address that varies between boots. */
#include "heap_test-app.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdlib.h>

#ifndef HEAP_TEST_MODE
#define HEAP_TEST_MODE 0
#endif
#define MIB (UINT64_C(1) << 20)
#define TAKE (512 * MIB)
#define TAG_OK UINT64_C(0x48454150544f4b)        /* "HEAPTOK" */
#define TAG_BAD UINT64_C(0x48454150544241)       /* "HEAPTBA" */
#define PATTERN UINT64_C(0x9e3779b97f4a7c15)

struct heap_test_result {
    uint64_t tag, mode, base, bytes;
    uint64_t in_range;            /* 1 when the malloc'd block lies inside the region */
    uint64_t nonzero_words;       /* words that were not zero when malloc returned the block */
    uint64_t fold_written, fold_read;
    uint64_t out_of_memory;       /* 1 when a request beyond the remaining heap returned NULL */
    uint64_t reused;              /* 1 when the block freed and allocated again was usable and in range */
    uint64_t reserved[128 / 8 - 10];
};
_Static_assert(sizeof(struct heap_test_result) == 128, "result is the manifest's output_limit");

static StaticTask_t intruder_state, worker_state;
static StackType_t intruder_stack[512], worker_stack[512];
static volatile uint8_t *region_base;
static TaskHandle_t main_handle;

static void intruder(void *unused)
{
    (void)unused;
    (void)*region_base;                          /* no application space: a permission fault at EL0 */
    ss_fail(0x1e02);
}

static void worker(void *unused)
{
    (void)unused;
    volatile uint64_t *words = (volatile uint64_t *)(region_base + 2 * MIB);
    for (unsigned i = 0; i < 4096; ++i) words[i] = PATTERN * (i + 1);
    xTaskNotifyGive(main_handle);
    for (;;) vTaskDelay(1000);
}

void heap_test_main(const struct ss_grant *grant)
{
    static struct heap_test_result r;
    r.mode = HEAP_TEST_MODE;
    size_t bytes;
    uint8_t *base = ss_large_base(0, &bytes);       /* the heap is the application's only large record */
    if (!base || ss_large_wait(0, 1000000) != 1) ss_fail(0x1e01);
    r.base = (uintptr_t)base;
    r.bytes = bytes;
    region_base = base;
#if HEAP_TEST_MODE == 1
    xTaskCreateStatic(intruder, "intruder", 512, NULL, 3, intruder_stack, &intruder_state);
    for (;;) vTaskDelay(10);
#elif HEAP_TEST_MODE == 2
    (void)*(volatile uint8_t *)(base + bytes);
    ss_fail(0x1e03);
#elif HEAP_TEST_MODE == 3
    main_handle = xTaskGetCurrentTaskHandle();
    TaskHandle_t w = xTaskCreateStaticAffinitySet(worker, "worker", 512, NULL, 3, worker_stack, &worker_state,
                                                  (UBaseType_t)(grant->worker_cores & -grant->worker_cores));
    ss_large_attach(w);
    (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
    volatile const uint64_t *check = (volatile const uint64_t *)(base + 2 * MIB);
    for (unsigned i = 0; i < 4096; ++i) r.fold_read ^= check[i];
    for (unsigned i = 0; i < 4096; ++i) r.fold_written ^= PATTERN * (i + 1);
    r.tag = r.fold_read == r.fold_written ? TAG_OK : TAG_BAD;
    ss_finish(&r, sizeof r);
#else
    (void)grant;
    uint64_t *block = malloc(TAKE);
    if (!block) { r.tag = TAG_BAD; ss_finish(&r, sizeof r); }
    r.in_range = (uint8_t *)block >= base && (uint8_t *)block + TAKE <= base + bytes;
    const size_t words = TAKE / 8;
    for (size_t i = 0; i < words; ++i) {
        r.nonzero_words += block[i] != 0;
        if (!(i & 0x3ffff)) ss_progress();
    }
    for (size_t i = 0; i < words; ++i) {
        uint64_t value = PATTERN * (i + 1);
        block[i] = value;
        r.fold_written ^= value;
        if (!(i & 0x3ffff)) ss_progress();
    }
    for (size_t i = 0; i < words; ++i) {
        r.fold_read ^= block[i];
        if (!(i & 0x3ffff)) ss_progress();
    }
    void *too_much = malloc(bytes);                 /* more than the region has left */
    r.out_of_memory = too_much == NULL;
    free(too_much);
    free(block);
    uint64_t *again = malloc(TAKE);
    r.reused = again && (uint8_t *)again >= base && (uint8_t *)again + TAKE <= base + bytes;
    if (again) { again[0] = 1; again[words - 1] = 2; r.reused = r.reused && again[0] == 1 && again[words - 1] == 2; }
    r.tag = r.in_range && !r.nonzero_words && r.fold_read == r.fold_written && r.out_of_memory && r.reused ? TAG_OK : TAG_BAD;
    ss_finish(&r, sizeof r);
#endif
}
