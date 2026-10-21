/* Existing SRAM entry with an authenticated extended memory contract. */
#include "a733_g2d_application.h"
#define a733_smp_launch_prepare g2d_supervised_base_prepare
#include "g2d-launch-base.c"
#undef a733_smp_launch_prepare
int a733_smp_launch_prepare(void)
{
    volatile const struct a733_g2d_session *g=(const void *)A733_BOARD_SESSION_ADDRESS;
    /* An opaque base register gives each field a base+offset load instead of
     * its own absolute address, which keeps this code within the SRAM region. */
    __asm__("":"+r"(g));
    volatile const struct a733_board_session *s=&g->board;
    ADMIT(g->magic==A733_G2D_SESSION_MAGIC&&g->version==1&&g->bytes==sizeof(*g)&&
        s->magic==A733_BOARD_SESSION_MAGIC&&s->version==2&&s->bytes==sizeof(*s)&&
        s->workload==2&&s->epoch==REQUEST[3]&&s->generation==REQUEST[4]&&
        s->lease==REQUEST[6]&&CONTRACT->reserved[1]==s->memory_authority,14);
    ADMIT(a733_g2d_geometry_valid(g),14);
    ADMIT(!s->minimum_reload_us&&!s->maximum_recovery_us&&!s->guard_us&&
        !s->reset_by_us&&!s->rtc_control&&!s->rtc_divider&&!s->rtc_source&&
        !s->deadline_us&&!s->session_start_ticks&&!s->reload_after_ticks,14);
    volatile const uint8_t *entry=(const void *)((uintptr_t)CONTRACT+256);
    for(unsigned i=0;i<32;++i)ADMIT(entry[i]==s->qualification[i],14);
    return g2d_supervised_base_prepare();
}
