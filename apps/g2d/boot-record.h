#ifndef SUNSTONE_G2D_BOOT_RECORD_H
#define SUNSTONE_G2D_BOOT_RECORD_H
#include "boot.h"
struct g2d_boot_contract {
    struct a733_smp_contract base;
    uint8_t session_digest[32],entry_digest[32];
};
_Static_assert(sizeof(struct g2d_boot_contract)==288,"G2D extended boot record");
/* Only the retained EL3 copy supplies authority; reserved[] still binds the
 * existing extended memory reservation. The complete live record must match. */
const struct g2d_boot_contract *g2d_boot_record(const struct a733_smp_contract *);
int g2d_board_prepare(const struct a733_smp_contract *);
#endif
