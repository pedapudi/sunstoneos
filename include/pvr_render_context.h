#ifndef SUNSTONE_PVR_RENDER_CONTEXT_H
#define SUNSTONE_PVR_RENDER_CONTEXT_H
#include "pvr_compute_context.h"
#define PVR_RENDER_CONTEXT_BYTES 680u
#define PVR_RENDER_GEOM_STATE_BYTES 160u
#define PVR_RENDER_FRAG_STATE_BYTES 32u
#define PVR_RENDER_SWITCH_WORDS 46u
/* One render context contains geometry at offset zero and fragment at 152.
 * Context state allocation includes all four hardware slots, even in the
 * admitted single-core execution mode. VM and both rings remain untouched. */
enum pvr_render_context_region { PVR_RENDER_CONTEXT, PVR_RENDER_GEOM_STATE,
    PVR_RENDER_FRAG_STATE, PVR_RENDER_GEOM_CONTROL, PVR_RENDER_GEOM_RING,
    PVR_RENDER_FRAG_CONTROL, PVR_RENDER_FRAG_RING, PVR_RENDER_VM,
    PVR_RENDER_CONTEXT_REGIONS };
struct pvr_render_context_config {
    uint64_t bvnc, callstack, switch_registers[PVR_RENDER_SWITCH_WORDS];
    uint32_t priority, context_id, owner_id;
};
/* Offline construction only. Exact fresh native backing, program addresses,
 * MMU permissions and private ownership must already be admitted by the native
 * graphics owner. Byte-layout validation cannot confer those authorities.
 * Inputs/output buffers must remain disjoint and stable through construction.
 * Refusal preserves output. Keep every context/program/ring allocation pinned
 * until independently verified device isolation and firmware cleanup. */
int pvr_render_context_build(const struct pvr_render_context_config *,
    const struct pvr_compute_buffer[PVR_RENDER_CONTEXT_REGIONS]);
#endif
