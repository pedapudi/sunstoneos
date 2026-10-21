/* Fixed start list of the integrated OS image. main.c calls the three board
 * application hooks at their existing points: pins before provider
 * construction, start after shared task binding, both before the scheduler.
 * Each hook runs every entry in list order and fails on the first refusal,
 * so main.c keeps its panic codes. Each entry validates its own session. */
#include "os.h"
#include "../board/application.h"
#include "../board/session.h"
#include "../mimus-board/application.h"
_Static_assert(OS_SERVICE_MIMUS==BOARD_SERVICES,"recognizer service follows the board services");
struct os_application {
    const char *name;
    int (*task_privileged)(void (*)(void *),void *);
    int (*pins)(struct a733_pin_handoff *);
    int (*start)(void);
};
/* Retained so checks/os-image-link.py can read the list from the image even
 * when the compiler unrolls the loops below into direct calls. */
__attribute__((used,retain)) const struct os_application os_applications[]={
    {"mimus",mimus_board_task_privileged,mimus_board_pins,mimus_board_application_start},
    {"thermal",os_thermal_task_privileged,os_thermal_pins,os_thermal_start},
};
#define OS_APPLICATIONS (sizeof(os_applications)/sizeof(os_applications[0]))
int board_application_task_privileged(void (*entry)(void *),void *context)
{
    for(unsigned i=0;i<OS_APPLICATIONS;++i)
        if(os_applications[i].task_privileged(entry,context)==1)return 1;
    return 0;
}
int board_application_pins(struct a733_pin_handoff *pins)
{
    for(unsigned i=0;i<OS_APPLICATIONS;++i)if(os_applications[i].pins(pins)!=1)return 0;
    return 1;
}
int board_application_start(void)
{
    for(unsigned i=0;i<OS_APPLICATIONS;++i)if(os_applications[i].start()!=1)return 0;
    return 1;
}
/* The OS contract admission accepted only a sealed span of exactly
 * sizeof(struct os_session) bytes (admission.c). */
const struct os_session *os_session(void){return board_session_sealed();}
