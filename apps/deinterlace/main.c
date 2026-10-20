/* Concrete hooks into the ordinary supervised board provider composition. */
#include "deinterlace.h"
#include "snapshot.h"
#include "boot.h"
#include "platform.h"
#include "a733_storage_digest.h"
#include <string.h>
int board_application_task_privileged(void (*entry)(void *),void *context)
{return !context&&entry==deinterlace_task;}
int board_application_pins(struct a733_pin_handoff *pins)
{
    const struct a733_deinterlace_session *source=deinterlace_admitted_session();
    return pins&&source&&!memcmp(&source->board,&board.admitted_session,sizeof(source->board));
}
int board_application_start(void)
{
    const struct a733_deinterlace_session *source=deinterlace_admitted_session();
    if(!source||memcmp(&source->board,&board.admitted_session,sizeof(source->board)))return 0;
    deinterlace.session=*source;
    if(!a733_deinterlace_session_valid(&deinterlace.session)||memcmp(deinterlace.session.board.image,a733_smp_build_id,32))return 0;
    if(!deinterlace_upload_allocate())return 0;
    deinterlace.handle=xTaskCreateStatic(deinterlace_task,"deinterlace",4096,NULL,4,deinterlace.stack,&deinterlace.task);
    return deinterlace.handle!=NULL;
}
