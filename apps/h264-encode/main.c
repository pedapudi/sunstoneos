/* Concrete hooks into the ordinary supervised board provider composition. */
#include "encode.h"
#include "snapshot.h"
#include "boot.h"
#include "platform.h"
#include "a733_storage_digest.h"
#include <string.h>
int board_application_task_privileged(void (*entry)(void *),void *context)
{return !context&&entry==h264_encode_task;}
int board_application_pins(struct a733_pin_handoff *pins)
{
    const struct a733_h264_encode_session *source=h264_encode_admitted_session();
    return pins&&source&&!memcmp(&source->board,&board.admitted_session,sizeof(source->board));
}
int board_application_start(void)
{
    const struct a733_h264_encode_session *source=h264_encode_admitted_session();
    if(!source||memcmp(&source->board,&board.admitted_session,sizeof(source->board)))return 0;
    h264_encode.session=*source;
    if(!a733_h264_encode_session_valid(&h264_encode.session)||memcmp(h264_encode.session.board.image,a733_smp_build_id,32))return 0;
    if(!h264_encode_storage_allocate())return 0;
    h264_encode.handle=xTaskCreateStatic(h264_encode_task,"h264-encode",4096,NULL,4,h264_encode.stack,&h264_encode.task);
    return h264_encode.handle!=NULL;
}
