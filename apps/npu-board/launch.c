/* Existing SRAM entry with an authenticated extended memory contract, for the
 * NPU board session (apps/npu-board/npu-board-session.h). */
#include "npu-board-session.h"
#define a733_smp_launch_prepare npu_board_supervised_base_prepare
#include "npu-board-launch-base.c"
#undef a733_smp_launch_prepare
int a733_smp_launch_prepare(void)
{
    volatile const struct npu_board_session *n=(const void *)A733_BOARD_SESSION_ADDRESS;
    /* An opaque base register gives each field a base+offset load instead of
     * its own absolute address, which keeps this code within the SRAM region. */
    __asm__("":"+r"(n));
    volatile const struct a733_board_session *s=&n->board;
    ADMIT(n->magic==NPU_BOARD_SESSION_MAGIC&&n->version==NPU_BOARD_SESSION_VERSION&&n->bytes==sizeof(*n)&&
        s->magic==A733_BOARD_SESSION_MAGIC&&s->version==2&&s->bytes==sizeof(*s)&&
        s->workload==NPU_BOARD_WORKLOAD&&s->epoch==REQUEST[3]&&s->generation==REQUEST[4]&&
        s->lease==REQUEST[6]&&CONTRACT->reserved[1]==s->memory_authority,14);
    ADMIT(n->owner&&n->command_admission&&n->supervisor&&!n->reserved&&n->pck_client<16&&
        n->pck_client!=s->pck_client&&n->owner!=s->pmic_owner&&n->owner!=s->usb0_owner,14);
    ADMIT(!s->minimum_reload_us&&!s->maximum_recovery_us&&!s->guard_us&&
        !s->reset_by_us&&!s->rtc_control&&!s->rtc_divider&&!s->rtc_source&&
        !s->deadline_us&&!s->session_start_ticks&&!s->reload_after_ticks,14);
    volatile const uint8_t *entry=(const void *)((uintptr_t)CONTRACT+256);
    for(unsigned i=0;i<32;++i)ADMIT(entry[i]==s->qualification[i],14);
    return npu_board_supervised_base_prepare();
}
