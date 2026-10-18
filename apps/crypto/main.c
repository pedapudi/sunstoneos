#include "crypto.h"
#include "boot.h"
#include <string.h>
int crypto_construct(void);
int board_application_task_privileged(void (*entry)(void *),void *context)
{return !context&&entry==crypto_task;}
int board_application_pins(struct a733_pin_handoff *pins){return pins!=NULL;}
int board_application_start(void)
{
 const volatile uint8_t *source=(const void *)A733_BOARD_SESSION_ADDRESS;
 for(unsigned i=0;i<sizeof(crypto.session);++i)((uint8_t *)&crypto.session)[i]=source[i];
 if(!crypto_session_valid(&crypto.session)||memcmp(crypto.session.board.image,a733_smp_build_id,32)||
 !crypto_construct())return 0;
 crypto.handle=xTaskCreateStatic(crypto_task,"crypto",2048,NULL,4,crypto.stack,&crypto.task);
 return crypto.handle!=NULL;
}
