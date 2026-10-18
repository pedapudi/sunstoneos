#include "adc.h"
#include "snapshot.h"
#include "boot.h"
#include <string.h>
int board_application_task_privileged(void (*entry)(void *),void *context)
{return !context&&entry==adc_task;}
int board_application_pins(struct a733_pin_handoff *pins)
{
 const struct adc_session *s=adc_admitted_session();
 if(!pins||!s||memcmp(&s->board,&board.admitted_session,sizeof(s->board)))return 0;
 return 1;
}
int board_application_start(void)
{
 const struct adc_session *s=adc_admitted_session();
 if(!s||memcmp(&s->board,&board.admitted_session,sizeof(s->board)))return 0;
 adc.session=*s;
 if(!adc_session_valid(&adc.session)||memcmp(s->board.image,a733_smp_build_id,32))return 0;
 adc.handle=xTaskCreateStatic(adc_task,"adc",2048,NULL,4,adc.stack,&adc.task);
 return adc.handle&&adc_construct();
}
