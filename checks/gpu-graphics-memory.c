#include "a733_gpu_graphics_memory.h"
#include "a733_npu_reserved.h"
#include "platform.h"
#include <string.h>
extern const unsigned char gpu_firmware[], gpu_firmware_end[];
extern void platform_exit_privileged(unsigned) __attribute__((noreturn));
static struct a733_gpu_reserved firmware;
static struct a733_gpu_compute_reserved compute;
static struct a733_gpu_graphics_memory graphics;
static struct pvr_gpu_service service;
static const struct a733_gpu_graphics_profile profile = {{64, 64, 32}, 28, 4, 25, 3000000};
static StaticTask_t client_state;
static StackType_t client_stack[4096];
static unsigned enable_calls, clock_calls, recovered;
volatile unsigned gpu_graphics_memory_case = 0xfeed;
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{ (void)task; platform_panic(name, 0); }
static uint32_t word(uintptr_t cpu) { return __atomic_load_n((uint32_t *)cpu, __ATOMIC_ACQUIRE); }
static void kept(struct device_memory_pool *pool, struct device_memory_lease token, unsigned expected)
{
    struct device_memory_extent extent; enum device_memory_state state;
    configASSERT(device_memory_describe(pool, token, &extent, &state) && state == expected);
    configASSERT(!device_memory_release(pool, token));
}
static int substituted(void *ctx, const struct pvr_compute_program_config *p)
{ (void)ctx; (void)p; return 1; }
static int now(void *ctx, uint64_t *out)
{
    configASSERT(ctx == &graphics && xTaskGetCurrentTaskHandle() == service.worker);
    ++clock_calls; uint64_t counter, frequency;
    __asm__ volatile("mrs %0, cntpct_el0\n mrs %1, cntfrq_el0" : "=r"(counter), "=r"(frequency));
    *out = (counter / frequency) * 1000000 + (counter % frequency) * 1000000 / frequency;
    if (gpu_graphics_memory_case == 2 && graphics.phase == A733_GPU_GRAPHICS_READY) *out = service.deadline;
    return 1;
}
static int read_queue(void *ctx, enum pvr_ccb_region r, uint32_t off, void *out, size_t n)
{ configASSERT(ctx == &graphics); return a733_gpu_memory_read(&firmware.memory, r, off, out, n); }
static int write_queue(void *ctx, enum pvr_ccb_region r, uint32_t off, const void *in, size_t n)
{ configASSERT(ctx == &graphics); return a733_gpu_memory_write(&firmware.memory, r, off, in, n); }
static int barrier(void *ctx, enum pvr_ccb_barrier b)
{ configASSERT(ctx == &graphics); return a733_gpu_memory_barrier(&firmware.memory, b); }
/* Power and all GPU-facing operations are unavailable. The fixture proves
 * native preparation up to that boundary; none can manufacture GPU success. */
