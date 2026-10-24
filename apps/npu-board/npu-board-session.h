#ifndef SUNSTONE_NPU_BOARD_SESSION_H
#define SUNSTONE_NPU_BOARD_SESSION_H
#include "a733_board_application.h"
#define NPU_BOARD_CONTRACT_MAGIC UINT64_C(0x415337334e424331)
#define NPU_BOARD_SESSION_MAGIC UINT64_C(0x415337334e424131)
#define NPU_BOARD_SESSION_VERSION 2u
#define NPU_BOARD_SESSION_SECONDS 20u
#define NPU_BOARD_WORKLOAD 2u /* The board's USB0 and provider selection, no host or radio. */
/* The board session page extended with the NPU record. The board prefix
 * carries the epoch, generation, lease, fabric, PMIC and USB0 fields. The
 * evidence digests bind reviewed records of the admission (platform, DDR
 * and extended-memory), clock, power and supervision conditions listed in
 * docs/designs/npu-board.md; they establish no physical fact themselves. */
struct npu_board_session {
    struct a733_board_session board;
    uint64_t magic,version,bytes;
    uint64_t owner,command_admission,supervisor;
    uint32_t pck_client,reserved;
    uint8_t catalogue[32];
    uint8_t admission[32],clock[32],power[32],supervision[32];
    /* Same thermal payload as apps/thermal/thermal_session.h, without its
     * duplicate board prefix and magic/version/bytes. */
    uint64_t thermal_owner,thermal_authority;
    uint8_t thermal_initial[32],thermal_electrical[32],thermal_clocks[32],thermal_supervision[32];
    uint32_t apb1,apb1_hz;
    uint8_t configure_adc_clock,alarm_mask,critical_mask,reserved0;
    uint16_t limits[5][3];
    uint8_t thermal_reserved[6];
};
_Static_assert(sizeof(struct npu_board_session)==1000,"NPU board session wire layout");
/* SHA-256 over the 32-byte model digests of the linked catalogue, in order. */
extern const uint8_t npu_board_catalogue_digest[32];
int npu_board_session_valid(const struct npu_board_session *);
#endif
