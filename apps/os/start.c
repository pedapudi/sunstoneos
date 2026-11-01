/* Fixed start list of the integrated OS image. main.c calls the three board
 * application hooks at their existing points: pins before provider
 * construction, start after shared task binding, both before the scheduler.
 * Each hook runs every entry in list order and fails on the first refusal,
 * so main.c keeps its panic codes. Each entry validates its own session. */
#include "os.h"
#include "../board/application.h"
#include "../board/session.h"
#include "application.h"
#include "boot.h"
#include "a733_smp_placement.h"
#include <string.h>
#ifdef OS_RECOGNITION_CORE
/* The GPU build's recognition core comes from cmake/a733-os.cmake. */
_Static_assert(OS_RECOGNITION_CORE==A733_SMP_RECOGNITION_CORE,"recognition core differs from the placement header");
#endif
_Static_assert(BOARD_SERVICE_TYPEC==2&&BOARD_SERVICE_POWER==3&&BOARD_SERVICE_USB2==4,"host service selection IDs");
int board_application_task_privileged(void (*entry)(void *),void *context)
{
    for(unsigned i=0;i<os_application_count;++i)
        if(os_applications[i].task_privileged(entry,context)==1)return 1;
    return 0;
}
int board_application_pins(struct a733_pin_handoff *pins)
{
    for(unsigned i=0;i<os_application_count;++i)if(os_applications[i].pins(pins)!=1)return 0;
    return 1;
}
int board_application_start(void)
{
    for(unsigned i=0;i<os_application_count;++i)if(os_applications[i].start()!=1)return 0;
    return 1;
}
const struct os_session *os_session_admitted(void)
{
    const struct os_session *s=os_session();
    return s&&!memcmp(&s->board,&board.admitted_session,sizeof(s->board))&&os_session_valid(s)&&
        !memcmp(s->board.image,a733_smp_build_id,32)?s:NULL;
}
