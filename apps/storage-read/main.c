#include "storage_read.h"
#include "snapshot.h"
#include "boot.h"
#include <string.h>
int board_application_task_privileged(void (*entry)(void *),void *context)
{return !context&&entry==storage_read_task;}
int board_application_pins(struct a733_pin_handoff *pins)
{
 const struct storage_read_session *s=storage_read_admitted_session();
 if(!pins||!s||memcmp(&s->board,&board.admitted_session,sizeof(s->board)))return 0;
 pins->authorized[A733_PIN_F]|=0x7f;return 1;
}
int board_application_start(void)
{
 const struct storage_read_session *s=storage_read_admitted_session();
 if(!s||memcmp(&s->board,&board.admitted_session,sizeof(s->board))||
 !storage_read_session_valid(s)||memcmp(s->board.image,a733_smp_build_id,32))return 0;
 storage_read.session=*s;
 storage_read.mutex=xSemaphoreCreateMutexStatic(&storage_read.mutex_storage);
 if(!storage_read.mutex)return 0;
 storage_read.handle=xTaskCreateStatic(storage_read_task,"sd0-read",2048,NULL,4,storage_read.stack,&storage_read.task);
 return storage_read.handle!=NULL;
}
