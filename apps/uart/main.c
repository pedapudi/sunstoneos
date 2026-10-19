#include "uart.h"
#include "snapshot.h"
#include "boot.h"
#include <string.h>
int board_application_task_privileged(void (*entry)(void *),void *context)
{
#ifdef SUNSTONE_UART_DMA
 if(dma_service_task_privileged(entry,context))return 1;
#endif
 return !context&&entry==uart_task;
}
int board_application_pins(struct a733_pin_handoff *pins)
{
 const struct uart_session *s=uart_admitted_session();
 if(!pins||!s||memcmp(&s->board,&board.admitted_session,sizeof(s->board)))return 0;
 pins->authorized[A733_PIN_B]|=UINT32_C(3);return 1;
}
int board_application_start(void)
{
 const struct uart_session *s=uart_admitted_session();
 if(!s||memcmp(&s->board,&board.admitted_session,sizeof(s->board)))return 0;
 uart.session=*s;
 if(!uart_session_valid(&uart.session)||memcmp(s->board.image,a733_smp_build_id,32))return 0;
 uart.handle=xTaskCreateStatic(uart_task,"uart",2048,NULL,4,uart.stack,&uart.task);
 return uart.handle&&uart_construct();
}
