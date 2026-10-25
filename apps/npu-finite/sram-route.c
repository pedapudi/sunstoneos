/* Shared SRAM routing for the NPU lifetime and a READY guard on NPU register
 * access, linked only into the SRAM-routing variant of the finite numerical
 * image (apps/npu-finite/build-sram-route.py, docs/reference-backsolve-npu.md).
 *
 * The boot ROM sets SYSCTRL 0x03000008 bit 1 at every reset, which routes the
 * 512 KiB shared SRAM (CPU 0x00074000-0x000f3fff) to the CPU; board U-Boot
 * clears it, which routes the SRAM to the NPU (board_sun60iw2.c:119-133 at
 * u-boot-orangepi b791be84). An image loaded through FEL never runs U-Boot.
 *
 * The variant's linker options redirect four references from the unchanged
 * objects to this file: a733_npu_startup_init (to record the one sequencer),
 * npu_service_activate (route to the NPU after READY, before the controller's
 * first register access), npu_convolution_stop_poll (route back to the CPU
 * after local stop) and a733_npu_native_operations (refuse NPU register
 * access unless routed and READY). The EL3 restore before ROM return is in
 * sram-route-el3.c. */
#include <stdatomic.h>
#include "a733_mmio.h"
#include "a733_npu_native.h"
#include "a733_npu_startup.h"
#include "npu_convolution.h"
#include "npu_service.h"

#define SRAM_ROUTING UINT32_C(0x03000008)
#define SRAM_TO_CPU UINT32_C(2)

int __real_a733_npu_startup_init(struct a733_npu_startup *, const struct a733_npu_startup_config *,
    const struct a733_npu_startup_providers *, const struct a733_npu_startup_ops *, void *);
enum npu_service_result __real_npu_service_activate(struct npu_service *);
enum a733_npu_platform_result __real_npu_convolution_stop_poll(struct npu_convolution *);
extern const struct a733_npu_ops __real_a733_npu_native_operations;

/* Written once before the scheduler starts; read-only afterwards. */
static struct a733_npu_startup *sram_route_startup;
/* 1 from the readback that shows bit 1 clear until the restore begins. */
static atomic_uint sram_route_npu;

int __wrap_a733_npu_startup_init(struct a733_npu_startup *s, const struct a733_npu_startup_config *c,
    const struct a733_npu_startup_providers *p, const struct a733_npu_startup_ops *o, void *cookie)
{
    if (sram_route_startup) return 0;
    if (!__real_a733_npu_startup_init(s, c, p, o, cookie)) return 0;
    sram_route_startup = s;
    return 1;
}

static int startup_ready(void)
{
    return sram_route_startup &&
        __atomic_load_n(&sram_route_startup->stage, __ATOMIC_ACQUIRE) == A733_NPU_STARTUP_READY;
}

/* One acquire load and one load of the stage per register access; the access
 * itself already carries two DSB SY. A refusal makes no bus access, and the
 * controller then retains the instance as an I/O fault. */
static int ready(void)
{
    return atomic_load_explicit(&sram_route_npu, memory_order_acquire) && startup_ready();
}

static int guarded_read(void *native, unsigned offset, uint32_t *value)
{return ready() && __real_a733_npu_native_operations.read(native, offset, value);}
static int guarded_write(void *native, unsigned offset, uint32_t value)
{return ready() && __real_a733_npu_native_operations.write(native, offset, value);}
static uint64_t forward_microseconds(void *native)
{return __real_a733_npu_native_operations.microseconds(native);}
static void forward_delay(void *native, unsigned microseconds)
{__real_a733_npu_native_operations.delay(native, microseconds);}
static void forward_barrier(void *native)
{__real_a733_npu_native_operations.barrier(native);}
static int forward_publish(void *native, const struct a733_npu_mmu_image *image,
    const struct a733_npu_command *command, int bootstrap)
{return __real_a733_npu_native_operations.publish(native, image, command, bootstrap);}
static int forward_finish(void *native, const struct a733_npu_command *command, int canceled)
{return __real_a733_npu_native_operations.finish(native, command, canceled);}

const struct a733_npu_ops __wrap_a733_npu_native_operations = {
    guarded_read, guarded_write, forward_microseconds, forward_delay, forward_barrier,
    forward_publish, forward_finish
};

/* After READY and before the service activates: the board read on both ports
 * (board-bundles-20261025-6, bb9-read) found bit 1 set, so any other entry
 * value refuses. The word has no other writer in this image. */
enum npu_service_result __wrap_npu_service_activate(struct npu_service *service)
{
    if (!atomic_load_explicit(&sram_route_npu, memory_order_relaxed)) {
        if (!startup_ready()) return NPU_SERVICE_DENIED;
        const uint32_t entry = a733_mmio_read32_dsb_sy_both(SRAM_ROUTING);
        if (!(entry & SRAM_TO_CPU)) return NPU_SERVICE_FAULT;
        a733_mmio_write32_dsb_sy_both(SRAM_ROUTING, entry & ~SRAM_TO_CPU);
        if (a733_mmio_read32_dsb_sy_both(SRAM_ROUTING) != (entry & ~SRAM_TO_CPU)) return NPU_SERVICE_FAULT;
        atomic_store_explicit(&sram_route_npu, 1, memory_order_release);
    }
    return __real_npu_service_activate(service);
}

/* After local stop has closed the NPU gates and asserted its resets, and
 * before the finite client reports quiescence: route the SRAM back to the CPU.
 * A failed readback retains everything, so no normal return follows. */
enum a733_npu_platform_result __wrap_npu_convolution_stop_poll(struct npu_convolution *c)
{
    const enum a733_npu_platform_result result = __real_npu_convolution_stop_poll(c);
    if (result != A733_NPU_PLATFORM_READY || !atomic_load_explicit(&sram_route_npu, memory_order_relaxed))
        return result;
    atomic_store_explicit(&sram_route_npu, 0, memory_order_release);
    const uint32_t routed = a733_mmio_read32_dsb_sy_both(SRAM_ROUTING);
    a733_mmio_write32_dsb_sy_both(SRAM_ROUTING, routed | SRAM_TO_CPU);
    if (a733_mmio_read32_dsb_sy_both(SRAM_ROUTING) != (routed | SRAM_TO_CPU)) return A733_NPU_PLATFORM_RETAINED;
    return result;
}
