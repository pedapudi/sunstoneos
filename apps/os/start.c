/* Fixed start list of the integrated OS image. main.c calls the three board
 * application hooks at their existing points: pins before provider
 * construction, start after shared task binding, both before the scheduler.
 * Each hook runs every entry in list order and fails on the first refusal,
 * so main.c keeps its panic codes. Each entry validates its own session. */
#include "os.h"
#include "cpu-return.h"
#include "../board/application.h"
#include "../board/session.h"
#include "application.h"
#include "boot.h"
#include "a733_smp_placement.h"
#include "sunstone_crumb.h"
#include <string.h>
#ifdef OS_RECOGNITION_CORE
/* The GPU build's recognition core comes from cmake/a733-os.cmake. */
_Static_assert(OS_RECOGNITION_CORE==A733_SMP_APPLICATION_CORE,"recognition core differs from the placement header");
#endif
_Static_assert(BOARD_SERVICE_USB0==1&&BOARD_SERVICE_TYPEC==2&&BOARD_SERVICE_POWER==3&&BOARD_SERVICE_USB2==4,
    "USB service selection IDs");
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
/* A refusal records its hook before main.c panics: the value is the hook's
 * list index (0xffff for the application preparation) and the caller is the
 * hook's own address, which the first-failure decoder resolves to its name. */
int board_application_start(void)
{
    if(!os_application_prepare()){
        board_fail_note(SUNSTONE_CRUMB_FAIL_SITE(BOARD),0xffff,(const void *)os_application_prepare);return 0;}
    for(unsigned i=0;i<os_application_count;++i)
        if(os_applications[i].start()!=1){
            board_fail_note(SUNSTONE_CRUMB_FAIL_SITE(BOARD),i,(const void *)os_applications[i].start);return 0;}
    /* The returning lifetime's per-core terminal tasks (apps/os/cpu-return.c). */
    if(os_returning()&&!os_cpu_return_start()){
        board_fail_note(SUNSTONE_CRUMB_FAIL_SITE(BOARD),0xfffe,(const void *)os_cpu_return_start);return 0;}
    return 1;
}
/* The admitted session, validated in full: the sealed copy, the board prefix the EL3 contract bound, the service
 * records and the build identity. os_session_admitted (apps/os/grants.c) calls this until it succeeds and then
 * keeps the answer: the session is sealed for the boot. */
const struct os_session *os_session_validated(void)
{
    const struct os_session *s=os_session();
    return s&&!memcmp(&s->board,&board.admitted_session,sizeof(s->board))&&os_session_valid(s)&&
        !memcmp(s->board.image,a733_smp_build_id,32)?s:NULL;
}
