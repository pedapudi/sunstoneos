#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "aarch64_device_memory.h"
#include "probe.h"
static StaticTask_t task;
static StackType_t stack[1024];
extern uint64_t gpu_cache_permission_descriptor(unsigned);
static _Noreturn void fail(const char *why)
{ platform_puts("FAIL GPU output cache permission: "); platform_puts(why); platform_puts("\n"); platform_exit(1); }
#define REQUIRE(v, why) do { if (!(v)) fail(why); } while (0)
static void bytes_check(void)
{
    volatile const uint64_t *tags = (volatile const uint64_t *)(uintptr_t)OUTPUT_BASE;
    for (unsigned i = 0; i < TAG_BYTES / 8; ++i) REQUIRE(tags[i] == 0, "tag clear");
    for (unsigned i = 0; i < 3; ++i) {
        volatile const uint64_t *p = (volatile const uint64_t *)sample_address(i);
        for (unsigned j = 0; j < 8; ++j) REQUIRE(p[j] == PATTERN + 8 * i + j, "initialized bytes");
    }
}
static void run(void *unused)
{
    (void)unused;
    uint64_t el, mair, tcr;
    __asm__ volatile("mrs %0, CurrentEL\n mrs %1, mair_el1\n mrs %2, tcr_el1" : "=r"(el), "=r"(mair), "=r"(tcr));
    REQUIRE(el == 4 && mair == 0x44ff00 && tcr == UINT64_C(0x200803519), "EL1 controls");
    REQUIRE(aarch64_device_cache_stride() == 64, "cache stride");
    for (unsigned i = 0; i < 2; ++i) {
        uintptr_t address = OUTPUT_BASE + i * UINT64_C(0x200000);
        struct aarch64_translation t;
        REQUIRE(gpu_cache_permission_descriptor(i) == (address | OUTPUT_ATTR), "descriptor");
        REQUIRE(aarch64_device_translate(address, 0, &t) && t.physical == address &&
                t.attributes == 0xff && t.shareability == 3, "EL1 read translation");
        REQUIRE(!aarch64_device_translate(address, 1, &t), "EL1 write translation");
        REQUIRE(aarch64_device_translate_el0(address, 0, &t) && t.physical == address &&
                t.attributes == 0xff && t.shareability == 3, "EL0 read translation");
        REQUIRE(!aarch64_device_translate_el0(address, 1, &t), "EL0 write translation");
    }
    bytes_check();
    uint64_t before, after;
    __asm__ volatile("isb\n mrs %0, cntpct_el0" : "=r"(before) :: "memory");
    REQUIRE(aarch64_device_cache(AARCH64_CACHE_CLEAN_INVALIDATE, OUTPUT_BASE, OUTPUT_BYTES), "cache helper");
    __asm__ volatile("isb\n mrs %0, cntpct_el0" : "=r"(after) :: "memory");
    REQUIRE(after >= before, "counter");
    bytes_check();
    platform_puts("GPU output cache ticks="); platform_hex(after - before); platform_puts("\n");
    platform_puts(COMPLETE);
    platform_exit(0);
}
void kernel_main(void)
{
    platform_init();
    configASSERT(xTaskCreateStatic(run, "RO-cache", 1024, 0, 1, stack, &task));
    vTaskStartScheduler();
    fail("scheduler returned");
}
void vApplicationStackOverflowHook(TaskHandle_t handle, char *name)
{ (void)handle; (void)name; fail("stack overflow"); }
