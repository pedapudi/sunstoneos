/* Existing SRAM entry with an authenticated extended memory contract. */
#include "a733_displayport_application.h"
#define a733_smp_launch_prepare displayport_supervised_base_prepare
#include "displayport-launch-base.c"
#undef a733_smp_launch_prepare
int a733_smp_launch_prepare(void)
{
    volatile const struct a733_displayport_session *g=(const void *)A733_BOARD_SESSION_ADDRESS;
    volatile const struct a733_board_session *s=&g->board;
    ADMIT(g->magic==A733_DISPLAYPORT_SESSION_MAGIC&&g->version==1&&g->bytes==sizeof(*g)&&
        s->magic==A733_BOARD_SESSION_MAGIC&&s->version==2&&s->bytes==sizeof(*s)&&
        s->workload==2&&s->epoch==REQUEST[3]&&s->generation==REQUEST[4]&&
        s->lease==REQUEST[6]&&CONTRACT->reserved[1]==s->memory_authority,14);
    ADMIT(a733_displayport_fields_valid(g),14);
    ADMIT(!s->minimum_reload_us&&!s->maximum_recovery_us&&!s->guard_us&&
        !s->reset_by_us&&!s->rtc_control&&!s->rtc_divider&&!s->rtc_source&&
        !s->deadline_us&&!s->session_start_ticks&&!s->reload_after_ticks,14);
    volatile const uint8_t *entry=(const void *)((uintptr_t)CONTRACT+256);
    for(unsigned i=0;i<32;++i)ADMIT(entry[i]==s->qualification[i],14);
    return displayport_supervised_base_prepare();
}
