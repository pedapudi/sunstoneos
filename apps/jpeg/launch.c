#include "a733_jpeg_application.h"
#define a733_smp_launch_prepare jpeg_supervised_base_prepare
#include "jpeg-launch-base.c"
#undef a733_smp_launch_prepare
int a733_smp_launch_prepare(void)
{
    volatile const struct a733_jpeg_session *j=(const void *)A733_BOARD_SESSION_ADDRESS;
    volatile const struct a733_board_session *s=&j->board;
    ADMIT(j->magic==A733_JPEG_SESSION_MAGIC&&j->version==2&&j->bytes==sizeof(*j)&&
        s->magic==A733_BOARD_SESSION_MAGIC&&s->version==2&&s->bytes==sizeof(*s)&&s->workload==2&&
        s->epoch==REQUEST[3]&&s->generation==REQUEST[4]&&s->lease==REQUEST[6],14);
    ADMIT(!s->minimum_reload_us&&!s->maximum_recovery_us&&!s->guard_us&&!s->reset_by_us&&
        !s->rtc_control&&!s->rtc_divider&&!s->rtc_source&&!s->deadline_us&&
        !s->session_start_ticks&&!s->reload_after_ticks,14);
    return jpeg_supervised_base_prepare();
}
