/* Concrete hooks into the ordinary supervised board provider composition. */
#include "h264.h"
#include "boot.h"
#include "platform.h"
#include "a733_storage_digest.h"
#include <string.h>
int board_application_task_privileged(void (*entry)(void *),void *context)
{return !context&&entry==h264_task;}
int board_application_pins(struct a733_pin_handoff *pins)
{
    const struct a733_h264_session *source=h264_admitted_session();
    return pins&&source&&!memcmp(&source->board,&board.admitted_session,sizeof(source->board));
}
int board_application_start(void)
{
    const struct a733_h264_session *source=h264_admitted_session();
    if(!source||memcmp(&source->board,&board.admitted_session,sizeof(source->board)))return 0;
    h264.session=*source;
    uint8_t hash[32];
    if(!a733_h264_session_valid(&h264.session)||memcmp(h264.session.board.image,a733_smp_build_id,32)||
       h264_fixture_end<=h264_fixture||h264_fixture_end-h264_fixture>A733_H264_INPUT_BYTES||h264_reference_end-h264_reference!=2304)return 0;
    storage_digest_bytes(h264_fixture,(size_t)(h264_fixture_end-h264_fixture),hash);
    struct a733_h264_picture picture;
    if(!a733_h264_parse(h264_fixture,(size_t)(h264_fixture_end-h264_fixture),&picture)||
       picture.width!=48||picture.height!=32)return 0;
    if(memcmp(hash,h264.session.fixture,32))return 0;
    storage_digest_bytes(h264_reference,2304,hash);
    if(memcmp(hash,h264.session.reference,32))return 0;
    h264.handle=xTaskCreateStatic(h264_task,"h264",4096,NULL,4,h264.stack,&h264.task);
    return h264.handle!=NULL;
}
