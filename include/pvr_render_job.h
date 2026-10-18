#ifndef SUNSTONE_PVR_RENDER_JOB_H
#define SUNSTONE_PVR_RENDER_JOB_H
#include "pvr_render_programs.h"
#include "pvr_cccb.h"
struct pvr_render_job_image {
    unsigned char geometry[112], fragment[480];
    unsigned char geometry_update[8], fragment_fence[8], fragment_update[8];
};
/* Fresh offline firmware commands for the fixed clear. Geometry first/last,
 * both engines single-core, explicit fragment dependency and real completion
 * UPDATEs. Partial-render reference is geometry sequence zero; final geometry
 * and fragment timelines are sequence one. Native owner proves all addresses. */
int pvr_render_job_build(const struct pvr_render_program_image *,
    uint32_t hwrt, uint32_t geometry_ufo, uint32_t fragment_ufo,
    struct pvr_render_job_image *);
#endif
