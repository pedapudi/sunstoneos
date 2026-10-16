#include "a733_gpu_reserved.h"
#include "a733_npu_reserved.h"
#include "a733_memory_boot.h"
#include "aarch64_device_memory.h"
#include "npu-convolution-data.h"
#include "platform.h"
#include <string.h>

extern const unsigned char gpu_firmware[], gpu_firmware_end[];
extern void platform_exit_privileged(unsigned) __attribute__((noreturn));
static struct a733_gpu_reserved gpu;
static struct a733_npu_reserved npu;
static struct pvr_gpu_service service;
static unsigned char guard[4096] __attribute__((aligned(4096)));
volatile unsigned gpu_reserved_case = 0xfeed;

/* Native RAM AT/cache instructions execute unchanged. Only the observed PA of
 * the modeled, unused NPU register page differs. No device commands are issued. */
int __real_aarch64_device_translate(uintptr_t, int, struct aarch64_translation *);
int __wrap_aarch64_device_translate(uintptr_t p, int write, struct aarch64_translation *out)
{
    int result = __real_aarch64_device_translate(p, write, out);
    if (result && p >= 0x09000000 && p < 0x09001000)
        out->physical = p - 0x09000000 + 0x03600000;
    return result;
}
static void fatal(void *cookie)
{ (void)cookie; platform_panic("native fault", 0); }
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{ (void)task; platform_panic(name, 0); }
static void mapped(uintptr_t p, unsigned attr)
{
    struct aarch64_translation t;
    configASSERT(aarch64_device_translate(p, 0, &t));
    configASSERT(t.physical == p && t.attributes == attr &&
                 (t.shareability == 3 || (attr == 0x44 && t.shareability == 2)));
}
static void allocation(struct device_memory_pool *pool, struct device_memory_lease lease,
                       struct device_memory_extent expected, enum device_memory_state state)
{
    struct device_memory_extent extent;
    enum device_memory_state observed;
    configASSERT(device_memory_describe(pool, lease, &extent, &observed));
    configASSERT(extent.cpu == expected.cpu && extent.physical == expected.physical &&
                 extent.bytes == expected.bytes && observed == state);
    configASSERT(!device_memory_release(pool, lease));
}
void kernel_main(void)
{
    platform_init();
    configASSERT(gpu_reserved_case <= 1);
    memset(guard, 0x6d, sizeof(guard));
    mapped(0x41bff000, 0xff); mapped(0x41c00000, 0x44);
    mapped(0x41dff000, 0x44); mapped(0x41e00000, 0xff);
    /* Actual boot helper refuses all runtime attribute changes. */
    configASSERT(!a733_memory_boot_uncached(NULL));
    struct device_memory_pool *wb = a733_memory_reservations_init();
    struct device_memory_pool *nc = a733_memory_uncached_init();
    configASSERT(wb && nc && wb != nc && wb->backing.bytes == 2097152 &&
                 nc->backing.cpu == 0x41c00000 && nc->backing.bytes == 2097152);
    configASSERT(a733_memory_uncached_init() == nc && wb->backing.cpu + wb->backing.bytes <= nc->backing.cpu);
    configASSERT(a733_npu_reserved_prepare(&npu, 7, fixture_model, sizeof(fixture_model),
                                           0x09000000, fatal, NULL) == &npu.convolution);
    configASSERT(npu.convolution.prepared && npu.pool == wb);
    uint64_t wb_generation = wb->next_generation, nc_generation = nc->next_generation;
    size_t bytes = (size_t)(gpu_firmware_end - gpu_firmware);
    configASSERT(!a733_gpu_reserved_prepare(&gpu, 8, gpu_firmware, bytes, 0));
    configASSERT(!a733_gpu_reserved_prepare(&gpu, 8, (void *)nc->backing.cpu, bytes, 400000000));
    configASSERT(wb->next_generation == wb_generation && nc->next_generation == nc_generation && !gpu.admitted);
    struct device_memory_lease blocker = {0};
    struct device_memory_extent blocked = {0};
    if (gpu_reserved_case == 1)
        configASSERT(device_memory_acquire(nc, 99, nc->backing.bytes, &blocker, &blocked));
    struct pvr_gpu_service_config *config = a733_gpu_reserved_prepare(&gpu, 8, gpu_firmware, bytes, 400000000);
    configASSERT(gpu.admitted && gpu.wb_pool == wb && gpu.nc_pool == nc);
    configASSERT(gpu.wb_extent.bytes == A733_GPU_RESERVED_WB_BYTES);
    configASSERT(gpu.wb_extent.cpu == npu.extent.cpu + npu.extent.bytes);
    configASSERT(gpu.wb_lease.owner == 8 && gpu.wb_lease.generation != npu.lease.generation);
    if (gpu_reserved_case == 1) {
        configASSERT(!config && !gpu.prepared && !gpu.nc_lease.generation);
        allocation(wb, gpu.wb_lease, gpu.wb_extent, DEVICE_MEMORY_RETAINED);
        configASSERT(device_memory_release(nc, blocker));
        configASSERT(!a733_gpu_reserved_prepare(&gpu, 8, gpu_firmware, bytes, 400000000));
        platform_puts("PASS reserved GPU: exhausted uncached pool retains admitted write-back parent\n");
    } else {
        configASSERT(config == &gpu.config && gpu.prepared && gpu.memory.phase == A733_GPU_MEMORY_OFFLINE);
        configASSERT(gpu.nc_extent.bytes == A733_GPU_RESERVED_NC_BYTES && gpu.nc_lease.owner == 8);
        configASSERT(gpu.boot.write_count == PVR_MIPS_WRITES && gpu.boot.physical_address_bits == 36);
        size_t offset = 0;
        for (unsigned i = 0; i < 3 + PVR_FW_RESOURCE_COUNT; ++i) {
            if (i == 3) { configASSERT(offset == gpu.wb_extent.bytes); offset = 0; }
            const struct pvr_mips_buffer *b = i < 3 ? &config->firmware_buffers[i] : &config->resources[i - 3];
            struct device_memory_extent parent = i < 3 ? gpu.wb_extent : gpu.nc_extent;
            configASSERT((uintptr_t)b->storage.cpu == parent.cpu + offset && b->storage.bytes == b->page_count * 4096u);
            for (unsigned j = 0; j < b->page_count; ++j) {
                configASSERT(b->pages[j] == parent.physical + offset + j * 4096u);
                mapped((uintptr_t)b->storage.cpu + j * 4096u, i < 3 ? 0xff : 0x44);
            }
            offset += b->storage.bytes;
        }
        configASSERT(offset == gpu.nc_extent.bytes);
        /* Real service admission refuses the missing power/isolation callbacks. */
        configASSERT(!pvr_gpu_service_init(&service, config, 2, 1) && !service.initialized);
        uint32_t value = 0x12345678, observed = 0;
        configASSERT(a733_gpu_memory_write(&gpu.memory, PVR_CCB_HOST_CONTROL, 0, &value, 4));
        configASSERT(a733_gpu_memory_read(&gpu.memory, PVR_CCB_HOST_CONTROL, 0, &observed, 4) && observed == value);
        configASSERT(a733_gpu_memory_publish(&gpu.memory));
        configASSERT(gpu.memory.phase == A733_GPU_MEMORY_PUBLISHED);
        allocation(wb, gpu.wb_lease, gpu.wb_extent, DEVICE_MEMORY_DEVICE);
        allocation(nc, gpu.nc_lease, gpu.nc_extent, DEVICE_MEMORY_DEVICE);
        configASSERT(!a733_gpu_reserved_prepare(&gpu, 8, gpu_firmware, bytes, 400000000));
        platform_puts("PASS reserved GPU: native mappings, actual NPU coexistence, firmware preparation and publication\n");
    }
    allocation(wb, npu.lease, npu.extent, DEVICE_MEMORY_DEVICE);
    for (unsigned i = 0; i < sizeof(guard); ++i) configASSERT(guard[i] == 0x6d);
    platform_exit_privileged(0);
}
