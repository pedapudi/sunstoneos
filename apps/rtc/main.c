#include "rtc.h"
#include "boot.h"
#include <string.h>
int board_application_task_privileged(void (*entry)(void *),void *context)
{return (!context&&entry==rtc_task)||(entry==rtc_service_client&&(uintptr_t)context<2);}
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
 if(s->version>=2){
  const struct a733_rtc_setting *setting=rtc_admitted_setting();
  const struct a733_rtc_initialization *initialization=rtc_admitted_initialization();
  if(!setting||!initialization||initialization->stage!=6)return 0;
  rtc.setting=*setting;rtc.initialized_output.initialization=*initialization;
  rtc.runtime_output.boot=*initialization;
 }
 rtc.spi=(struct gicv3_spi){0x03400000,228};
 if(s->version==4){
  for(unsigned i=0;i<2;++i){
   rtc.client_handles[i]=xTaskCreateStaticAffinitySet(rtc_service_client,"rtc-client",1024,
       (void *)(uintptr_t)i,3,rtc.client_stacks[i],&rtc.clients[i],1);
   if(!rtc.client_handles[i])return 0;
  }
  if(!rtc_service_prepare(rtc.client_handles,rtc.setting.day,rtc.setting.hms,rtc.session.alarm_seconds))return 0;
 }
 rtc.handle=xTaskCreateStaticAffinitySet(rtc_task,"rtc",2048,NULL,4,rtc.stack,&rtc.task,1);
 return rtc.handle!=NULL;
}
