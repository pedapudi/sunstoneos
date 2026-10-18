#include "thermal.h"
#include "snapshot.h"
#include "boot.h"
#include <string.h>
int board_application_task_privileged(void (*entry)(void *),void *context)
{return !context&&entry==thermal_task;}
int board_application_pins(struct a733_pin_handoff *pins)
{
 const struct thermal_session *source=thermal_admitted_session();
 return pins&&source&&!memcmp(&source->board,&board.admitted_session,sizeof(source->board));
}
int board_application_start(void)
{
 const struct thermal_session *source=thermal_admitted_session();
 if(!source||memcmp(&source->board,&board.admitted_session,sizeof(source->board)))return 0;
 thermal.session=*source;
 if(!thermal_session_valid(&thermal.session)||memcmp(thermal.session.board.image,a733_smp_build_id,32)||
 !thermal_construct())return 0;
 thermal.handle=xTaskCreateStatic(thermal_task,"thermal",2048,NULL,4,thermal.stack,&thermal.task);
 return thermal.handle!=NULL;
}
