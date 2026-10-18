/* Same supervised SRAM entry and unchanged592-byte board ABI. The EL3 image
 * authenticates the retained profile before SID or peripheral access. */
#include "a733_board_application.h"
#define a733_smp_launch_prepare bluetooth_supervised_base_prepare
#include "board-launch-base.c"
#undef a733_smp_launch_prepare
int a733_smp_launch_prepare(void)
{
    volatile const struct a733_board_session *s=(const void *)A733_BOARD_SESSION_ADDRESS;
    ADMIT(s->magic==A733_BOARD_SESSION_MAGIC&&s->version==A733_BOARD_SESSION_VERSION&&s->bytes==sizeof(*s)&&
        s->epoch==REQUEST[3]&&s->generation==REQUEST[4]&&s->lease==REQUEST[6]&&s->workload==2&&s->sid_authority,14);
    ADMIT(!s->minimum_reload_us&&!s->maximum_recovery_us&&!s->guard_us&&!s->reset_by_us&&
        !s->rtc_control&&!s->rtc_divider&&!s->rtc_source&&!s->deadline_us&&!s->session_start_ticks&&!s->reload_after_ticks,14);
    uint8_t aggregate=0;for(unsigned i=0;i<32;++i)aggregate|=s->sid_evidence[i];ADMIT(aggregate,14);
    return bluetooth_supervised_base_prepare();
}
