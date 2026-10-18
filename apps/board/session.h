#ifndef SUNSTONE_BOARD_BOOT_SESSION_H
#define SUNSTONE_BOARD_BOOT_SESSION_H
#include "a733_board_application.h"
/* Compatibility names used only by the reused boot publication patch. */
#define A733_STORAGE_SESSION_ADDRESS A733_BOARD_SESSION_ADDRESS
/* Primary EL3 seals the already admitted contract's complete session span.
 * The getter exposes only the authenticated board prefix after publication. */
int board_session_prepare(const struct a733_smp_contract *);
const struct a733_board_session *board_session_snapshot(void);
#endif
