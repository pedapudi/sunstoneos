/* Reuse the supervised SRAM launcher with the existing extended-memory shape. */
#include "a733_board_application.h"
#define a733_smp_launch_prepare dsi_supervised_base_prepare
#include "dsi-launch-base.c"
#undef a733_smp_launch_prepare
int a733_smp_launch_prepare(void)
{
 volatile const struct a733_board_session *s=(const void *)A733_BOARD_SESSION_ADDRESS;
 volatile const uint64_t *profile=(const void *)(A733_BOARD_SESSION_ADDRESS+592);
 ADMIT(profile[0]==UINT64_C(0x3153494433375341)&&profile[1]==1&&profile[2]==4096&&
 s->magic==A733_BOARD_SUPERVISED_SESSION_MAGIC&&s->version==2&&s->bytes==sizeof(*s)&&s->workload==2&&
 s->epoch==REQUEST[3]&&s->generation==REQUEST[4]&&s->lease==REQUEST[6],14);
 ADMIT(!s->minimum_reload_us&&!s->maximum_recovery_us&&!s->guard_us&&!s->reset_by_us&&
 !s->rtc_control&&!s->rtc_divider&&!s->rtc_source&&!s->deadline_us&&!s->session_start_ticks&&!s->reload_after_ticks,14);
 return dsi_supervised_base_prepare();
}
