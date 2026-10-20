/* Concrete hooks into the ordinary supervised board provider composition. */
#include "g2d.h"
#include "snapshot.h"
#include "boot.h"
#include "platform.h"
#include "a733_storage_digest.h"
#include <string.h>
int board_application_task_privileged(void (*entry)(void *),void *context)
{return !context&&entry==g2d_task;}
int board_application_pins(struct a733_pin_handoff *pins)
{
    const struct a733_g2d_session *source=g2d_admitted_session();
    return pins&&source&&!memcmp(&source->board,&board.admitted_session,sizeof(source->board));
}
int board_application_start(void)
{
    const struct a733_g2d_session *source=g2d_admitted_session();
    if(!source||memcmp(&source->board,&board.admitted_session,sizeof(source->board)))return 0;
    g2d.session=*source;
    if(!a733_g2d_session_valid(&g2d.session)||memcmp(g2d.session.board.image,a733_smp_build_id,32))return 0;
    if(!g2d_upload_allocate())return 0;
    g2d.handle=xTaskCreateStatic(g2d_task,"g2d",4096,NULL,4,g2d.stack,&g2d.task);
    return g2d.handle!=NULL;
}
