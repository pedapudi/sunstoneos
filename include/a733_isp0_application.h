#ifndef SUNSTONE_A733_ISP0_APPLICATION_H
#define SUNSTONE_A733_ISP0_APPLICATION_H
#include "a733_board_application.h"
#define A733_ISP0_CONTRACT_MAGIC UINT64_C(0x4153373349504331)
#define A733_ISP0_SESSION_MAGIC UINT64_C(0x4153373349504131)
#define A733_ISP0_INPUT_CAPACITY 614400u
#define A733_ISP0_OUTPUT_CAPACITY 614400u
#define A733_ISP0_RESULT_BYTES 192u
#define A733_ISP0_MAX_WIDTH 640u
#define A733_ISP0_MAX_HEIGHT 480u
#define A733_ISP0_SESSION_SECONDS 20u
#define A733_ISP0_FORMAT_BGGR12 12u
/* One recorded BGGR12 input in little-endian 16-bit containers, converted to
 * NV16 with contiguous Y and UV planes. The input digest excludes the 64 zero
 * prefetch bytes retained after it. Evidence binds external physical facts;
 * reference is reserved and zero. */
struct a733_isp0_session {
    struct a733_board_session board;
    uint64_t magic,version,bytes,owner,authority,pck_client;
    uint8_t initial[32],memory[32],electrical[32],supervision[32];
    uint8_t fixture[32],reference[32];
    uint32_t iommu_gates[2],supply_uv,supply_rail;
    uint32_t width,height,input_stride,output_stride,format,reserved[3];
};
_Static_assert(sizeof(struct a733_isp0_session)==880,"ISP0 immutable wire layout");
_Static_assert(offsetof(struct a733_isp0_session,width)==848,"ISP0 geometry offset");
static inline int a733_isp0_geometry_valid(const volatile struct a733_isp0_session *s)
{
    return s&&s->width==640&&s->height==480&&s->input_stride==1280&&
        s->output_stride==640&&s->format==A733_ISP0_FORMAT_BGGR12&&
        !s->reserved[0]&&!s->reserved[1]&&!s->reserved[2];
}
int a733_isp0_session_valid(const struct a733_isp0_session *);
#endif
