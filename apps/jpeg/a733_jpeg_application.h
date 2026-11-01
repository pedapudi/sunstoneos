#ifndef SUNSTONE_A733_JPEG_APPLICATION_H
#define SUNSTONE_A733_JPEG_APPLICATION_H
#include "a733_board_application.h"
#define A733_JPEG_CONTRACT_MAGIC UINT64_C(0x415337334a504332)
#define A733_JPEG_SESSION_MAGIC UINT64_C(0x415337334a504132)
#define A733_JPEG_OUTPUT_BYTES 2304u
/* Fixed board-provider selection2 plus authenticated, immutable JPEG authority.
 * These evidence identities must name reviewed physical facts. They are never
 * generated or qualified by application startup or register idle readback. */
struct a733_jpeg_session {
    struct a733_board_session board;
    uint64_t magic,version,bytes,owner,authority,pck_client;
    uint8_t initial[32],memory[32],electrical[32],supervision[32];
    uint8_t fixture[32],reference[32];
    uint32_t module,module_hz;
    uint32_t iommu_gates[2];
    uint32_t supply_uv,supply_rail;
};
_Static_assert(sizeof(struct a733_jpeg_session)==856,"JPEG immutable wire layout");
int a733_jpeg_session_valid(const struct a733_jpeg_session *);
#endif
