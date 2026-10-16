#ifndef SUNSTONE_A733_GPU_BACKEND_H
#define SUNSTONE_A733_GPU_BACKEND_H
#include "pvr_gpu_service.h"
#include "a733_gpu_memory.h"
#include "a733_gpu_platform.h"
#include "gicv3_spi.h"
#include "pvr_compute_mode.h"
#include <stdatomic.h>

#define A733_GPU_REGISTERS UINT32_C(0x01800000)
#define A733_GPU_GICD UINT32_C(0x03400000)
#define A733_GPU_INTID 95u

struct a733_gpu_backend_external {
    /* Mandatory proven platform implementation: block new GPU memory traffic,
     * drain outstanding traffic, and leave power OFF with its resource token
     * released before returning 1. Use the supplied CLOSE deadline. Retain
     * everything on failure. A power OFF operation alone does not satisfy this
     * contract. The provider must also establish safe subsequent power-on. */
    int (*isolate)(void *, struct a733_gpu_platform *, uint64_t deadline);
    int (*event)(void *, const unsigned char[PVR_CCB_COMMAND_BYTES]);
    void *context;
};
enum a733_gpu_backend_phase { A733_GPU_BACKEND_NEW, A733_GPU_BACKEND_BOUND,
    A733_GPU_BACKEND_POWERING, A733_GPU_BACKEND_IDENTIFY, A733_GPU_BACKEND_ACTIVE,
    A733_GPU_BACKEND_ISOLATED };
enum a733_gpu_backend_error { A733_GPU_BACKEND_OK, A733_GPU_BACKEND_STATE,
    A733_GPU_BACKEND_MAPPING, A733_GPU_BACKEND_CLOCK, A733_GPU_BACKEND_IDENTITY,
    A733_GPU_BACKEND_ACCESS, A733_GPU_BACKEND_MEMORY, A733_GPU_BACKEND_POWER,
    A733_GPU_BACKEND_ISOLATION, A733_GPU_BACKEND_INTERRUPT, A733_GPU_BACKEND_EVENT,
    A733_GPU_BACKEND_COMPUTE_MODE };
struct a733_gpu_backend {
    struct pvr_gpu_service *service;
    struct a733_gpu_memory *memory;
    struct a733_gpu_platform *power;
    struct a733_gpu_backend_external external;
    struct pvr_free_list_ops free_list_ops;
    void *free_list_context;
    struct gicv3_spi irq;
    struct pvr_compute_mode mode;
    atomic_uint error;
    uint64_t deadline, last_now, identity;
    uint32_t identity_step, legacy_revision, initialized;
    enum a733_gpu_backend_phase phase;
};
/* Zero storage. Bind before service initialization, at EL1 with MMU/cache on.
 * memory is initialized and OFFLINE; power is initialized and NEW. Config must
 * describe exactly that memory instance at 400 MHz. A successful call replaces
 * queue/start/control/stop/power/IRQ callbacks and context, leaving other config
 * fields intact. Optional free-list operations retain their original context
 * through forwarding callbacks. The saved context stays live and disjoint from
 * backend/device backing. Refusal preserves backend and config. Only translations and
 * CPU register reads occur; no GPU/GIC access or power operation occurs.
 *
 * All allowlisted GPU and GIC pages, including 0x0180f000 for core count and
 * capabilities, need pinned identity Device-nGnRnE mappings. Post-power reads
 * admit only the documented A733 MC1 profile before firmware startup. A count
 * above one faults with MULTICORE_REQUIRED and performs no secondary-bank reads.
 * Backend/service/memory/power/control storage, firmware and page lists must
 * remain private and mutually disjoint from device backing. One service worker
 * serializes lifecycle callbacks. IRQ callbacks use the service's critical
 * sections and can run on another core. They never take a task mutex.
 *
 * Caller independently configures SPI 63 / INTID 95 disabled, level-high, with
 * a qualified route/group/API-safe priority. DVFS interrupt 96 is separate.
 * Recovery, pinned firmware hash, reservation ownership and the physical
 * isolation implementation are mandatory before board execution. */
int a733_gpu_backend_bind(struct a733_gpu_backend *, struct pvr_gpu_service *,
    struct pvr_gpu_service_config *, struct a733_gpu_memory *, struct a733_gpu_platform *,
    const struct a733_gpu_backend_external *);
#endif
