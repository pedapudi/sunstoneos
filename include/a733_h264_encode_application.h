#ifndef SUNSTONE_A733_H264_ENCODE_APPLICATION_H
#define SUNSTONE_A733_H264_ENCODE_APPLICATION_H
#include "a733_board_application.h"
#define A733_H264_ENCODE_CONTRACT_MAGIC UINT64_C(0x4153373348454331)
#define A733_H264_ENCODE_SESSION_MAGIC UINT64_C(0x4153373348454131)
#define A733_H264_ENCODE_INPUT_BYTES 921600u
#define A733_H264_ENCODE_OUTPUT_CAPACITY 1048576u
/* Fixed two-picture VGA NV12 Baseline/CAVLC/QP26. input_digest authenticates
 * both tightly packed460800-byte frames in decode order before device use.
 * reserved_reference is zero: normal encoding has no supplied numeric oracle.
 * First592 bytes and resource facts retain the ordinary supervised board ABI. */
struct a733_h264_encode_session {
    struct a733_board_session board;
    uint64_t magic,version,bytes,owner,authority,pck_client;
    uint8_t initial[32],memory[32],electrical[32],supervision[32];
    uint8_t input_digest[32],reserved_reference[32];
    uint32_t module,module_hz,iommu_gates[2],supply_uv,supply_rail;
};
_Static_assert(sizeof(struct a733_h264_encode_session)==856,"H264 encode immutable session");
int a733_h264_encode_session_valid(const struct a733_h264_encode_session *);
#endif
