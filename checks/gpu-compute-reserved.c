#include "a733_gpu_compute_reserved.h"
#include "a733_npu_reserved.h"
#include "platform.h"
#include <string.h>

extern const unsigned char gpu_firmware[], gpu_firmware_end[];
extern void platform_exit_privileged(unsigned) __attribute__((noreturn));
static struct a733_gpu_reserved firmware;
static struct a733_gpu_compute_reserved compute, copied;
static struct pvr_gpu_service service;
volatile unsigned gpu_compute_reserved_case = 0xfeed;
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{ (void)task; platform_panic(name, 0); }
static void kept(struct device_memory_pool *p, struct device_memory_lease l,
                 struct device_memory_extent e, enum device_memory_state expected)
{
    struct device_memory_extent actual; enum device_memory_state state;
    configASSERT(device_memory_describe(p, l, &actual, &state) && state == expected);
    configASSERT(actual.cpu == e.cpu && actual.physical == e.physical && actual.bytes == e.bytes);
    configASSERT(!device_memory_release(p, l));
}
void kernel_main(void)
{
    platform_init(); configASSERT(gpu_compute_reserved_case <= 1);
    struct device_memory_pool *wb = a733_memory_reservations_init();
    struct device_memory_pool *nc = a733_memory_uncached_init(); configASSERT(wb && nc);
    /* Existing NPU/entropy preparation is already checked independently. These
     * leases model only their complete retained occupancy for this new layout. */
    struct device_memory_lease npu, entropy, pressure = {0};
    struct device_memory_extent npu_range, entropy_range, pressure_range;
    configASSERT(device_memory_acquire(wb, 7, A733_NPU_RESERVED_BYTES, &npu, &npu_range));
    configASSERT(device_memory_acquire(wb, 8, 4096, &entropy, &entropy_range));
    device_memory_lookup(wb, npu)->state = DEVICE_MEMORY_DEVICE;
    device_memory_lookup(wb, entropy)->state = DEVICE_MEMORY_DEVICE;
    configASSERT(a733_gpu_reserved_prepare(&firmware, 9, gpu_firmware,
                 (size_t)(gpu_firmware_end - gpu_firmware), 400000000));
    if (gpu_compute_reserved_case == 1) {
        /* Leave 99 pages: enough for the 88-page payload parent, but one page
         * short of the actual 12-page VM plan. The six NC pages are acquired. */
        configASSERT(device_memory_acquire(wb, 99, 113 * 4096u, &pressure, &pressure_range));
    }
    struct pvr_compute_service *result = a733_gpu_compute_reserved_prepare(&compute, &firmware, &service, 10);
    configASSERT(compute.requirements.table_pages == 12 && compute.requirements.table_bytes == 49152);
    configASSERT(compute.requirements.directory_pages == 3 && compute.requirements.leaf_tables == 8);
    configASSERT(compute.requirements.mapped_pages == 88 && compute.requirements.mapped_bytes == 360448);
    configASSERT(compute.payload_extent.bytes == 360448 && compute.firmware_extent.bytes == 24576);
    if (gpu_compute_reserved_case == 1) {
        configASSERT(!result && !compute.table_lease.generation && !firmware.config.compute);
        kept(wb, compute.payload_lease, compute.payload_extent, DEVICE_MEMORY_RETAINED);
        kept(nc, compute.firmware_lease, compute.firmware_extent, DEVICE_MEMORY_RETAINED);
        configASSERT(device_memory_release(wb, pressure));
        configASSERT(!a733_gpu_compute_reserved_prepare(&copied, &firmware, &service, 10));
        configASSERT(!a733_gpu_compute_reserved_prepare(&compute, &firmware, &service, 10));
        platform_puts("PASS compute reservations: exact planner capacity refusal retains both acquired parents\n");
    } else {
        configASSERT(result == &compute.compute && result->phase == PVR_COMPUTE_SERVICE_BOUND);
        configASSERT(firmware.config.compute == result && result->service == &service && result->config == &compute.config);
        configASSERT(compute.vm.phase == A733_GPU_VM_MEMORY_PREPARED && compute.table_extent.bytes == 49152);
        configASSERT(compute.boot.write_count == PVR_MIPS_WRITES);
        for (unsigned i = 0; i < 6; ++i) {
            const struct pvr_mips_object *m = &compute.mappings[PVR_FW_RESOURCE_COUNT + i];
            configASSERT(m->pages == compute.config.firmware[i].pages && m->page_count == 1 &&
                         m->address == compute.config.firmware_address[i] && m->cache_policy == PVR_MIPS_UNCACHED);
        }
        configASSERT(compute.job.admit(compute.job.admission_context, &compute.job) == 1);
        configASSERT(!pvr_gpu_service_init(&service, &firmware.config, 2, 1));
        configASSERT(!service.initialized && !firmware.config.compute_mode);
        for (unsigned i = 0; i < 32; ++i)
            configASSERT(compute.image.expected[i] == ((17u + (i + 1u) * 3u) ^ (0x9e3779b9u + (i << 16))));
        /* New proof refuses copied bindings, changed mappings and stale leases
         * without touching generated buffers or manufacturing readiness. */
        copied = compute;
        configASSERT(!copied.job.admit(&copied.binding, &copied.job));
        configASSERT(compute.programs.pds.base == UINT64_C(0xda00000000) &&
                     compute.programs.usc.base == UINT64_C(0xe000000000) &&
                     compute.programs.general.base == UINT64_C(0x8000000000));
        volatile unsigned char *system = firmware.config.resources[PVR_FW_SYSTEM_INIT].storage.cpu;
        unsigned char heap_byte = system[12]; system[12] ^= 1;
        configASSERT(!compute.job.admit(compute.job.admission_context, &compute.job));
        system[12] = heap_byte;
        uint64_t saved = compute.maps[0].physical;
        compute.maps[0].physical = firmware.wb_extent.physical;
        configASSERT(!compute.job.admit(compute.job.admission_context, &compute.job));
        compute.maps[0].physical = saved;
        struct device_memory_lease token = compute.table_lease;
        ++compute.table_lease.generation;
        configASSERT(!compute.job.admit(compute.job.admission_context, &compute.job));
        compute.table_lease = token;
        configASSERT(compute.job.admit(compute.job.admission_context, &compute.job));
        /* Exercise the real native reservation callback after completion. */
        service.phase=PVR_GPU_SERVICE_READY;service.epoch=17;
        service.queue.phase=PVR_CCB_ACTIVE;service.queue.epoch=17;
        compute.compute.phase=PVR_COMPUTE_SERVICE_COMPLETE;compute.compute.epoch=17;
        compute.compute.client.phase=PVR_CCCB_IDLE;compute.compute.client.epoch=17;
        compute.compute.client.serial=1;compute.compute.client.queue=&service.queue;
        compute.compute.job.phase=PVR_JOB_COMPLETE;compute.compute.job.epoch=17;
        compute.compute.job.serial=1;compute.compute.job.sequence=1;
        compute.compute.job.transport=&compute.compute.client;compute.compute.job.config=&compute.job;
        *(volatile uint32_t *)compute.config.firmware[5].storage.cpu=1;
        configASSERT(compute.job.admit(compute.job.admission_context,&compute.job));
        service.owner=PVR_GPU_SERVICE_CLIENTS;
        configASSERT(!compute.job.admit(compute.job.admission_context,&compute.job));
        service.owner=0;
        ++compute.compute.job.epoch;
        configASSERT(!compute.job.admit(compute.job.admission_context,&compute.job));
        --compute.compute.job.epoch;
        *(volatile uint32_t *)compute.config.firmware[5].storage.cpu=0;
        configASSERT(!compute.job.admit(compute.job.admission_context,&compute.job));
        *(volatile uint32_t *)compute.config.firmware[5].storage.cpu=1;
        ++compute.compute.client.serial;
        configASSERT(!compute.job.admit(compute.job.admission_context,&compute.job));
        --compute.compute.client.serial;
        configASSERT(compute.job.admit(compute.job.admission_context,&compute.job));
        unsigned wb_pages = 0, nc_pages = 0;
        for (unsigned i = 0; i < DEVICE_MEMORY_MAX_ALLOCATIONS; ++i) {
            if (wb->allocations[i].state != DEVICE_MEMORY_FREE) wb_pages += wb->allocations[i].pages;
            if (nc->allocations[i].state != DEVICE_MEMORY_FREE) nc_pages += nc->allocations[i].pages;
        }
        configASSERT(wb_pages == 400 && nc_pages == 51);
        kept(wb, compute.payload_lease, compute.payload_extent, DEVICE_MEMORY_DEVICE);
        kept(nc, compute.firmware_lease, compute.firmware_extent, DEVICE_MEMORY_DEVICE);
        kept(wb, compute.table_lease, compute.table_extent, DEVICE_MEMORY_DEVICE);
        configASSERT(!a733_gpu_compute_reserved_prepare(&compute, &firmware, &service, 10));
        platform_puts("PASS compute reservations: actual VM/program/context/job builders and real lease admission bind native service\n");
    }
    kept(wb, npu, npu_range, DEVICE_MEMORY_DEVICE); kept(wb, entropy, entropy_range, DEVICE_MEMORY_DEVICE);
    kept(wb, firmware.wb_lease, firmware.wb_extent, DEVICE_MEMORY_DEVICE);
    kept(nc, firmware.nc_lease, firmware.nc_extent, DEVICE_MEMORY_DEVICE);
    platform_exit_privileged(0);
}
