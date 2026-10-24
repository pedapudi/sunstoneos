#include "a733_memory_local.h"
#include "a733_cached_memory.h"
#include "a733_cached_devices.h"
#include "aarch64_device_memory.h"
#include "cached.h"
#include "npu-convolution-data.h"
#include <string.h>
#define MONITOR __attribute__((section(".bss.a733_smp_monitor"), aligned(128)))
unsigned char fixture_el3_stacks[8 * 16384] MONITOR;
unsigned char fixture_el1_stacks[8 * 32768] __attribute__((aligned(4096)));
static volatile uint64_t tables_published MONITOR;
static volatile uint64_t el3_active[8] MONITOR;
static unsigned el1_observed[8];
extern unsigned char __bss_start[], __bss_end[], __tables_start[], __tables_end[];
extern unsigned char __device_pool_start[], __device_pool_end[];
extern const unsigned char gpu_firmware[], gpu_firmware_end[];
extern volatile uint8_t ucPrimaryCoreInitDoneFlag;
extern int a733_smp_pre_scheduler(unsigned long, uint64_t, uint64_t);
extern _Noreturn void fixture_exit(unsigned, uint64_t);
struct a733_cached_runtime a733_cached_runtime[8];
static void check(int condition, unsigned line)
{
    if (condition) return;
    char message[] = "FAIL 00000000\n";
    for (unsigned i = 0; i < 8; ++i) {
        unsigned digit = (line >> (i * 4)) & 15;
        message[12 - i] = (char)(digit < 10 ? '0' + digit : 'a' + digit - 10);
    }
    register uintptr_t operation __asm__("x0") = 4;
    register uintptr_t argument __asm__("x1") = (uintptr_t)message;
    __asm__ volatile("hlt #0xf000" : "+r"(operation), "+r"(argument) :: "memory");
    fixture_exit(1, line);
}
#define CHECK(x) check(!!(x), __LINE__)
void __wrap_platform_panic(const char *file, unsigned line)
{ (void)file; check(0, line); __builtin_unreachable(); }
static void fatal(void *cookie)
{ (void)cookie; fixture_exit(1, 0xfafa); }
const struct a733_cached_device_inputs a733_cached_device_inputs = {
    .npu_model = fixture_model, .npu_model_bytes = sizeof(fixture_model),
    .gpu_firmware = gpu_firmware, .gpu_firmware_bytes = GPU_FIXTURE_BYTES,
    .gpu_clock_hz = 400000000, .npu_owner = 7, .gpu_owner = 8, .compute_owner = 9,
    .fatal = fatal
};
uint64_t a733_smp_counter(void)
{ uint64_t value; __asm__ volatile("isb\n mrs %0, cntpct_el0" : "=r"(value)); return value; }
static void barrier(void) { __asm__ volatile("dsb sy" ::: "memory"); }
static void wait_device(volatile uint64_t *value)
{ while (!*value) { CHECK(a733_smp_counter() < UINT32_MAX); barrier(); } }

