/* Concrete hooks into the ordinary supervised board provider composition. */
#include "jpeg.h"
#include "boot.h"
#include "platform.h"
#include "a733_storage_digest.h"
#include <string.h>
int board_application_task_privileged(void (*entry)(void *),void *context)
{return !context&&entry==jpeg_task;}
int board_application_pins(struct a733_pin_handoff *pins)
{return pins!=NULL;}
int board_application_start(void)
{
    const volatile uint8_t *source=(const void *)A733_BOARD_SESSION_ADDRESS;
    for(unsigned i=0;i<sizeof(jpeg.session);++i)((uint8_t *)&jpeg.session)[i]=source[i];
    uint8_t hash[32];
    if(!a733_jpeg_session_valid(&jpeg.session)||memcmp(jpeg.session.board.image,a733_smp_build_id,32)||
       !jpeg_fixture_bytes()||jpeg_fixture_bytes()>4096||jpeg_reference_end-jpeg_reference!=2304)return 0;
    storage_digest_bytes(jpeg_fixture,jpeg_fixture_bytes(),hash);
    if(memcmp(hash,jpeg.session.fixture,32))return 0;
    storage_digest_bytes(jpeg_reference,2304,hash);
    if(memcmp(hash,jpeg.session.reference,32))return 0;
    jpeg.handle=xTaskCreateStatic(jpeg_task,"jpeg",4096,NULL,4,jpeg.stack,&jpeg.task);
    return jpeg.handle!=NULL;
}
