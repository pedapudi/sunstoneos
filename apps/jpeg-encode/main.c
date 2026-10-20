/* Concrete hooks into the ordinary supervised board provider composition. */
#include "encode.h"
#include "snapshot.h"
#include "boot.h"
#include "platform.h"
#include "a733_storage_digest.h"
#include <string.h>
int board_application_task_privileged(void (*entry)(void *),void *context)
{return !context&&entry==jpeg_encode_task;}
int board_application_pins(struct a733_pin_handoff *pins)
{
    const struct a733_jpeg_encode_session *source=jpeg_encode_admitted_session();
    return pins&&source&&!memcmp(&source->board,&board.admitted_session,sizeof(source->board));
}
int board_application_start(void)
{
    const struct a733_jpeg_encode_session *source=jpeg_encode_admitted_session();
    if(!source||memcmp(&source->board,&board.admitted_session,sizeof(source->board)))return 0;
    jpeg_encode.session=*source;
    if(!a733_jpeg_encode_session_valid(&jpeg_encode.session)||memcmp(jpeg_encode.session.board.image,a733_smp_build_id,32))return 0;
    if(!jpeg_encode_upload_allocate())return 0;
    jpeg_encode.handle=xTaskCreateStatic(jpeg_encode_task,"jpeg",4096,NULL,4,jpeg_encode.stack,&jpeg_encode.task);
    return jpeg_encode.handle!=NULL;
}
