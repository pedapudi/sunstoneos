/* Concrete hooks into the ordinary supervised board provider composition. */
#include "jpeg.h"
#include "snapshot.h"
#include "boot.h"
#include "platform.h"
#include "a733_storage_digest.h"
#include <string.h>
int board_application_task_privileged(void (*entry)(void *),void *context)
{return !context&&entry==jpeg_task;}
int board_application_pins(struct a733_pin_handoff *pins)
{
    const struct a733_jpeg_session *source=jpeg_admitted_session();
    return pins&&source&&!memcmp(&source->board,&board.admitted_session,sizeof(source->board));
}
int board_application_start(void)
{
    const struct a733_jpeg_session *source=jpeg_admitted_session();
    if(!source||memcmp(&source->board,&board.admitted_session,sizeof(source->board)))return 0;
    jpeg.session=*source;
    if(!a733_jpeg_session_valid(&jpeg.session)||memcmp(jpeg.session.board.image,a733_smp_build_id,32))return 0;
    jpeg.handle=xTaskCreateStatic(jpeg_task,"jpeg",4096,NULL,4,jpeg.stack,&jpeg.task);
    return jpeg.handle!=NULL;
}
