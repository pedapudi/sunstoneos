/* The supervised radio profile bounds of apps/board/radio_profile.c, unchanged, under the name that the OS image's
 * lifetime dispatch calls (apps/os/radio_profile.c). */
#define board_radio_profile_runtime board_radio_profile_supervised
#include "../board/radio_profile.c"
#undef board_radio_profile_runtime
