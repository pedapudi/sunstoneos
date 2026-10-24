#ifndef SUNSTONE_NPU_BOARD_BOOT_RECORD_H
#define SUNSTONE_NPU_BOARD_BOOT_RECORD_H
#include "boot.h"
/* The 224-byte boot contract followed by the session digest and the board
 * qualification digest. reserved[] binds the extended memory reservation
 * (docs/device-memory-capacity.md). Only the retained EL3 copy supplies
 * authority; the complete live record must match it. */
struct npu_board_boot_contract {
    struct a733_smp_contract base;
    uint8_t session_digest[32],entry_digest[32];
};
_Static_assert(sizeof(struct npu_board_boot_contract)==288,"NPU board extended boot record");
const struct npu_board_boot_contract *npu_board_boot_record(const struct a733_smp_contract *);
int npu_board_prepare(const struct a733_smp_contract *);
#endif
