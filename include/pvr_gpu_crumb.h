#ifndef SUNSTONE_PVR_GPU_CRUMB_H
#define SUNSTONE_PVR_GPU_CRUMB_H
/* Breadcrumbs of the GPU service, its controller, its MMU-cache command and
 * the A733 backend, so that a run which fails or never returns names the
 * step it reached.
 *
 * The progress-record header sunstone_crumb.h supplies sunstone_crumb and,
 * since the first-failure record, sunstone_crumb_fail and
 * SUNSTONE_CRUMB_HERE. Without the header, or in an image without
 * SUNSTONE_CRUMBS, every call compiles to nothing.
 *
 * Progress stages go to the LAST word. They are in the GPU group (0x04),
 * after those of the header (0x0401 to 0x0440) and of the OS caller
 * (0x0450 to 0x0453); the record keeps 16 bits of each value.
 *
 * The first failure of any of these units also fills the FAULT, ESR, ELR
 * and FAR words through the first-failure SVC. The EL1 vector keeps the
 * first record, so the innermost unit, which fails first, names the step:
 *   ESR  PVR_GPU_CRUMB_FAIL_GROUP << 12 | source line (bits 11:0)
 *        | error code << 16 (bits 27:16) | unit << 28 (bits 30:28)
 *   ELR  the failing statement
 *   FAR  the unit's detail word, described with each unit below
 * The header names groups 1 to 6; the GPU group is 7. A decoder that reads
 * only the header reports it as an unknown group with the same fields. */
#include <stdint.h>
#if defined(__has_include)
#if __has_include("sunstone_crumb.h")
#include "sunstone_crumb.h"
#endif
#endif
#ifndef SUNSTONE_CRUMB_DEFINED
#define SUNSTONE_CRUMB_DEFINED 1
static inline void sunstone_crumb(uint32_t stage, uint32_t value) { (void)stage; (void)value; }
#endif
#ifdef SUNSTONE_CRUMB_FAIL_SVC
#define PVR_GPU_CRUMB_HERE() SUNSTONE_CRUMB_HERE()
#define pvr_gpu_crumb_record sunstone_crumb_fail
#else
#define PVR_GPU_CRUMB_HERE() ((const void *)0)
static inline void pvr_gpu_crumb_record(uint32_t site, uint32_t value, const void *caller)
{ (void)site; (void)value; (void)caller; }
#endif

/* Progress stages; the value is given with each. */
#define PVR_GPU_CRUMB_STARTED 0x0460u      /* firmware started word 1; value: start steps */
#define PVR_GPU_CRUMB_COMPATIBLE 0x0461u   /* compatibility record accepted */
#define PVR_GPU_CRUMB_HEALTH_SENT 0x0462u  /* health command submitted; value: executed count before */
#define PVR_GPU_CRUMB_EXECUTED 0x0463u     /* health command counted executed; value: count after */
#define PVR_GPU_CRUMB_RETURN 0x0464u       /* health command settled; value: return word (0: none) */
#define PVR_GPU_CRUMB_MMU_SENT 0x0465u     /* MMU-cache command submitted; value: executed count before */
#define PVR_GPU_CRUMB_MMU_FLUSHED 0x0466u  /* MMU-cache command executed; value: return word */
#define PVR_GPU_CRUMB_SECURE 0x0467u       /* secure configuration read back; value: SMC0 << 8 | drm_ctrl */
#define PVR_GPU_CRUMB_TIMER 0x0468u        /* first timer correlation published; value: CR_TIMER reads */
#define PVR_GPU_CRUMB_FAILED 0x046fu       /* first failure of a unit; value: unit << 12 | source line */

#define PVR_GPU_CRUMB_FAIL_GROUP 0x7u
/* Units, in ESR bits 30:28. FAR detail by unit:
 *   SERVICE    service phase << 24 | health steps << 16 | start step index
 *   CONTROL    phase at failure << 24 | health steps << 16
 *              | executed-count change << 8 | health return word (low 8 bits)
 *   FLUSH      phase at failure << 24 | executed-count change << 8
 *              | MMU-cache return word (low 8 bits)
 *   BACKEND    backend phase << 24 | identity step << 16 | service phase << 8
 *              | controller phase */
enum pvr_gpu_crumb_unit { PVR_GPU_CRUMB_SERVICE = 1, PVR_GPU_CRUMB_CONTROL = 2,
    PVR_GPU_CRUMB_FLUSH = 3, PVR_GPU_CRUMB_BACKEND = 4 };
/* Health steps, in FAR bits 23:16 of the SERVICE and CONTROL details. */
enum { PVR_GPU_CRUMB_STEP_STARTED = 1, PVR_GPU_CRUMB_STEP_COMPATIBLE = 2,
    PVR_GPU_CRUMB_STEP_HEALTH_SENT = 4, PVR_GPU_CRUMB_STEP_EXECUTED = 8,
    PVR_GPU_CRUMB_STEP_RETURN = 16, PVR_GPU_CRUMB_STEP_MMU_FLUSHED = 32 };
static inline uint32_t pvr_gpu_crumb_site(unsigned unit, unsigned error, unsigned line)
{
    return PVR_GPU_CRUMB_FAIL_GROUP << 12 | (line & 0xfffu) | (error & 0xfffu) << 16 | (unit & 7u) << 28;
}
static inline void pvr_gpu_crumb_fail(unsigned unit, unsigned error, unsigned line,
                                      uint32_t detail, const void *here)
{
    sunstone_crumb(PVR_GPU_CRUMB_FAILED, (unit & 0xfu) << 12 | (line & 0xfffu));
    pvr_gpu_crumb_record(pvr_gpu_crumb_site(unit, error, line), detail, here);
}

#endif