void fixture_el3_prepare(unsigned core)
{
    if (!core) {
        memset(__bss_start, 0, (size_t)(__bss_end - __bss_start));
        /* A live lower translation regime must refuse before touching tables. */
        uint64_t old;
        __asm__ volatile("mrs %0, sctlr_el1" : "=r"(old));
        uint64_t live = old | 1;
        __asm__ volatile("msr sctlr_el1, %0\n isb" :: "r"(live) : "memory");
        CHECK(!a733_cached_memory_prepare());
        CHECK(!a733_cached_el3_l1[1] && !a733_cached_el1_l1[1]);
        __asm__ volatile("msr sctlr_el1, %0\n isb" :: "r"(old) : "memory");
        CHECK(a733_cached_memory_prepare());
        CHECK(a733_cached_el3_l2[14] & (UINT64_C(1) << 5));
        CHECK((a733_cached_el3_l2[14] & 31) == 9 && (a733_cached_el1_l2[14] & 31) == 9);
        CHECK(a733_cpu_coherence_publish(0x41000000, 0x200000, UINT32_MAX) == 1);
        CHECK(a733_cpu_coherence_publish(0x41200000, (uintptr_t)__bss_end - 0x41200000, UINT32_MAX) == 1);
        CHECK(a733_cpu_coherence_publish((uintptr_t)__tables_start,
            (size_t)(__tables_end - __tables_start), UINT32_MAX) == 1);
        barrier(); tables_published = 1; barrier();
    } else wait_device(&tables_published);
}
static uint64_t el3_translation(uintptr_t address, int write)
{
    uint64_t par;
    if (write) __asm__ volatile("at s1e3w, %1\n isb\n mrs %0, par_el1" : "=r"(par) : "r"(address) : "memory");
    else __asm__ volatile("at s1e3r, %1\n isb\n mrs %0, par_el1" : "=r"(par) : "r"(address) : "memory");
    return par;
}
static void el3_range(uintptr_t begin, uintptr_t end, unsigned attribute)
{
    for (uintptr_t a = begin; a < end; a += 4096) for (int write = 0; write != 2; ++write) {
        uint64_t p = el3_translation(a, write);
        CHECK(!(p & 1) && (p >> 56) == attribute);
        CHECK((p & UINT64_C(0x000ffffffffff000)) == a && (p & (1u << 9)));
    }
}
void fixture_el3_mapped(unsigned core)
{
    barrier(); el3_active[core] = 1; barrier();
    for (unsigned i = 0; i < 8; ++i) wait_device(&el3_active[i]);
    el3_range((uintptr_t)__device_pool_start, (uintptr_t)__device_pool_end, 0xff);
    el3_range(0x41c00000, 0x41e00000, 0x44);
    el3_range(0x41bff000, 0x41c00000, 0xff);
    el3_range(0x41e00000, 0x41e01000, 0xff);
    CHECK(el3_translation(0x41000000, 1) & 1);
    CHECK(!a733_cached_memory_prepare()); /* Enabled profiles cannot be rewritten. */
}
static void el1_range(uintptr_t begin, uintptr_t end, unsigned attribute)
{
    for (uintptr_t a = begin; a < end; a += 4096) for (int write = 0; write != 2; ++write) {
        struct aarch64_translation t;
        CHECK(aarch64_device_translate(a, write, &t));
        CHECK(t.physical == a && t.attributes == attribute);
        CHECK(attribute == 0xff ? t.shareability == 3 : t.shareability == 2 || t.shareability == 3);
    }
}
static void held(struct device_memory_pool *pool, struct device_memory_lease lease)
{
    struct device_memory_extent extent; enum device_memory_state state;
    CHECK(device_memory_describe(pool, lease, &extent, &state) && state == DEVICE_MEMORY_DEVICE);
    CHECK(!device_memory_release(pool, lease));
}
void fixture_el1(unsigned core)
{
    a733_cached_runtime[core].epoch = 1;
    a733_cached_runtime[core].startup_deadline = UINT32_MAX;
    el1_range((uintptr_t)__device_pool_start, (uintptr_t)__device_pool_end, 0xff);
    el1_range(0x41c00000, 0x41e00000, 0x44);
    el1_range(0x41bff000, 0x41c00000, 0xff);
    el1_range(0x41e00000, 0x41e01000, 0xff);
    struct aarch64_translation t;
    CHECK(!aarch64_device_translate(0x42000000, 0, &t));
    CHECK(!aarch64_device_translate(0x421f0000, 0, &t));
    CHECK(!aarch64_device_translate(0x41000000, 1, &t));
    CHECK(!a733_cached_memory_prepare());
    if (!core) {
        CHECK(!a733_cached_devices_prepare(&a733_cached_device_inputs)); /* Actual barrier has not admitted primary. */
        CHECK(!a733_cached_devices_prepared());
    }
    CHECK(a733_smp_pre_scheduler(core, 1, UINT32_MAX / 24));
    if (!core) {
        a733_cached_runtime[0].startup_deadline = 0;
        CHECK(!a733_smp_device_preparation_admitted());
        a733_cached_runtime[0].startup_deadline = UINT32_MAX;
        CHECK(a733_smp_device_preparation_admitted());
        /* As platform/a733-smp-cached/continue.c does on core 0. */
        CHECK(a733_memory_local_prepare());
        CHECK(a733_cached_devices_prepare(&a733_cached_device_inputs));
        CHECK(!a733_cached_devices_prepare(&a733_cached_device_inputs));
        struct a733_cached_devices *d = a733_cached_devices_prepared(); CHECK(d);
        CHECK(d->npu.convolution.prepared && !d->npu.convolution.bound);
        CHECK(d->compute.compute.phase == PVR_COMPUTE_SERVICE_BOUND && !d->gpu_service.initialized);
        CHECK(d->compute.requirements.table_pages == 12);
        CHECK(d->compute.job.admit(d->compute.job.admission_context, &d->compute.job) == 1);
        unsigned wb = 0, nc = 0;
        for (unsigned i = 0; i < DEVICE_MEMORY_MAX_ALLOCATIONS; ++i) {
            if (d->gpu.wb_pool->allocations[i].state != DEVICE_MEMORY_FREE) wb += d->gpu.wb_pool->allocations[i].pages;
            if (d->gpu.nc_pool->allocations[i].state != DEVICE_MEMORY_FREE) nc += d->gpu.nc_pool->allocations[i].pages;
        }
        CHECK(wb == 399 && nc == 51);
        held(d->npu.pool, d->npu.lease); held(d->gpu.wb_pool, d->gpu.wb_lease);
        held(d->gpu.nc_pool, d->gpu.nc_lease); held(d->gpu.wb_pool, d->compute.payload_lease);
        held(d->gpu.wb_pool, d->compute.table_lease); held(d->gpu.nc_pool, d->compute.firmware_lease);
        __atomic_store_n(&ucPrimaryCoreInitDoneFlag, 1, __ATOMIC_RELEASE);
        CHECK(!a733_smp_device_preparation_admitted());
    } else {
        CHECK(!a733_smp_device_preparation_admitted());
        while (!__atomic_load_n(&ucPrimaryCoreInitDoneFlag, __ATOMIC_ACQUIRE))
            CHECK(a733_smp_counter() < UINT32_MAX);
        struct a733_cached_devices *d = a733_cached_devices_prepared(); CHECK(d);
        CHECK(d->npu.convolution.prepared && d->compute.compute.phase == PVR_COMPUTE_SERVICE_BOUND);
    }
    __atomic_store_n(&el1_observed[core], 1, __ATOMIC_RELEASE);
    if (!core) {
        for (unsigned i = 0; i < 8; ++i) while (!__atomic_load_n(&el1_observed[i], __ATOMIC_ACQUIRE))
            CHECK(a733_smp_counter() < UINT32_MAX);
        fixture_exit(0, 0);
    }
    for (;;) __asm__ volatile("wfe");
}
