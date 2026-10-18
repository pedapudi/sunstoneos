#ifndef SUNSTONE_PVR_RENDER_PROGRAMS_H
#define SUNSTONE_PVR_RENDER_PROGRAMS_H
#include "pvr_render_context.h"
/* Fixed first graphics workload: one 64x64 single-sample RGBA8 clear. General
 * graphics, other formats/resolutions and additional shaders remain required. */
#define PVR_RENDER_WIDTH 64u
#define PVR_RENDER_HEIGHT 64u
#define PVR_RENDER_PIXEL UINT32_C(0xffc08020)
#define PVR_RENDER_POISON UINT32_C(0x73951a6e)
#define PVR_RENDER_GUARD UINT32_C(0x36da97c5)
#define PVR_RENDER_PDS_BASE UINT64_C(0xda00000000)
#define PVR_RENDER_USC_BASE UINT64_C(0xe000000000)
#define PVR_RENDER_PDS_ADDRESS PVR_RENDER_PDS_BASE
#define PVR_RENDER_USC_ADDRESS PVR_RENDER_USC_BASE
#define PVR_RENDER_STATIC_GENERAL_ADDRESS UINT64_C(0x8000000000)
#define PVR_RENDER_GENERAL_ADDRESS UINT64_C(0x8008000000)
#define PVR_RENDER_GENERAL_BYTES UINT32_C(0x56000)
#define PVR_RENDER_PROGRAM_BYTES (8192u + PVR_RENDER_GENERAL_BYTES)
#define PVR_RENDER_OUTPUT_OFFSET UINT32_C(0x11000)
#define PVR_RENDER_OUTPUT_BYTES (64u + PVR_RENDER_WIDTH * PVR_RENDER_HEIGHT * 4u + 64u)
#define PVR_RENDER_OUTPUT_ADDRESS (PVR_RENDER_GENERAL_ADDRESS + PVR_RENDER_OUTPUT_OFFSET + 64u)
#define PVR_RENDER_BORDER_ADDRESS (PVR_RENDER_GENERAL_ADDRESS + UINT64_C(0x16000))
#define PVR_RENDER_VDM_ADDRESS (PVR_RENDER_GENERAL_ADDRESS + UINT64_C(0xf000))
#define PVR_RENDER_CALLSTACK (PVR_RENDER_GENERAL_ADDRESS + UINT64_C(0xe000))
struct pvr_render_program_image {
    uint64_t switch_registers[PVR_RENDER_SWITCH_WORDS];
    uint64_t pbe[3], background[3];
    uint32_t event_data, event_info, pixel_output, event_code;
};
/* Actual native parent owns contiguous PDS/USC/general spans in this order.
 * Fixed addresses above must be mapped before publication. The output starts
 * poisoned with guards; the constructor never writes the expected pixels.
 * Native owner admission must exclude aliases and retain every referenced
 * byte. This bounded offline producer performs no callbacks or device I/O. */
int pvr_render_programs_build(void *private_payload, size_t bytes,
    struct pvr_render_program_image *);
#endif
