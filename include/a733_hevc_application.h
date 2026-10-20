#ifndef SUNSTONE_A733_HEVC_APPLICATION_H
#define SUNSTONE_A733_HEVC_APPLICATION_H
#include "a733_board_application.h"
#define A733_HEVC_CONTRACT_MAGIC UINT64_C(0x4153373348354331)
#define A733_HEVC_SESSION_MAGIC UINT64_C(0x4153373348354131)
#define A733_HEVC_INPUT_CAPACITY 524288u
#define A733_HEVC_PICTURE_INPUT_CAPACITY 262144u
#define A733_HEVC_VISIBLE_BYTES 460800u
#define A733_HEVC_PICTURES 2u
struct a733_hevc_input_range { uint64_t offset,bytes; };
/* One immutable AnnexB clip: progressive Main8/420 VGA, IDR then one P.
 * The two contiguous ranges and complete nonzero input digest authenticate the
 * upload before device effects. Normal output is copied NV12; independent
 * numerical reference pixels belong to host qualification, outside this ABI.
 * The first592 bytes retain the supervised board provider contract. */
struct a733_hevc_session {
    struct a733_board_session board;
    uint64_t magic,version,bytes,owner,authority,pck_client;
    uint8_t initial[32],memory[32],electrical[32],supervision[32];
    uint8_t input_digest[32];
    struct a733_hevc_input_range pictures[A733_HEVC_PICTURES];
    uint32_t module,module_hz,iommu_gates[2],supply_uv,supply_rail;
};
_Static_assert(sizeof(struct a733_hevc_session)==856,"HEVC immutable session");
int a733_hevc_session_valid(const struct a733_hevc_session *);
#endif
