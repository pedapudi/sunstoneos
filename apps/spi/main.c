#include "spi.h"
#include "snapshot.h"
#include "irq.h"
#include "boot.h"
#include <string.h>
int board_application_task_privileged(void (*entry)(void *),void *context)
{return !context&&entry==spi_task;}
int board_application_pins(struct a733_pin_handoff *pins)
{
 const struct spi_session *s=spi_admitted_session();
 if(!pins||!s||memcmp(&s->board,&board.admitted_session,sizeof(s->board)))return 0;
 pins->authorized[A733_PIN_E]|=UINT32_C(15);return 1;
}
int board_application_start(void)
{
 const struct spi_session *s=spi_admitted_session();
 if(!s||memcmp(&s->board,&board.admitted_session,sizeof(s->board)))return 0;
 spi.session=*s;
 if(!spi_session_valid(&spi.session)||memcmp(s->board.image,a733_smp_build_id,32))return 0;
 spi.handle=xTaskCreateStatic(spi_task,"spi",2048,NULL,4,spi.stack,&spi.task);
 if(!spi.handle)return 0;
 if(SPI_SESSION_USES_IRQ(spi.session.version))vTaskCoreAffinitySet(spi.handle,1);
 if(!spi_construct())return 0;
 return !SPI_SESSION_USES_IRQ(spi.session.version)||spi_irq_bind();
}
