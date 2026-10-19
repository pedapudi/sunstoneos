#ifndef SUNSTONE_PVR_RENDER_DRAW_H
#define SUNSTONE_PVR_RENDER_DRAW_H
#include "pvr_render_programs.h"
#define PVR_RENDER_DRAW_PROFILE 1u
#define PVR_RENDER_DRAW_MAX_FRAMES (UINT32_MAX/2u)
#define PVR_RENDER_DRAW_VERTICES 126u
#define PVR_RENDER_DRAW_BYTES (PVR_RENDER_DRAW_VERTICES*32u)
#define PVR_RENDER_DRAW_ADDRESS (PVR_RENDER_GENERAL_ADDRESS+UINT64_C(0x60000))
#define PVR_RENDER_DRAW_COUNT_OFFSET 0xf020u
#define PVR_RENDER_DRAW_SCISSOR (PVR_RENDER_GENERAL_ADDRESS+UINT64_C(0x10060))
/* IEEE binary32 clip coordinates and RGBA. One nonindexed triangle list;
 * x/y in [-1,1], z=0, w=1 and colors in [0,1]. Values must be finite.
 * Source bytes are copied before acceptance; caller storage may then change. */
struct pvr_render_vertex { float position[4], color[4]; };
int pvr_render_draw_vertices_valid(const void *,size_t bytes,uint32_t count);
/* Apply the pinned compiler/PDS/PPP programs to an already constructed fixed
 * render payload. Output poisoning, context state and background stay GPU jobs. */
int pvr_render_draw_programs_build(void *,size_t bytes);
#endif
