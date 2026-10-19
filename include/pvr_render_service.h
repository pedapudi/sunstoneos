#ifndef SUNSTONE_PVR_RENDER_SERVICE_H
#define SUNSTONE_PVR_RENDER_SERVICE_H
#include "pvr_render_job.h"
#include "pvr_render_draw.h"
#include <stdatomic.h>
struct a733_gpu_graphics_memory;
struct pvr_gpu_service;
enum pvr_render_service_phase { PVR_RENDER_SERVICE_NEW, PVR_RENDER_SERVICE_BOUND,
    PVR_RENDER_SERVICE_READY, PVR_RENDER_SERVICE_FORMAT_GEOMETRY,
    PVR_RENDER_SERVICE_FORMAT_FRAGMENT, PVR_RENDER_SERVICE_SUBMIT,
    PVR_RENDER_SERVICE_TIMELINES, PVR_RENDER_SERVICE_INVALIDATE,
    PVR_RENDER_SERVICE_OUTPUT, PVR_RENDER_SERVICE_COMPLETE, PVR_RENDER_SERVICE_FAILED };
enum pvr_render_service_error { PVR_RENDER_SERVICE_NO_ERROR, PVR_RENDER_SERVICE_ADMISSION,
    PVR_RENDER_SERVICE_CLOCK, PVR_RENDER_SERVICE_QUEUE, PVR_RENDER_SERVICE_TIMELINE,
    PVR_RENDER_SERVICE_RESULT, PVR_RENDER_SERVICE_MEMORY, PVR_RENDER_SERVICE_RESET };
/* Embedded in the actual native graphics owner. No arbitrary provider binds
 * this object. Every request is serialized by the privileged GPU service.
 * One fixed job per boot; fault or completion retains all native parents. */
struct pvr_render_service {
    struct a733_gpu_graphics_memory *owner;
    struct pvr_cccb geometry, fragment;
    struct pvr_cccb_pair pair;
    struct pvr_render_job_image image;
    struct pvr_render_program_image programs;
    uint64_t epoch, deadline, last_now;
    uint32_t phase, error, checked;
    atomic_uint draw_state; /* 0 free, 1 copying, 2 queued; scoped producer storage. */
    uint64_t draw_ticket,draw_epoch;
    uint32_t draw_client,draw_count,draw_bytes,draw_accepted;
    unsigned char draw_copy[PVR_RENDER_DRAW_BYTES];
    /* Copied output becomes readable only at COMPLETE, after both timelines,
     * coherent reads, every pixel/guard and the final deadline observation. */
    uint32_t pixels[PVR_RENDER_WIDTH*PVR_RENDER_HEIGHT];
};
int pvr_render_service_begin(struct pvr_render_service *, uint64_t deadline);
/* Worker only: validate the complete copied input before touching backing.
 * Return 1 accepted,0 invalid,-1 retained memory/submission failure. */
int pvr_render_service_draw(struct pvr_render_service *,const void *,size_t,uint32_t,uint64_t deadline);
int pvr_render_service_poll(struct pvr_render_service *);
void pvr_render_service_abort(struct pvr_render_service *, unsigned error);
#endif
