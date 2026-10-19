/* Existing SRAM entry and supervised handoff with the fixed extended record. */
#include "a733_h264_application.h"
#define a733_smp_launch_prepare h264_supervised_base_prepare
#include "h264-launch-base.c"
#undef a733_smp_launch_prepare
int a733_smp_launch_prepare(void)
{
    volatile const struct a733_h264_session *j=(const void *)A733_BOARD_SESSION_ADDRESS;
    volatile const struct a733_board_session *s=&j->board;
    ADMIT(j->magic==A733_H264_SESSION_MAGIC&&j->version==3&&j->bytes==sizeof(*j)&&
        s->magic==A733_BOARD_SESSION_MAGIC&&s->version==2&&s->bytes==sizeof(*s)&&s->workload==2&&
        s->epoch==REQUEST[3]&&s->generation==REQUEST[4]&&s->lease==REQUEST[6]&&
        CONTRACT->reserved[1]==s->memory_authority,14);
    ADMIT(!s->minimum_reload_us&&!s->maximum_recovery_us&&!s->guard_us&&!s->reset_by_us&&
        !s->rtc_control&&!s->rtc_divider&&!s->rtc_source&&!s->deadline_us&&
        !s->session_start_ticks&&!s->reload_after_ticks,14);
    volatile const uint8_t *entry=(const void *)((uintptr_t)CONTRACT+256);
    for(unsigned i=0;i<32;++i)ADMIT(entry[i]==s->qualification[i],14);
    return h264_supervised_base_prepare();
}
