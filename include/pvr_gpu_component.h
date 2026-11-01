#ifndef SUNSTONE_PVR_GPU_COMPONENT_H
#define SUNSTONE_PVR_GPU_COMPONENT_H
/* Components hosted by the GPU service worker (drivers/gpu/pvr_gpu_service.c,
 * docs/designs/gpu-compute-queue.md).
 *
 * Status: prototype. Nothing implements these declarations yet.
 *
 * A component owns firmware contexts and their client rings, and the jobs
 * that run on them. The worker owns the firmware, the kernel command queue,
 * power, the clock, the interrupt and the device lifecycle. It calls the
 * operations below in its own task, at EL1, one at a time; no operation runs
 * concurrently with another operation of any component. Every operation is
 * bounded and waits for no tick. The compute queue is the first component;
 * the mimus session takes this form during migration and is then removed. */
#include <stdatomic.h>
#include <stdint.h>
#include "pvr_mips.h"

struct pvr_gpu_service;
struct pvr_gpu_service_config;

#define PVR_GPU_SERVICE_COMPONENTS 4u

/* Flags reported by the deadline operation. */
#define PVR_GPU_COMPONENT_IN_FLIGHT 1u   /* a kicked job is waiting for its timeline; deadline is set */
#define PVR_GPU_COMPONENT_SUBMITTING 2u  /* ring writes before a kick remain: the CPU-run block is skipped */
#define PVR_GPU_COMPONENT_QUEUED 4u      /* accepted jobs wait to be kicked; deadline covers their not_after */
struct pvr_gpu_component_timing {
    uint64_t deadline;   /* earliest absolute deadline of any flagged work */
    uint64_t progress;   /* changes whenever the component advanced */
    uint32_t flags;
};

/* Registration flag: the component runs compute work, so the worker checks
 * the powered core mode (config.compute_mode) before firmware start. */
#define PVR_GPU_COMPONENT_COMPUTE 1u

struct pvr_gpu_component_ops {
    /* At OPEN, before power: copy at most capacity firmware objects into
     * objects, for the MIPS table that the worker builds. Returns the count.
     * The count is the same at every OPEN of the boot. */
    unsigned (*objects)(void *, struct pvr_mips_object *objects, unsigned capacity);
    /* At OPEN, before power: write context bytes, program pages and timeline
     * words. 1 accepted, 0 failed (the worker fails the service). */
    int (*prepare)(void *, uint64_t deadline);
    /* PUBLISHING phase: one bounded cache or table step per call.
     * 1 published, 0 pending, -1 fault. */
    int (*publish)(void *);
    /* READY phase, after the worker serviced firmware events: take queued
     * jobs, kick them, observe timelines and post records.
     * 1 or 0 normal, -1 a fault the service must take. */
    int (*poll)(void *);
    /* Every firmware event the worker drains. 0 for a malformed event or a
     * context reset that names this component, else 1. */
    int (*event)(void *, const unsigned char command[88]);
    /* The service failed: refuse every later submit and post FAULT for every
     * owed record. No device access and no resource release. */
    void (*abort)(void *);
    /* Before device CLOSE: seal admission when nothing is queued, in flight
     * or owed. 1 sealed, 0 busy with nothing changed. */
    int (*close)(void *);
    /* Fill timing; returns timing.flags. */
    unsigned (*deadline)(const void *, struct pvr_gpu_component_timing *timing);
    /* Performance hold: stop kicking queued jobs. 1 once nothing is in
     * flight, 0 while kicked jobs remain. Queued jobs stay queued. */
    int (*hold)(void *);
    /* End of a hold, after the firmware resumed at its new clock: kick
     * queued jobs again. 1 done, 0 refused. */
    int (*release)(void *);
    /* After healthy STOPPED and before the next OPEN epoch: check retained
     * leases, rebuild mutable firmware context bytes, restore each timeline to
     * its last sequence. 1 ready, 0 refused without writes, -1 failed with
     * backing retained. */
    int (*reopen)(void *);
};

/* Registered component. wake is shared with client tasks; every other field
 * is fixed at registration. */
struct pvr_gpu_component {
    const struct pvr_gpu_component_ops *ops;
    void *self;
    uint32_t flags;      /* PVR_GPU_COMPONENT_COMPUTE */
    uint32_t index;      /* set by registration */
    atomic_uint wake;    /* 1 while a wake message for this component is queued */
};

/* Before pvr_gpu_service_init, at EL1, before the scheduler. Validates the
 * component's storage against the service and the other components and
 * sets its index. Firmware objects of all components and the firmware's own
 * resources must fit PVR_MIPS_MAX_OBJECTS. 1 registered, 0 refused. */
int pvr_gpu_service_register(struct pvr_gpu_service_config *, struct pvr_gpu_component *);

/* Client task, after making work visible to the component (release order).
 * Sends one wake message unless one is already queued for this component;
 * the worker clears wake before it polls the component, so a submit that
 * follows the clear sends again. The request queue holds one message per
 * client, one interrupt wake and one wake per component, so this send
 * cannot find it full. 1 woken or already pending; 0 only if that bound was
 * broken, which the caller treats as a service fault. */
int pvr_gpu_component_wake(struct pvr_gpu_service *, struct pvr_gpu_component *);
#endif
