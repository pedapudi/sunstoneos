/* Host shim of apps/board/application.h: the providers and session that
 * apps/os/perf.c uses. */
#ifndef SUNSTONE_BOARD_APPLICATION_H
#define SUNSTONE_BOARD_APPLICATION_H
#include "a733_pmic.h"
struct board_shim { struct { uint64_t epoch, generation; } session; };
struct providers_shim { struct { struct a733_counter_clock counter; struct a733_rail_leases rails; struct a733_pmic pmic; } shared; };
extern struct board_shim board;
extern struct providers_shim providers;
#endif
