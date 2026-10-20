/* Existing SRAM entry with an authenticated extended memory contract. */
#include "a733_deinterlace_application.h"
#define a733_smp_launch_prepare deinterlace_supervised_base_prepare
#include "deinterlace-launch-base.c"
#undef a733_smp_launch_prepare
int a733_smp_launch_prepare(void)
{
    volatile const struct a733_deinterlace_session *g=(const void *)A733_BOARD_SESSION_ADDRESS;
    volatile const struct a733_board_session *s=&g->board;
    ADMIT(g->magic==A733_DEINTERLACE_SESSION_MAGIC&&g->version==1&&g->bytes==sizeof(*g)&&
        s->magic==A733_BOARD_SESSION_MAGIC&&s->version==2&&s->bytes==sizeof(*s)&&
        s->workload==2&&s->epoch==REQUEST[3]&&s->generation==REQUEST[4]&&
        s->lease==REQUEST[6]&&CONTRACT->reserved[1]==s->memory_authority,14);
    ADMIT(a733_deinterlace_geometry_valid(g),14);
    /* The supervised timing interval is all zero, including the reserved
     * word rejected by native board admission. Byte access avoids aliasing. */
    volatile const uint8_t *timing=(const volatile void *)&s->minimum_reload_us;
    for(unsigned i=0;i<offsetof(struct a733_board_session,topology)-
            offsetof(struct a733_board_session,minimum_reload_us);++i)
        ADMIT(!timing[i],14);
    volatile const uint8_t *entry=(const void *)((uintptr_t)CONTRACT+256);
    for(unsigned i=0;i<32;++i)ADMIT(entry[i]==s->qualification[i],14);
    return deinterlace_supervised_base_prepare();
}
