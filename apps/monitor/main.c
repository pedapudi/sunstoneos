#include "monitor.h"
#include "snapshot.h"
#include "boot.h"
#include <string.h>
int board_application_task_privileged(void (*entry)(void *),void *context)
{return entry==monitor_task&&(context==&monitor.owners[0]||context==&monitor.owners[1]);}
int board_application_pins(struct a733_pin_handoff *pins)
{
 const struct monitor_session *s=monitor_admitted_session();
 return pins&&s&&!memcmp(&s->board,&board.admitted_session,sizeof(s->board));
}
int board_application_start(void)
{
 const struct monitor_session *s=monitor_admitted_session();
 if(!s||memcmp(&s->board,&board.admitted_session,sizeof(s->board))||
 !monitor_session_valid(s)||memcmp(s->board.image,a733_smp_build_id,32))return 0;
 monitor.session=*s;
 for(unsigned i=0;i<2;++i){struct monitor_owner *o=&monitor.owners[i];o->kind=i;
  o->handle=xTaskCreateStatic(monitor_task,i?"thermal":"adc",2048,o,4,o->stack,&o->task);
  if(!o->handle)return 0;}
 return monitor_construct();
}
