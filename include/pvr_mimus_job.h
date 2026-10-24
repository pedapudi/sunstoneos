#ifndef SUNSTONE_PVR_MIMUS_JOB_H
#define SUNSTONE_PVR_MIMUS_JOB_H
/* The job and completion record that the recognizer and the GPU service's
 * mimus session exchange (docs/gpu-mimus-service.md). Plain C with no
 * FreeRTOS dependency, so the OS image's dispatcher and its host checks use
 * the same types as the session component. */
#include <stdint.h>

/* One job of a placed call: rows activation rows from x_offset in the arena
 * span, x_stride bytes apart, of tensor `ordinal`. */
struct pvr_mimus_request {
    uint64_t x_offset;
    uint32_t ordinal, rows, x_stride;
};
/* OK after OPEN means the session is ready; after a job it means the
 * timeline reached the job's sequence and the output region is maintained
 * (the tags still need checking). REFUSED: nothing reached the client ring
 * and the caller computes the job's rows. LOST: the GPU or the service
 * failed; nothing in the output region is valid. */
enum pvr_mimus_result { PVR_MIMUS_OK, PVR_MIMUS_REFUSED, PVR_MIMUS_LOST };
struct pvr_mimus_record { uint32_t sequence, result; };
_Static_assert(sizeof(struct pvr_mimus_record) == 8, "completion record");
#endif
