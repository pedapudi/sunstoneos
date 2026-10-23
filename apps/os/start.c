/* Fixed start list of the integrated OS image. main.c calls the three board
 * application hooks at their existing points: pins before provider
 * construction, start after shared task binding, both before the scheduler.
 * Each hook runs every entry in list order and fails on the first refusal,
 * so main.c keeps its panic codes. Each entry validates its own session. */
#include "os.h"
#include "../board/application.h"
#include "../board/session.h"
#include "../mimus-board/application.h"
#include "boot.h"
#include <string.h>
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
#ifdef SUNSTONE_A733_OS_AUDIO
    {"audio",os_audio_task_privileged,os_audio_pins,os_audio_start},
#endif
#ifdef SUNSTONE_A733_OS_RADIO
    {"radio",os_radio_task_privileged,os_radio_pins,os_radio_start},
#endif
#ifdef SUNSTONE_A733_OS_HEADSET
    {"headset",os_headset_task_privileged,os_headset_pins,os_headset_start},
#endif
#ifdef SUNSTONE_A733_OS_GPU_LINEAR
    {"gpu-linear",os_gpu_linear_task_privileged,os_gpu_linear_pins,os_gpu_linear_start},
#endif
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
const struct os_session *os_session_admitted(void)
{
    const struct os_session *s=os_session();
    return s&&!memcmp(&s->board,&board.admitted_session,sizeof(s->board))&&os_session_valid(s)&&
        !memcmp(s->board.image,a733_smp_build_id,32)?s:NULL;
}
#ifdef SUNSTONE_A733_OS_AUDIO
int os_capture(unsigned g,const uint8_t **wav,size_t *bytes){return os_audio_capture(g,wav,bytes);}
#elif defined(SUNSTONE_A733_OS_HEADSET)
int os_capture(unsigned g,const uint8_t **wav,size_t *bytes){return os_headset_capture(g,wav,bytes);}
#endif
