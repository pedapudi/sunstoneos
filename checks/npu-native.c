#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "platform.h"
#include "a733_npu_native.h"
#include "aarch64_device_memory.h"
#include <string.h>

static _Alignas(4096) uint8_t arena[4096 + 16384];
static _Alignas(4096) uint8_t boot_memory[524288], job_memory[524288];
static _Alignas(4096) uint8_t data[4 * 4096];
static struct a733_npu_native adapter, invalid;
static struct a733_npu_command commands[2];
static StaticTask_t tasks[2];
static StackType_t stacks[2][2048];
static StaticSemaphore_t mutex_state;
static SemaphoreHandle_t mutex;
static unsigned completed, expected_fatal;
static const struct a733_npu_ops *ops = &a733_npu_native_operations;

static void fatal(void *cookie)
{
    configASSERT(cookie == &adapter && expected_fatal && completed == 2);
    configASSERT(adapter.failed && adapter.memory.state == A733_NPU_MEMORY_FAULT);
    platform_puts("PASS NPU native: EL1 mappings/cache, Device MMIO, deadlines, retained failures, and serialized FreeRTOS callers\n");
    platform_exit(0);
}
static void translations(void)
{
    uint64_t before_flags, after_flags, before_par, after_par;
    __asm__ volatile("mrs %0, daif\n mrs %1, par_el1" : "=r"(before_flags), "=r"(before_par));
    struct aarch64_translation t;
    configASSERT(aarch64_device_translate((uintptr_t)data + 73, 1, &t));
    configASSERT(t.physical == (uintptr_t)data + 73 && t.attributes == 0xff && t.shareability == 3);
    configASSERT(aarch64_device_translate(0x47000001, 0, &t));
    configASSERT(!aarch64_device_translate(0x47000001, 1, &t));
    configASSERT(!aarch64_device_translate(0x48000000, 0, &t));
    __asm__ volatile("mrs %0, daif\n mrs %1, par_el1" : "=r"(after_flags), "=r"(after_par));
    configASSERT(before_flags == after_flags && before_par == after_par);
}
static void worker(void *argument)
{
    unsigned id = (unsigned)(uintptr_t)argument;
    for (unsigned round = 0; round < 4; ++round) {
        configASSERT(xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE);
        translations();
        memset(data + 4096, (int)(id * 31 + round), 4096);
        uint64_t before = ops->microseconds(&adapter);
        configASSERT(ops->publish(&adapter, &adapter.memory.image, &commands[id], id == 0));
        configASSERT(adapter.memory.state == A733_NPU_MEMORY_DEVICE);
        ops->delay(&adapter, 50);
        configASSERT(ops->microseconds(&adapter) - before >= 50);
        /* QEMU has no NPU. Device quiescence is guaranteed by the absence of a
         * device, and this call exercises the real cache-invalidation path. */
        configASSERT(ops->finish(&adapter, &commands[id], 0));
        configASSERT(adapter.memory.state == A733_NPU_MEMORY_IDLE);
        for (unsigned i = 0; i < sizeof(data); ++i) {
            unsigned expected = i >= 4096 && i < 8192 ? id * 31 + round : 0x69;
            configASSERT(data[i] == expected);
        }
        configASSERT(xSemaphoreGive(mutex) == pdTRUE);
        vTaskDelay(1);
    }
    configASSERT(xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE);
    if (++completed == 2) {
        uint32_t value = 0x12345678;
        configASSERT(!ops->read(&adapter, 4096, &value));
        configASSERT(adapter.failed && value == 0x12345678);
        configASSERT(!ops->read(&adapter, 0x18, &value));
        configASSERT(!ops->write(&adapter, 0, '!'));
        configASSERT(!ops->publish(&adapter, &adapter.memory.image, &commands[0], 1));
        configASSERT(!ops->finish(&adapter, &commands[0], 0));
        expected_fatal = 1;
        ops->microseconds(&adapter); /* Void/value callbacks cannot hide failure. */
        configASSERT(0);
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
    uint64_t ttbr;
    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(ttbr));
    uint64_t *root = (void *)(uintptr_t)(ttbr & UINT64_C(0x000ffffffffff000));
    uint64_t *level2 = (void *)(uintptr_t)(root[1] & UINT64_C(0x000ffffffffff000));
    level2[(0x47000000 - 0x40000000) / 0x200000] |= 1u << 7;
    __asm__ volatile("dsb sy\n tlbi vmalle1\n dsb sy\n isb" ::: "memory");
    struct a733_npu_buffer buffers[] = {
        {(uintptr_t)boot_memory, (uintptr_t)boot_memory, sizeof(boot_memory), 0x10000000, A733_NPU_BUFFER_BOOTSTRAP},
        {(uintptr_t)job_memory, (uintptr_t)job_memory, sizeof(job_memory), 0x10080000, A733_NPU_BUFFER_COMMAND},
        {(uintptr_t)data + 4096, (uintptr_t)data + 4096, 4096, 0x10100000, A733_NPU_BUFFER_WRITE},
        {(uintptr_t)data + 8192, (uintptr_t)data + 8192, 4096, 0x10101000, A733_NPU_BUFFER_READ}
    };
    memset(data, 0x69, sizeof(data));
    memset(arena, 0xa5, sizeof(arena));
    /* RAM and substituted physical addresses cannot become an MMIO window. */
    configASSERT(!a733_npu_native_init(&invalid, (uintptr_t)data, (uintptr_t)data,
        arena, sizeof(arena), (uintptr_t)arena, buffers, 4, fatal, &adapter));
    configASSERT(!a733_npu_native_init(&invalid, 0x46000000, 0x46000000,
        arena, sizeof(arena), (uintptr_t)arena, buffers, 4, fatal, &adapter));
    configASSERT(!a733_npu_native_init(&invalid, 0x09000000, 0x09001000,
        arena, sizeof(arena), (uintptr_t)arena, buffers, 4, fatal, &adapter));
    struct a733_npu_buffer saved = buffers[2];
    buffers[2].cpu = buffers[2].physical = 0x47000000;
    configASSERT(!a733_npu_native_init(&invalid, 0x09000000, 0x09000000,
        arena, sizeof(arena), (uintptr_t)arena, buffers, 4, fatal, &adapter));
    buffers[2] = saved;
    for (unsigned i = 0; i < sizeof(invalid); ++i) configASSERT(!((uint8_t *)&invalid)[i]);
    for (unsigned i = 0; i < sizeof(arena); ++i) configASSERT(arena[i] == 0xa5);
    configASSERT(a733_npu_bootstrap(boot_memory, sizeof(boot_memory)));
    /* PL011 is the Device-memory target for MMIO access only. Never run the
     * NPU controller's register sequence against an unrelated peripheral. */
    configASSERT(a733_npu_native_init(&adapter, 0x09000000, 0x09000000,
        arena, sizeof(arena), (uintptr_t)arena, buffers, 4, fatal, &adapter));
    configASSERT(a733_npu_memory_command(&adapter.memory, 0x10000000, &commands[0]));
    configASSERT(a733_npu_memory_command(&adapter.memory, 0x10080000, &commands[1]));
    uint32_t value;
    configASSERT(ops->read(&adapter, 0x18, &value) && !(value & (1u << 5)));
    configASSERT(ops->write(&adapter, 0, '.'));
    configASSERT(!aarch64_device_cache(AARCH64_CACHE_CLEAN, (uintptr_t)data + 1, 64));
    configASSERT(!aarch64_device_cache(AARCH64_CACHE_INVALIDATE, (uintptr_t)data, 65));
    configASSERT(!aarch64_device_cache((enum aarch64_cache_op)-1, (uintptr_t)data, 64));
    uint64_t marker = 0x12345000;
    __asm__ volatile("msr par_el1, %0" :: "r"(marker) : "memory");
    translations();
    mutex = xSemaphoreCreateMutexStatic(&mutex_state);
    configASSERT(mutex);
    for (unsigned i = 0; i < 2; ++i)
        configASSERT(xTaskCreateStatic(worker, "npu", 2048, (void *)(uintptr_t)i,
                                      3, stacks[i], &tasks[i]));
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
