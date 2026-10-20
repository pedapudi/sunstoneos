#ifndef SUNSTONE_A733_DEINTERLACE_APPLICATION_H
#define SUNSTONE_A733_DEINTERLACE_APPLICATION_H
#include "a733_board_application.h"
#define A733_DEINTERLACE_CONTRACT_MAGIC UINT64_C(0x4153373344494331)
#define A733_DEINTERLACE_SESSION_MAGIC UINT64_C(0x4153373344494131)
#define A733_DEINTERLACE_INPUT_CAPACITY 460800u
#define A733_DEINTERLACE_OUTPUT_CAPACITY 460800u
#define A733_DEINTERLACE_RESULT_BYTES 192u
#define A733_DEINTERLACE_MAX_WIDTH 640u
#define A733_DEINTERLACE_MAX_HEIGHT 480u
#define A733_DEINTERLACE_SESSION_SECONDS 20u
#define A733_DEINTERLACE_FORMAT_NV12 UINT32_C(0x3231564e)
#define A733_DEINTERLACE_WEAVE 1u
/* One immutable, combined-field NV12 frame, top field first. Y and UV lines
 * are interleaved by field in their ordinary full-frame planes. fixture binds
 * all460800 bytes; reference remains zero as in the inherited copied-upload
 * envelope. No oracle, recorded-source permission or reclamation is implied. */
struct a733_deinterlace_session {
    struct a733_board_session board;
    uint64_t magic,version,bytes,owner,authority,pck_client;
    uint8_t initial[32],memory[32],electrical[32],supervision[32];
    uint8_t fixture[32],reference[32];
    uint32_t module,module_hz,iommu_gates[2],supply_uv,supply_rail;
    uint32_t width,height,y_stride,uv_stride,format,mode,top_field_first,reserved;
};
_Static_assert(sizeof(struct a733_deinterlace_session)==888,"DI immutable wire layout");
static inline int a733_deinterlace_geometry_valid(const volatile struct a733_deinterlace_session *s)
{
    return s && s->width==640 && s->height==480 && s->y_stride==640 &&
        s->uv_stride==640 && s->format==A733_DEINTERLACE_FORMAT_NV12 &&
        s->mode==A733_DEINTERLACE_WEAVE && s->top_field_first==1 && !s->reserved;
}
int a733_deinterlace_session_valid(const struct a733_deinterlace_session *);
#endif
