#include "rtc.h"
#include "boot.h"
#include <string.h>
int board_application_task_privileged(void (*entry)(void *),void *context)
{return !context&&entry==rtc_task;}
int board_application_pins(struct a733_pin_handoff *pins)
{
 const struct a733_rtc_session *s=rtc_admitted_session();
 return pins&&s&&!memcmp(&s->board,&board.admitted_session,sizeof(s->board));
}
int board_application_start(void)
{
 const struct a733_rtc_session *s=rtc_admitted_session();
 if(!s||memcmp(&s->board,&board.admitted_session,sizeof(s->board))||
    !a733_rtc_session_valid(s)||memcmp(s->board.image,a733_smp_build_id,32))return 0;
 rtc.session=*s;
 rtc.spi=(struct gicv3_spi){0x03400000,228};
 rtc.handle=xTaskCreateStaticAffinitySet(rtc_task,"rtc",2048,NULL,4,rtc.stack,&rtc.task,1);
 return rtc.handle!=NULL;
}
