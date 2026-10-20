/* Concrete hooks into the ordinary supervised board provider composition. */
#include "isp0.h"
#include "snapshot.h"
#include "boot.h"
#include "platform.h"
#include "a733_storage_digest.h"
#include <string.h>
int board_application_task_privileged(void (*entry)(void *),void *context)
{return !context&&entry==isp0_task;}
int board_application_pins(struct a733_pin_handoff *pins)
{
    const struct a733_isp0_session *source=isp0_admitted_session();
    return pins&&source&&!memcmp(&source->board,&board.admitted_session,sizeof(source->board));
}
int board_application_start(void)
{
    const struct a733_isp0_session *source=isp0_admitted_session();
    if(!source||memcmp(&source->board,&board.admitted_session,sizeof(source->board)))return 0;
    isp0.session=*source;
    if(!a733_isp0_session_valid(&isp0.session)||memcmp(isp0.session.board.image,a733_smp_build_id,32))return 0;
    if(!isp0_upload_allocate())return 0;
    isp0.handle=xTaskCreateStatic(isp0_task,"isp0",4096,NULL,4,isp0.stack,&isp0.task);
    return isp0.handle!=NULL;
}
