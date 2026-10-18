/* Actual supervised SRAM entry, with the fixed large GPU reservation ABI. */
#include "a733_board_application.h"
#define a733_smp_launch_prepare gpu_supervised_base_prepare
#include "gpu-launch-base.c"
#undef a733_smp_launch_prepare
int a733_smp_launch_prepare(void)
{
    volatile const struct a733_board_session *s=(const void *)A733_BOARD_SESSION_ADDRESS;
    ADMIT(s->magic==A733_BOARD_SUPERVISED_SESSION_MAGIC&&s->version==2&&s->bytes==sizeof(*s)&&
        s->epoch==REQUEST[3]&&s->generation==REQUEST[4]&&s->lease==REQUEST[6]&&s->workload==2,14);
    ADMIT(!s->minimum_reload_us&&!s->maximum_recovery_us&&!s->guard_us&&!s->reset_by_us&&
        !s->rtc_control&&!s->rtc_divider&&!s->rtc_source&&!s->deadline_us&&
        !s->session_start_ticks&&!s->reload_after_ticks,14);
    return gpu_supervised_base_prepare();
}
