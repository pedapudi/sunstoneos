/* Concrete hooks into the ordinary supervised board provider composition. */
#include "hevc.h"
#include "boot.h"
#include "platform.h"
#include "a733_memory_reservations.h"
#include "a733_memory_uncached.h"
#include <string.h>
/* Private parser state for the one upload; never device-visible. */
static _Alignas(16) uint8_t hevc_parser[A733_HEVC_PARSER_BYTES];
int board_application_task_privileged(void (*entry)(void *),void *context)
{return !context&&entry==hevc_task;}
int board_application_pins(struct a733_pin_handoff *pins)
{
    const struct a733_hevc_session *source=hevc_admitted_session();
    return pins&&source&&!memcmp(&source->board,&board.admitted_session,sizeof(source->board));
}
/* Before scheduler release: the extended-memory provider has completed both
 * reservation scans, and no USB or controller task observes partial storage. */
int board_application_start(void)
{
    const struct a733_hevc_session *source=hevc_admitted_session();
    if(!source||memcmp(&source->board,&board.admitted_session,sizeof(source->board)))return 0;
    hevc.session=*source;
    if(!a733_hevc_session_valid(&hevc.session)||memcmp(hevc.session.board.image,a733_smp_build_id,32)||
       a733_hevc_parser_bytes()>sizeof(hevc_parser))return 0;
    if(!hevc_storage_init(&hevc.storage,a733_memory_reservations_init(),a733_memory_uncached_init(),hevc.session.owner)||
       !hevc_input_bind(&hevc.supplied,hevc.storage.upload,A733_HEVC_INPUT_CAPACITY,hevc_parser,sizeof(hevc_parser)))return 0;
    hevc.handle=xTaskCreateStatic(hevc_task,"hevc",4096,NULL,4,hevc.stack,&hevc.task);
    return hevc.handle!=NULL;
}