static int unavailable(void *ctx) { (void)ctx; configASSERT(0); return 0; }
static int read_reg(void *ctx, uint32_t off, uint32_t width, uint64_t *out)
{ (void)off; (void)width; (void)out; return unavailable(ctx); }
static int write_reg(void *ctx, uint32_t off, uint32_t width, uint64_t value)
{ (void)off; (void)width; (void)value; return unavailable(ctx); }
static int read32(void *ctx, uint32_t off, uint32_t *out)
{ (void)off; (void)out; return unavailable(ctx); }
static int write32(void *ctx, uint32_t off, uint32_t value)
{ (void)off; (void)value; return unavailable(ctx); }
static int compat(void *ctx, uint32_t off, void *out, size_t n)
{ (void)off; (void)out; (void)n; return unavailable(ctx); }
static int sync_read(void *ctx, uint32_t *out) { (void)out; return unavailable(ctx); }
static int sync_write(void *ctx, uint32_t in) { (void)in; return unavailable(ctx); }
static int refuse_power(void *ctx)
{
    configASSERT(ctx == &graphics && gpu_graphics_memory_case == 0); ++enable_calls;
    configASSERT(graphics.phase == A733_GPU_GRAPHICS_READY && graphics.vm.phase == A733_GPU_VM_MEMORY_PUBLISHED);
    configASSERT(compute.compute.phase == PVR_COMPUTE_SERVICE_READY && compute.vm.phase == A733_GPU_VM_MEMORY_PUBLISHED);
    configASSERT(service.free_lists.phase == PVR_FREE_LIST_IDLE && service.free_lists.count == 2);
    configASSERT(graphics.acquired[0] == 28 && graphics.acquired[1] == 28);
    for (unsigned i = 0; i < 2; ++i) {
        uintptr_t record = graphics.shared_extent.cpu + i * 64;
        configASSERT(word(record + 20) == (i ? 32 : 28));
        configASSERT(word(record + 28) == (i ? 24 : 28));
        configASSERT(word(record + 48) == (i ? 4 : 0));
    }
    configASSERT(service.mappings[25].address == graphics.object.address && service.mappings[25].pages == &graphics.firmware_page);
    /* Source-defined reconstruction changes only HWR state and LAST_GEOM. The
     * existing manager event/response checks are reused; these are real native
     * linked-record operations on the allocated uncached page. */
    for (unsigned i = 0; i < 2; ++i) {
        uintptr_t p = (uintptr_t)graphics.target.data[i];
        __atomic_store_n((uint32_t *)(p + 168), 0xac, __ATOMIC_RELEASE);
        __atomic_store_n((uint32_t *)(p + 172), 3, __ATOMIC_RELEASE);
    }
    unsigned char before[2][384];
    for (unsigned i = 0; i < 2; ++i) memcpy(before[i], graphics.target.data[i], 384);
    configASSERT(firmware.config.free_lists.ops.recover_targets(ctx, 2) == 1);
    for (unsigned i = 0; i < 2; ++i) configASSERT(!memcmp(before[i], graphics.target.data[i], 384));
    configASSERT(firmware.config.free_lists.ops.recover_targets(ctx, 1) == 1);
    for (unsigned i = 0; i < 2; ++i) {
        uint32_t flags = 0xa8, state = 9;
        memcpy(before[i] + 168, &flags, 4); memcpy(before[i] + 172, &state, 4);
        configASSERT(!memcmp(before[i], graphics.target.data[i], 384));
    }
    recovered = 1; return 0;
}
static void client(void *unused)
{
    (void)unused;
    struct pvr_gpu_service_request request = {PVR_GPU_SERVICE_OPEN, 4000000};
    struct pvr_gpu_service_reply reply; uint64_t ticket;
    configASSERT(pvr_gpu_service_send(&service, &request, 0, &ticket));
    configASSERT(pvr_gpu_service_collect(&service, ticket, &reply, 500));
    configASSERT(reply.result == PVR_GPU_SERVICE_OK && reply.owns_session);
    for (unsigned i = 0; i < 2000; ++i) {
        vTaskDelay(1); request = (struct pvr_gpu_service_request){PVR_GPU_SERVICE_STATUS, 0};
        configASSERT(pvr_gpu_service_send(&service, &request, 0, &ticket));
        configASSERT(pvr_gpu_service_collect(&service, ticket, &reply, 500));
        if (reply.phase == PVR_GPU_SERVICE_FAILED) break;
    }
    configASSERT(reply.phase == PVR_GPU_SERVICE_FAILED && reply.owns_session &&
        reply.graphics_phase == A733_GPU_GRAPHICS_FAILED && compute.compute.phase == PVR_COMPUTE_SERVICE_FAILED);
    if (gpu_graphics_memory_case == 0) configASSERT(reply.error == PVR_GPU_SERVICE_ENABLE && enable_calls == 1 && recovered);
    else configASSERT(reply.error == PVR_GPU_SERVICE_CLOCK && !enable_calls && !recovered);
    unsigned previous = clock_calls;
    vTaskDelay(2);
    configASSERT(a733_gpu_graphics_memory_poll(&graphics) == -1 &&
        a733_gpu_graphics_memory_begin(&graphics) == -1 && clock_calls == previous);
    kept(firmware.wb_pool, graphics.backing_lease, DEVICE_MEMORY_DEVICE);
    kept(firmware.wb_pool, graphics.payload_lease, DEVICE_MEMORY_DEVICE);
    kept(firmware.nc_pool, graphics.shared_lease, DEVICE_MEMORY_DEVICE);
    kept(firmware.wb_pool, graphics.table_lease, DEVICE_MEMORY_DEVICE);
    platform_puts(gpu_graphics_memory_case == 0 ?
        "PASS graphics memory: combined native publication, exact leases and linked HWRT recovery reach refused power boundary\n" :
        "PASS graphics memory: late final publication retains all parents and blocks compute/power\n");
    platform_exit(0);
}
void kernel_main(void)
{
    platform_init(); configASSERT(gpu_graphics_memory_case <= 2);
    struct device_memory_pool *wb = a733_memory_reservations_init(), *nc = a733_memory_uncached_init();
    configASSERT(wb && nc);
    struct device_memory_lease npu, entropy, pressure;
    struct device_memory_extent extent;
    configASSERT(device_memory_acquire(wb, 7, A733_NPU_RESERVED_BYTES, &npu, &extent));
    device_memory_lookup(wb, npu)->state = DEVICE_MEMORY_DEVICE;
    configASSERT(device_memory_acquire(wb, 8, 4096, &entropy, &extent));
    device_memory_lookup(wb, entropy)->state = DEVICE_MEMORY_DEVICE;
    configASSERT(a733_gpu_reserved_prepare(&firmware, 9, gpu_firmware,
        (size_t)(gpu_firmware_end - gpu_firmware), 400000000));
    configASSERT(a733_gpu_compute_reserved_prepare(&compute, &firmware, &service, 10));
    int (*saved_admit)(void *, const struct pvr_compute_program_config *) = compute.programs.admit;
    compute.programs.admit = substituted;
    configASSERT(!a733_gpu_graphics_memory_prepare(&graphics, &firmware, &compute, &service, 11, &profile));
    configASSERT(!graphics.self && !graphics.backing_lease.generation);
    compute.programs.admit = saved_admit;
    if (gpu_graphics_memory_case == 1)
        configASSERT(device_memory_acquire(wb, 99, 46 * 4096, &pressure, &extent));
    int result = a733_gpu_graphics_memory_prepare(&graphics, &firmware, &compute, &service, 11, &profile);
    if (gpu_graphics_memory_case == 1) {
        configASSERT(!result && graphics.phase == A733_GPU_GRAPHICS_FAILED && !graphics.table_lease.generation);
        kept(wb, graphics.backing_lease, DEVICE_MEMORY_RETAINED);
        kept(wb, graphics.payload_lease, DEVICE_MEMORY_RETAINED);
        kept(nc, graphics.shared_lease, DEVICE_MEMORY_RETAINED);
        configASSERT(!firmware.config.graphics && !firmware.config.free_lists.count);
        configASSERT(!a733_gpu_graphics_memory_prepare(&graphics, &firmware, &compute, &service, 11, &profile));
        platform_puts("PASS graphics memory: VM capacity refusal retains all three admitted parents\n");
        platform_exit_privileged(0);
    }
    configASSERT(result && a733_gpu_graphics_memory_validate(&graphics, &firmware.config, &service));
    struct pvr_free_list_page *saved = graphics.lists[1].pages;
    graphics.lists[1].pages = (void *)&compute;
    configASSERT(!a733_gpu_graphics_memory_validate(&graphics, &firmware.config, &service));
    graphics.lists[1].pages = saved;
    uint64_t physical = graphics.maps[0].physical;
    graphics.maps[0].physical = compute.payload_extent.physical;
    configASSERT(!a733_gpu_graphics_memory_validate(&graphics, &firmware.config, &service));
    graphics.maps[0].physical = physical;
    configASSERT(a733_gpu_graphics_memory_validate(&graphics, &firmware.config, &service));
    struct pvr_gpu_service_config *c = &firmware.config;
    c->queue = (struct pvr_ccb_ops){read_queue, write_queue, barrier, write32};
    c->start = (struct pvr_gpu_start_ops){now, read_reg, write_reg, sync_read, unavailable};
    c->control = (struct pvr_gpu_control_ops){now, read32, write32, compat, NULL};
    c->stop = (struct pvr_gpu_stop_ops){now, read_reg, write_reg, sync_write, sync_read, unavailable};
    c->enable = refuse_power; c->compute_mode = unavailable;
    configASSERT(pvr_gpu_service_init(&service, c, 3, 1));
    TaskHandle_t task = xTaskCreateStaticAffinitySet(client, "graphics-check", 4096, NULL, 2,
        client_stack, &client_state, 2);
    configASSERT(task && pvr_gpu_service_bind(&service, 0, task));
    vTaskStartScheduler(); platform_panic("scheduler", 0);
}
