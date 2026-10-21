#ifndef SUNSTONE_A733_DEINTERLACE_APPLICATION_H
#define SUNSTONE_A733_DEINTERLACE_APPLICATION_H
#include "a733_board_application.h"
#define A733_DEINTERLACE_CONTRACT_MAGIC UINT64_C(0x4153373344494331)
#define A733_DEINTERLACE_SESSION_MAGIC UINT64_C(0x4153373344494131)
#define A733_DEINTERLACE_INPUT_CAPACITY 921600u
#define A733_DEINTERLACE_OUTPUT_CAPACITY 460800u
#define A733_DEINTERLACE_RESULT_BYTES 192u
#define A733_DEINTERLACE_MAX_WIDTH 640u
#define A733_DEINTERLACE_MAX_HEIGHT 480u
#define A733_DEINTERLACE_SESSION_SECONDS 20u
#define A733_DEINTERLACE_FORMAT_NV12 UINT32_C(0x3231564e)
#define A733_DEINTERLACE_WEAVE 1u
#define A733_DEINTERLACE_MOTION 2u
#define A733_DEINTERLACE_TNR 3u
/* TNR strength. Zero keeps the source's fixed strength 0x3a. The others take
 * the pinned adaptive initial gain for LOW 38, MIDDLE 48 or HIGH 62. */
#define A733_DEINTERLACE_STRENGTH_FIXED 0u
#define A733_DEINTERLACE_STRENGTH_LOW 1u
#define A733_DEINTERLACE_STRENGTH_MIDDLE 2u
#define A733_DEINTERLACE_STRENGTH_HIGH 3u
#define A733_DEINTERLACE_FRAME_BYTES 460800u
#define A733_DEINTERLACE_MD_BYTES 76800u
#define A733_DEINTERLACE_TNR_MD_BYTES 307200u
/* One immutable job, combined-field NV12 and top field first. Mode1 uploads
 * one frame; modes2/3 upload previous+current full frames (921600 bytes) and
 * start with zero motion-flag history. Mode2 produces NV12; mode3 produces
 * planar YUV420 with the selected strength; no gain feedback follows the one
 * operation. No steady-state history or numerical qualification is implied.
 * strength is zero outside mode3. fixture binds
 * the entire exact mode-dependent upload; reference remains zero as in the inherited copied-upload
 * envelope. No oracle, recorded-source permission or reclamation is implied. */
struct a733_deinterlace_session {
    struct a733_board_session board;
    uint64_t magic,version,bytes,owner,authority,pck_client;
    uint8_t initial[32],memory[32],electrical[32],supervision[32];
    uint8_t fixture[32],reference[32];
    uint32_t module,module_hz,iommu_gates[2],supply_uv,supply_rail;
    uint32_t width,height,y_stride,uv_stride,format,mode,top_field_first,strength;
};
_Static_assert(sizeof(struct a733_deinterlace_session)==888,"DI immutable wire layout");
static inline int a733_deinterlace_geometry_valid(const volatile struct a733_deinterlace_session *s)
{
    return s && s->width==640 && s->height==480 && s->y_stride==640 &&
        s->uv_stride==640 && s->format==A733_DEINTERLACE_FORMAT_NV12 &&
        (s->mode==A733_DEINTERLACE_WEAVE || s->mode==A733_DEINTERLACE_MOTION || s->mode==A733_DEINTERLACE_TNR) && s->top_field_first==1 &&
        s->strength<=A733_DEINTERLACE_STRENGTH_HIGH && (!s->strength || s->mode==A733_DEINTERLACE_TNR);
}
static inline size_t a733_deinterlace_input_bytes(const struct a733_deinterlace_session *s)
{return s->mode==A733_DEINTERLACE_WEAVE?460800u:921600u;}
int a733_deinterlace_session_valid(const struct a733_deinterlace_session *);
#endif
