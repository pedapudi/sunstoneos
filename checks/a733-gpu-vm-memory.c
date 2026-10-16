#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "a733_gpu_vm_memory.h"
#include <string.h>

#define TABLE_CPU UINT64_C(0x44000000)
#define CONTEXT_CPU UINT64_C(0x46000040)
static struct a733_gpu_vm_memory memory, refused, empty;
static uint64_t pages[3] = {TABLE_CPU, TABLE_CPU + 4096, TABLE_CPU + 8192};
static struct pvr_gpu_vm_tables tables = {(void *)TABLE_CPU, 3 * 4096, pages, 3};
static struct pvr_gpu_mapping maps[2] = {
    {0x40000000, 0x45000000, 4096, 12, 0},
    {0x40001000, 0x45001000, 4096, 12, PVR_GPU_VM_READ_ONLY}
};
static unsigned char snapshot[3 * 4096];
static StaticTask_t task;
static StackType_t stack[4096];
static uint64_t *level2;
static void worker(void *unused);
BaseType_t pvr_gpu_service_task_privileged(TaskFunction_t entry, void *argument)
{ return entry == worker && !argument ? pdTRUE : pdFALSE; }
void vApplicationStackOverflowHook(TaskHandle_t handle, char *name)
{ (void)handle; platform_panic(name, 0); }
static uint64_t word(const void *p, unsigned bytes)
{
    const volatile unsigned char *b = p;
    uint64_t v = 0;
    for (unsigned i = 0; i < bytes; ++i) v |= (uint64_t)b[i] << (8 * i);
    return v;
}
static void table_read_only(int yes)
{
    unsigned i = (TABLE_CPU - 0x40000000) / 0x200000;
    uint64_t saved = level2[i];
    level2[i] = 0;
    __asm__ volatile("dsb ishst\n tlbi vmalle1is\n dsb ish\n isb" ::: "memory");
    level2[i] = yes ? saved | (1u << 7) : saved & ~(UINT64_C(1) << 7);
    __asm__ volatile("dsb ishst\n tlbi vmalle1is\n dsb ish\n isb" ::: "memory");
}
static void guards(void)
{
    const volatile unsigned char *table = (void *)TABLE_CPU;
    const volatile unsigned char *ctx = (void *)(CONTEXT_CPU & ~UINT64_C(4095));
    for (unsigned i = 3 * 4096; i < 4 * 4096; ++i) configASSERT(table[i] == 0xa5);
    for (unsigned i = 0; i < 4096; ++i)
        if (i < 64 || i >= 64 + PVR_GPU_VM_CONTEXT_BYTES) configASSERT(ctx[i] == 0xa5);
}
static void complete(enum a733_gpu_vm_memory_phase phase)
{
    for (unsigned i = 0; i < 3; ++i) {
        configASSERT(memory.cursor == i && memory.phase == phase);
        configASSERT(a733_gpu_vm_memory_poll(&memory) == A733_GPU_VM_MEMORY_AGAIN);
    }
    configASSERT(memory.cursor == 3 && memory.phase == phase);
    configASSERT(a733_gpu_vm_memory_poll(&memory) == A733_GPU_VM_MEMORY_OK);
}
static void worker(void *unused)
{
    (void)unused;
    uint64_t el, mode;
    __asm__ volatile("mrs %0, CurrentEL\n mrs %1, spsel" : "=r"(el), "=r"(mode));
    configASSERT(el == 4 && !mode);
    memset((void *)TABLE_CPU, 0xa5, 4 * 4096);
    memset((void *)(CONTEXT_CPU & ~UINT64_C(4095)), 0xa5, 4096);

    /* These refusals must precede output/state mutation. */
    configASSERT(!a733_gpu_vm_memory_init(&refused, &tables, (void *)0x45000040, 0x45000040));
    struct pvr_gpu_vm_tables bad = tables;
    bad.cpu = (void *)(TABLE_CPU + 4096);
    configASSERT(!a733_gpu_vm_memory_init(&refused, &bad, (void *)CONTEXT_CPU, CONTEXT_CPU));
    bad.cpu = (void *)0x48000000;
    configASSERT(!a733_gpu_vm_memory_init(&refused, &bad, (void *)CONTEXT_CPU, CONTEXT_CPU));
    table_read_only(1);
    configASSERT(!a733_gpu_vm_memory_init(&refused, &tables, (void *)CONTEXT_CPU, CONTEXT_CPU));
    table_read_only(0);
    configASSERT(!memcmp(&refused, &empty, sizeof(empty)));
    configASSERT(a733_gpu_vm_memory_init(&memory, &tables, (void *)CONTEXT_CPU, CONTEXT_CPU));
    configASSERT(a733_gpu_vm_memory_publish_begin(&memory) == A733_GPU_VM_MEMORY_INVALID);
    memcpy(snapshot, (void *)TABLE_CPU, sizeof(snapshot));
    uint64_t pa = maps[0].physical;
    maps[0].physical = CONTEXT_CPU & ~UINT64_C(4095);
    configASSERT(a733_gpu_vm_memory_build(&memory, maps, 2) == A733_GPU_VM_MEMORY_INVALID);
    maps[0].physical = pa;
    configASSERT(!memcmp(snapshot, (void *)TABLE_CPU, sizeof(snapshot)));
    configASSERT(word((void *)CONTEXT_CPU, 8) == UINT64_C(0xa5a5a5a5a5a5a5a5));
    configASSERT(memory.phase == A733_GPU_VM_MEMORY_OFFLINE);

    configASSERT(a733_gpu_vm_memory_build(&memory, maps, 2) == A733_GPU_VM_MEMORY_OK);
    configASSERT(word((void *)(TABLE_CPU + 4), 4) == (pages[1] >> 8 | 1));
    configASSERT(word((void *)(TABLE_CPU + 4096), 8) == (pages[2] | 1));
    configASSERT(word((void *)(TABLE_CPU + 8192), 8) == (maps[0].physical | 1));
    configASSERT(word((void *)(TABLE_CPU + 8200), 8) == (maps[1].physical | 3));
    configASSERT(word((void *)CONTEXT_CPU, 8) == pages[0]);
    configASSERT(word((void *)(CONTEXT_CPU + 8), 4) == UINT32_MAX);
    configASSERT(a733_gpu_vm_memory_publish_begin(&memory) == A733_GPU_VM_MEMORY_AGAIN);
    complete(A733_GPU_VM_MEMORY_PUBLISHING);
    configASSERT(memory.phase == A733_GPU_VM_MEMORY_PUBLISHED);
    configASSERT(a733_gpu_vm_memory_build(&memory, maps, 2) == A733_GPU_VM_MEMORY_INVALID);
    /* Simulated firmware assignment within the uncached mixed-ownership record. */
    *(volatile uint32_t *)(CONTEXT_CPU + 8) = 7;
    __asm__ volatile("dsb sy" ::: "memory");
    /* No GPU is attached in this fixture; isolation is true by construction. */
    configASSERT(a733_gpu_vm_memory_reclaim_begin(&memory) == A733_GPU_VM_MEMORY_AGAIN);
    complete(A733_GPU_VM_MEMORY_RECLAIMING);
    configASSERT(memory.phase == A733_GPU_VM_MEMORY_OFFLINE);
    configASSERT(word((void *)(CONTEXT_CPU + 8), 4) == 7);
    guards();

    configASSERT(a733_gpu_vm_memory_build(&memory, maps, 2) == A733_GPU_VM_MEMORY_OK);
    configASSERT(a733_gpu_vm_memory_publish_begin(&memory) == A733_GPU_VM_MEMORY_AGAIN);
    configASSERT(a733_gpu_vm_memory_poll(&memory) == A733_GPU_VM_MEMORY_AGAIN);
    configASSERT(memory.cursor == 1);
    table_read_only(1);
    configASSERT(a733_gpu_vm_memory_poll(&memory) == A733_GPU_VM_MEMORY_FAULT);
    configASSERT(memory.phase == A733_GPU_VM_MEMORY_FAILED && memory.cursor == 1);
    table_read_only(0);
    /* Unmapped backing would fault if any terminal call touched it. */
    void *saved = memory.tables.cpu;
    memory.tables.cpu = (void *)0x48000000;
    configASSERT(a733_gpu_vm_memory_poll(&memory) == A733_GPU_VM_MEMORY_FAULT);
    configASSERT(a733_gpu_vm_memory_build(&memory, maps, 2) == A733_GPU_VM_MEMORY_FAULT);
    configASSERT(a733_gpu_vm_memory_publish_begin(&memory) == A733_GPU_VM_MEMORY_FAULT);
    configASSERT(a733_gpu_vm_memory_reclaim_begin(&memory) == A733_GPU_VM_MEMORY_FAULT);
    memory.tables.cpu = saved;
    configASSERT(memory.cursor == 1);
    guards();
    platform_puts("PASS GPU workload memory: native mappings, bounded publication/reclaim, retained mapping fault\n");
    platform_exit(0);
}
void kernel_main(void)
{
    platform_init();
    uint64_t ttbr;
    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(ttbr));
    uint64_t *root = (void *)(uintptr_t)(ttbr & UINT64_C(0x000ffffffffff000));
    level2 = (void *)(uintptr_t)(root[1] & UINT64_C(0x000ffffffffff000));
    configASSERT(xTaskCreateStaticAffinitySet(worker, "gpu-vm", 4096, NULL, 4, stack, &task, 1));
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
