#include "crypto.h"
#include "snapshot.h"
#include "boot.h"
#include <string.h>
int crypto_construct(void);
int board_application_task_privileged(void (*entry)(void *),void *context)
{return !context&&entry==crypto_task;}
int board_application_pins(struct a733_pin_handoff *pins)
{
 const struct crypto_session *source=crypto_admitted_session();
 return pins&&source&&!memcmp(&source->board,&board.admitted_session,sizeof(source->board));
}
int board_application_start(void)
{
 const struct crypto_session *source=crypto_admitted_session();
 if(!source||memcmp(&source->board,&board.admitted_session,sizeof(source->board)))return 0;
 crypto.session=*source;
 if(!crypto_session_valid(&crypto.session)||memcmp(crypto.session.board.image,a733_smp_build_id,32)||
 (crypto.session.version==1&&!crypto_construct()))return 0;
 if(crypto.session.version==2){
  crypto.mutex=xSemaphoreCreateMutexStatic(&crypto.mutex_storage);if(!crypto.mutex)return 0;
 }else crypto.constructed=1;
 crypto.handle=xTaskCreateStatic(crypto_task,"crypto",2048,NULL,4,crypto.stack,&crypto.task);
 return crypto.handle!=NULL;
}
