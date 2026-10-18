#include "pwm.h"
#include "snapshot.h"
#include "boot.h"
#include "a733_storage_digest.h"
#include <string.h>
int board_application_task_privileged(void (*entry)(void *),void *context)
{return !context&&entry==pwm_task;}
int board_application_pins(struct a733_pin_handoff *pins)
{
 const struct pwm_session *s=pwm_admitted_session();
 if(!pins||!s||memcmp(&s->board,&board.admitted_session,sizeof(s->board)))return 0;
 pins->authorized[A733_PIN_D]|=1u<<5;
 if(s->led_enabled){pins->authorized[A733_PIN_L]|=1u<<7;
  pins->latch[0]=(struct a733_pin_latch_handoff){s->latch_identity,s->latch_owner,s->latch_known_mask,s->latch_value};}
 return 1;
}
int board_application_start(void)
{
 const struct pwm_session *s=pwm_admitted_session();
 if(!s||memcmp(&s->board,&board.admitted_session,sizeof(s->board))||
 !pwm_session_valid(s)||memcmp(s->board.image,a733_smp_build_id,32))return 0;
 pwm.session=*s;storage_digest_bytes(&pwm.session,sizeof(pwm.session),pwm.session_digest);
 pwm.mutex=xSemaphoreCreateMutexStatic(&pwm.mutex_storage);
 if(!pwm.mutex)return 0;
 pwm.handle=xTaskCreateStatic(pwm_task,"pwm-fan",2048,NULL,4,pwm.stack,&pwm.task);
 return pwm.handle!=NULL;
}
