#ifndef SUNSTONE_A733_JPEG_ENCODE_APPLICATION_H
#define SUNSTONE_A733_JPEG_ENCODE_APPLICATION_H
#include "a733_board_application.h"
#define A733_JPEG_ENCODE_CONTRACT_MAGIC UINT64_C(0x415337334a454331)
#define A733_JPEG_ENCODE_SESSION_MAGIC UINT64_C(0x415337334a454131)
#define A733_JPEG_ENCODE_INPUT_BYTES 460800u
#define A733_JPEG_ENCODE_OUTPUT_BYTES 1048576u
/* One retained encoder job: VGA NV12, stride640, quality75. The fixture
 * field authenticates exactly460800 input bytes; reference must be zero.
 * The first592 bytes retain ordinary supervised board-provider authority.
 * These evidence identities must name reviewed physical facts. They are never
 * generated or qualified by application startup or register idle readback. */
struct a733_jpeg_encode_session {
    struct a733_board_session board;
    uint64_t magic,version,bytes,owner,authority,pck_client;
    uint8_t initial[32],memory[32],electrical[32],supervision[32];
    uint8_t fixture[32],reference[32];
    uint32_t module,module_hz;
    uint32_t iommu_gates[2];
    uint32_t supply_uv,supply_rail;
};
_Static_assert(sizeof(struct a733_jpeg_encode_session)==856,"JPEG immutable wire layout");
int a733_jpeg_encode_session_valid(const struct a733_jpeg_encode_session *);
#endif
