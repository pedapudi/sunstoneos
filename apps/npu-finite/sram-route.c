/* Shared SRAM routing for the NPU lifetime and a READY guard on NPU register
 * access, linked only into the SRAM-routing variant of the finite numerical
 * image (apps/npu-finite/build-sram-route.py, docs/designs/npu-sram-route.md).
 *
 * The boot ROM sets SYSCTRL 0x03000008 bit 1 on every entry, which routes the
 * 512 KiB shared SRAM (CPU 0x00074000-0x000f3fff) to the CPU; board U-Boot
 * clears it at board init, before any NPU clock or reset changes, which
 * routes the SRAM to the NPU (board_sun60iw2.c:119-133, board.c:248-252 at
 * u-boot-orangepi b791be84). An image loaded through FEL never runs U-Boot.
 *
 * The variant's linker options redirect five references from the unchanged
 * objects to this file:
 * - a733_npu_startup_init records the one startup sequencer;
 * - a733_npu_platform_native_write clears bit 1 before the sequencer's first
 *   write of the NPU bus and reset word (CCU +0xb04), while the AHB, AXI and
 *   core resets are still asserted and the bus and module gates closed;
 * - npu_service_activate refuses activation unless that clear succeeded;
 * - npu_convolution_stop_poll sets bit 1 again after local stop;
 * - a733_npu_native_operations refuses NPU register access unless the SRAM is
 *   routed and startup is READY.
 * The EL1 restore after local stop is diagnostic: it reads back the CPU
 * routing early and turns a failure into RETAINED. The EL3 restore in
 * sram-route-el3.c, immediately before the ROM return, is the one that the
 * ROM depends on. */
#include <stdatomic.h>
#include "a733_mmio.h"
#include "a733_npu_native.h"
#include "a733_npu_platform_native.h"
#include "a733_npu_startup.h"
#include "npu_convolution.h"
#include "npu_service.h"

#define SRAM_ROUTING UINT32_C(0x03000008)
#define SRAM_TO_CPU UINT32_C(2)
#define CCU_NPU_CLOCK UINT32_C(0x02002b00)
#define CCU_NPU_BUS_RESET UINT32_C(0x02002b04)
/* Bus gate 0 and the core, AXI and AHB resets 16-18 (manual 4.1.6.120). */
#define NPU_BUS_GATE_AND_RESETS UINT32_C(0x00070001)
#define NPU_MODULE_GATE UINT32_C(0x80000000)

int __real_a733_npu_startup_init(struct a733_npu_startup *, const struct a733_npu_startup_config *,
    const struct a733_npu_startup_providers *, const struct a733_npu_startup_ops *, void *);
int __real_a733_npu_platform_native_write(void *, uintptr_t, uint32_t);
enum npu_service_result __real_npu_service_activate(struct npu_service *);
enum a733_npu_platform_result __real_npu_convolution_stop_poll(struct npu_convolution *);
extern const struct a733_npu_ops __real_a733_npu_native_operations;

/* Written once before the scheduler starts; read-only afterwards. */
static struct a733_npu_startup *sram_route_startup;
/* 1 from the readback that shows bit 1 clear until the restore begins. Only
 * the bootstrap task writes it; the service worker reads it. */
static atomic_uint sram_route_npu;
/* Set once the clear has been attempted, so a refusal is never retried. */
static unsigned sram_route_attempted;

int __wrap_a733_npu_startup_init(struct a733_npu_startup *s, const struct a733_npu_startup_config *c,
    const struct a733_npu_startup_providers *p, const struct a733_npu_startup_ops *o, void *cookie)
{
    if (sram_route_startup) return 0;
    if (!__real_a733_npu_startup_init(s, c, p, o, cookie)) return 0;
    sram_route_startup = s;
    return 1;
}

static unsigned startup_stage(void)
{
    return sram_route_startup ? __atomic_load_n(&sram_route_startup->stage, __ATOMIC_ACQUIRE) : UINT32_MAX;
}

/* Before the first write of CCU +0xb04: the stage is OPEN, PLL_NPU is locked
 * and the local open table is about to release the resets. Require the resets
 * still asserted and both gates closed, the ROM's routing (bit 1 set, as the
 * board read found it on both ports), then clear bit 1 and read it back. Any
 * refusal fails the write, so the sequencer retains its resources and never
 * releases a reset; no later step can route the SRAM. */
int __wrap_a733_npu_platform_native_write(void *cookie, uintptr_t address, uint32_t value)
{
    if (address == CCU_NPU_BUS_RESET && !sram_route_attempted) {
        sram_route_attempted = 1;
        if (startup_stage() != A733_NPU_STARTUP_OPEN ||
            (a733_mmio_read32_dsb_sy_both(CCU_NPU_BUS_RESET) & NPU_BUS_GATE_AND_RESETS) ||
            (a733_mmio_read32_dsb_sy_both(CCU_NPU_CLOCK) & NPU_MODULE_GATE)) return 0;
        const uint32_t entry = a733_mmio_read32_dsb_sy_both(SRAM_ROUTING);
        if (!(entry & SRAM_TO_CPU)) return 0;
        a733_mmio_write32_dsb_sy_both(SRAM_ROUTING, entry & ~SRAM_TO_CPU);
        if (a733_mmio_read32_dsb_sy_both(SRAM_ROUTING) != (entry & ~SRAM_TO_CPU)) return 0;
        atomic_store_explicit(&sram_route_npu, 1, memory_order_release);
    }
    return __real_a733_npu_platform_native_write(cookie, address, value);
}

/* One acquire load and one load of the stage per register access; the access
 * itself already carries two DSB SY. A refusal makes no bus access, and the
 * controller then retains the instance as an I/O fault. */
static int ready(void)
{
    return atomic_load_explicit(&sram_route_npu, memory_order_acquire) &&
        startup_stage() == A733_NPU_STARTUP_READY;
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

/* After READY: the controller opens only if the SRAM was routed before the
 * resets were released. */
enum npu_service_result __wrap_npu_service_activate(struct npu_service *service)
{
    if (startup_stage() != A733_NPU_STARTUP_READY) return NPU_SERVICE_DENIED;
    if (!atomic_load_explicit(&sram_route_npu, memory_order_relaxed)) return NPU_SERVICE_FAULT;
    return __real_npu_service_activate(service);
}

/* After local stop has closed the NPU gates and asserted its resets: set
 * bit 1 again and read it back. This is an early diagnostic readback; the
 * EL3 restore before the ROM return is the safety step. A failure returns
 * RETAINED, so the client never reports quiescence and no normal return
 * follows. */
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
