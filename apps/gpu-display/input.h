#ifndef SUNSTONE_GPU_DISPLAY_INPUT_H
#define SUNSTONE_GPU_DISPLAY_INPUT_H
#include "pvr_render_texture.h"
#include <stddef.h>
#define GPU_DISPLAY_INPUT_MAGIC UINT64_C(0x4153373347444931)
#define GPU_DISPLAY_INPUT_VERSION 1u
#define GPU_DISPLAY_ENVELOPE_LIMIT UINT32_C(1048576)
#define GPU_DISPLAY_WIDTH 1280u
#define GPU_DISPLAY_HEIGHT 720u
struct gpu_display_input_frame {
    struct pvr_render_texture_input input;
    uint32_t offset,bytes,reserved;
    uint8_t digest[32];
};
/* Pointer-free application data within the root-owned authenticated envelope.
 * bytes covers this header and both canonical payloads. Offsets are relative to
 * this header, not CPU/device addresses. Payloads must be contiguous, in frame
 * order, and cover bytes exactly. Root retains the full envelope before BSS.
 * Digests bind bytes; authority, isolation and physical qualification are
 * independent root-owned admission requirements. */
struct gpu_display_inputs {
    uint64_t magic;
    uint32_t version,bytes;
    struct gpu_display_input_frame frame[2];
};
_Static_assert(sizeof(struct pvr_render_texture_input)==20,"texture input ABI");
_Static_assert(sizeof(struct gpu_display_input_frame)==64,"display frame ABI");
_Static_assert(sizeof(struct gpu_display_inputs)==144,"display data ABI");
_Static_assert(offsetof(struct gpu_display_inputs,frame)==16,"display frame offset");
/* Original retained input, valid through GPU copying and display shutdown.
 * Root authenticates the containing envelope, selected profile and providers.
 * A NULL result refuses preparation. No standalone implementation is supplied. */
const struct gpu_display_inputs *gpu_display_inputs_get(void);
/* Validate root-retained bounded data again before any runtime service copy. */
int gpu_display_inputs_valid(const struct gpu_display_inputs *);
#endif
