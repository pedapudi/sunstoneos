#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "platform.h"
#include "a733_dma_native.h"

static _Alignas(4096) uint8_t pool[3 * 4096];
static struct a733_dma_native adapter;
static StaticTask_t tasks[2];
static StackType_t stacks[2][2048];
static StaticSemaphore_t mutex_state;
static SemaphoreHandle_t mutex;
static unsigned completed;
static const struct a733_dma_operations *ops = &a733_dma_native_operations;

static void fatal(void *unused)
{ (void)unused; platform_panic("DMA adapter contract", 0); }

static void worker(void *argument)
{
    unsigned id = (unsigned)(uintptr_t)argument;
    uint8_t *page = pool + id * 4096;
    struct a733_dma_mapping m = {page + 64, (uintptr_t)page + 64, 128, A733_DMA_WRITE};
    for (unsigned round = 0; round < 8; ++round) {
        configASSERT(xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE);
        configASSERT(ops->validate_mapping(&adapter, &m));
        uint64_t flags_before, flags_after, par_before, par_after;
        __asm__ volatile("mrs %0, daif\n mrs %1, par_el1" : "=r"(flags_before), "=r"(par_before));
        struct dma_translation t;
        configASSERT(a733_dma_native_translate(NULL, (uintptr_t)m.cpu, 1, &t));
        __asm__ volatile("mrs %0, daif\n mrs %1, par_el1" : "=r"(flags_after), "=r"(par_after));
        configASSERT(flags_before == flags_after && par_before == par_after);
        configASSERT(t.physical == m.bus && t.attributes == 0xff && t.shareability == 3);
        for (unsigned i = 0; i < 4096; ++i) page[i] = 0x5a;
        for (unsigned i = 0; i < m.bytes; ++i) ((uint8_t *)m.cpu)[i] = (uint8_t)(id * 31 + round + i);
        ops->cache(&adapter, A733_DMA_CLEAN, m.cpu, m.bytes);
        ops->cache(&adapter, A733_DMA_CLEAN_INVALIDATE, m.cpu, m.bytes);
        ops->cache(&adapter, A733_DMA_INVALIDATE, m.cpu, m.bytes);
        ops->barrier(&adapter);
        for (unsigned i = 0; i < 4096; ++i) {
            unsigned value = i >= 64 && i < 192 ? (uint8_t)(id * 31 + round + i - 64) : 0x5a;
            configASSERT(page[i] == value);
        }
        configASSERT(xSemaphoreGive(mutex) == pdTRUE);
        vTaskDelay(1);
    }
    configASSERT(xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE);
    if (++completed == 2) {
        platform_puts("PASS DMA native: EL1 translations, cache instructions, register access, and serialized FreeRTOS callers\n");
        platform_exit(0);
    }
    configASSERT(xSemaphoreGive(mutex) == pdTRUE);
    vTaskSuspend(NULL);
    configASSERT(0);
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{ (void)task; platform_panic(name, 0); }

void kernel_main(void)
{
    platform_init();
    /* Make a spare QEMU RAM block read-only to exercise real AT permission faults.
     * This block contains neither executable code nor the test's allocations. */
    uint64_t ttbr;
    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(ttbr));
    uint64_t *root = (void *)(uintptr_t)(ttbr & UINT64_C(0x000ffffffffff000));
    uint64_t *level2 = (void *)(uintptr_t)(root[1] & UINT64_C(0x000ffffffffff000));
    level2[(0x47000000 - 0x40000000) / 0x200000] |= 1u << 7;
    __asm__ volatile("dsb sy\n tlbi vmalle1\n dsb sy\n isb" ::: "memory");
    struct dma_memory_window windows[] = {
        {(uintptr_t)pool, (uintptr_t)pool, sizeof(pool), 7},
        {0x47000000, 0x47000000, 4096, 7}
    };
    /* PL011 provides actual emulated Device memory for adapter MMIO checks.
     * No A733 DMA controller exists in this machine and none is accessed. */
    configASSERT(a733_dma_native_init(&adapter, 0x09000000, 0x09000000,
                                      windows, 2, NULL, 0, fatal, NULL));
    struct a733_dma_mapping m = {(void *)0x47000000, 0x47000000, 4096, A733_DMA_READ};
    configASSERT(ops->validate_mapping(&adapter, &m));
    m.access = A733_DMA_WRITE;
    configASSERT(!ops->validate_mapping(&adapter, &m));
    m.access = A733_DMA_DESCRIPTORS;
    configASSERT(!ops->validate_mapping(&adapter, &m));
    m = (struct a733_dma_mapping){pool, (uintptr_t)pool + 64, 64, A733_DMA_READ};
    configASSERT(!ops->validate_mapping(&adapter, &m));
    struct dma_translation t;
    configASSERT(!a733_dma_native_translate(NULL, 0x48000000, 0, &t));
    configASSERT(a733_dma_native_translate(NULL, 0x09000018, 0, &t));
    configASSERT(t.physical == 0x09000018 && t.attributes == 0);
    configASSERT(!(ops->read(&adapter, 0x09000018) & (1u << 5)));
    ops->write(&adapter, 0x09000000, '.');
    uint64_t before = ops->ticks(&adapter);
    configASSERT(ops->ticks(&adapter) >= before);
    uint64_t marker = 0x12345000;
    __asm__ volatile("msr par_el1, %0" :: "r"(marker) : "memory");
    mutex = xSemaphoreCreateMutexStatic(&mutex_state);
    configASSERT(mutex);
    for (unsigned i = 0; i < 2; ++i)
        configASSERT(xTaskCreateStatic(worker, "dma", 2048, (void *)(uintptr_t)i,
                                      3, stacks[i], &tasks[i]));
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
