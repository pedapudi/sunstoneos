#ifndef SUNSTONE_A733_DEINTERLACE_APPLICATION_H
#define SUNSTONE_A733_DEINTERLACE_APPLICATION_H
#include "a733_board_application.h"
#define A733_DEINTERLACE_CONTRACT_MAGIC UINT64_C(0x4153373344494331)
#define A733_DEINTERLACE_SESSION_MAGIC UINT64_C(0x4153373344494131)
#define A733_DEINTERLACE_FRAME_BYTES 460800u
/* At most three repeated jobs follow the first. The 20-second session must
 * carry five uploaded frames and four copied outputs at USB full speed. */
#define A733_DEINTERLACE_MAX_REPEATS 3u
#define A733_DEINTERLACE_INPUT_CAPACITY (A733_DEINTERLACE_FRAME_BYTES*(2u+A733_DEINTERLACE_MAX_REPEATS))
#define A733_DEINTERLACE_OUTPUT_CAPACITY (A733_DEINTERLACE_FRAME_BYTES*(1u+A733_DEINTERLACE_MAX_REPEATS))
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
#define A733_DEINTERLACE_MD_BYTES 76800u
#define A733_DEINTERLACE_TNR_MD_BYTES 307200u
/* Combined-field NV12 and top field first. Mode1 uploads one frame per job;
 * modes2/3 upload previous+current full frames for the first job and start
 * with zero motion-flag history. Mode2 produces NV12; mode3 produces planar
 * YUV420 with the selected strength. No numerical qualification is implied.
 * strength is zero outside mode3. repeats adds that many jobs; each consumes
 * one more uploaded frame and produces one more output. It occupies the upper
 * half of the former strength word, which earlier sessions kept zero, so they
 * still run one job. fixture binds the entire exact upload; reference remains
 * zero as in the inherited copied-upload envelope. No oracle, recorded-source
 * permission or reclamation is implied. */
struct a733_deinterlace_session {
    struct a733_board_session board;
    uint64_t magic,version,bytes,owner,authority,pck_client;
    uint8_t initial[32],memory[32],electrical[32],supervision[32];
    uint8_t fixture[32],reference[32];
    uint32_t module,module_hz,iommu_gates[2],supply_uv,supply_rail;
    uint32_t width,height,y_stride,uv_stride,format,mode,top_field_first;
    uint16_t strength,repeats;
};
_Static_assert(sizeof(struct a733_deinterlace_session)==888,"DI immutable wire layout");
static inline int a733_deinterlace_geometry_valid(const volatile struct a733_deinterlace_session *s)
{
    return s && s->width==640 && s->height==480 && s->y_stride==640 &&
        s->uv_stride==640 && s->format==A733_DEINTERLACE_FORMAT_NV12 &&
        (s->mode==A733_DEINTERLACE_WEAVE || s->mode==A733_DEINTERLACE_MOTION || s->mode==A733_DEINTERLACE_TNR) && s->top_field_first==1 &&
        s->strength<=A733_DEINTERLACE_STRENGTH_HIGH && (!s->strength || s->mode==A733_DEINTERLACE_TNR) &&
        s->repeats<=A733_DEINTERLACE_MAX_REPEATS;
}
static inline unsigned a733_deinterlace_jobs(const struct a733_deinterlace_session *s)
{return 1u+s->repeats;}
static inline size_t a733_deinterlace_input_bytes(const struct a733_deinterlace_session *s)
{return (size_t)A733_DEINTERLACE_FRAME_BYTES*(s->repeats+(s->mode==A733_DEINTERLACE_WEAVE?1u:2u));}
int a733_deinterlace_session_valid(const struct a733_deinterlace_session *);
#endif
