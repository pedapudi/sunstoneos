#include "uart.h"
#include "boot.h"
#include "a733_storage_digest.h"
#include <string.h>
static int failure(void)
{
 /* Metadata retention needs the real current task even after lifetime loss.
  * No FIFO, reset, pin or gate accesses follow a failed operation. */
 if(uart.platform.initialized)(void)a733_uart_platform_retain(&uart.platform);
 atomic_store_explicit(&uart.failed,1,memory_order_release);
 atomic_store_explicit(&uart.result,2,memory_order_release);
 return -1;
}
static int publish(void)
{
 memset(uart.payload,0,sizeof(uart.payload));uint64_t observed=a733_smp_counter()/24;
 memcpy(uart.payload,&observed,8);
 uint32_t values[]={uart.platform.uart.plan.actual_baud,(uint32_t)uart.sent,(uint32_t)uart.received,
 uart.platform.uart.errors,2,A733_UART_PLATFORM_CLOSED};
 memcpy(uart.payload+8,values,sizeof(values));memcpy(uart.payload+32,uart.tx,32);
 memcpy(uart.payload+64,uart.rx,32);memcpy(uart.payload+96,uart.status,32);
 storage_digest_bytes(uart.payload,sizeof(uart.payload),uart.digest);
 if(!uart_live()||observed>=uart.deadline)return failure();
 atomic_store_explicit(&uart.result,1,memory_order_release);return 1;
}
int uart_step(void)
{
 if(atomic_load_explicit(&uart.failed,memory_order_acquire)){
  /* Registry contention may have deferred retention metadata. */
  if(uart.platform.initialized)(void)a733_uart_platform_retain(&uart.platform);
  return -1;
 }
 if(!uart_live())return failure();
 if(atomic_load_explicit(&uart.result,memory_order_acquire)==1)return 1;
 if(a733_smp_counter()/24>=uart.deadline)return failure();
 enum a733_uart_platform_result p;
 switch(uart.stage){
 case 0:
  p=a733_uart_platform_poll(&uart.platform);
  if(p==A733_UART_PLATFORM_PENDING||p==A733_UART_PLATFORM_BUSY)return 0;
  if(p!=A733_UART_PLATFORM_READY)return failure();uart.stage=1;return 0;
 case 1:
  p=a733_uart_platform_live(&uart.platform,uart.deadline);
  if(p==A733_UART_PLATFORM_BUSY)return 0;
  if(p!=A733_UART_PLATFORM_READY)return failure();
  {struct a733_uart_byte bytes[32];struct a733_uart_result result={0};
   struct a733_uart *controller=a733_uart_platform_controller(&uart.platform);
   if(!controller)return failure();
   int r=a733_uart_exchange(controller,bytes,uart.session.rx_bytes-uart.received,
    uart.tx+uart.sent,uart.session.tx_bytes-uart.sent,32,uart.deadline,&result);
   /* Completed accesses remain recorded even if a later access faults. */
   for(size_t i=0;i<result.received;++i){uart.rx[uart.received+i]=bytes[i].value;uart.status[uart.received+i]=bytes[i].status;}
   uart.received+=result.received;uart.sent+=result.transmitted;
   if(r!=A733_UART_READY||result.errors)return failure();}
  if(uart.sent!=uart.session.tx_bytes||uart.received!=uart.session.rx_bytes)return 0;
  if(memcmp(uart.tx,uart.rx,uart.sent))return failure();
  uart.stage=2;return 0;
 case 2:
  p=a733_uart_platform_close(&uart.platform,1,uart.deadline);
  if(p==A733_UART_PLATFORM_BUSY)return 0;
  if(p!=A733_UART_PLATFORM_PENDING)return failure();uart.stage=3;return 0;
 case 3:
  p=a733_uart_platform_poll(&uart.platform);
  if(p==A733_UART_PLATFORM_PENDING||p==A733_UART_PLATFORM_BUSY)return 0;
  if(p!=A733_UART_PLATFORM_CLOSED)return failure();uart.stage=4;return publish();
 default:return failure();
 }
}
void uart_task(void *context)
{(void)context;for(;;){uart_step();vTaskDelay(1);}}
