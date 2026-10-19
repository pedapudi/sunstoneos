/* Reuse the accepted actual-provider fixture without repeating its cases. */
#define main accepted_uart_platform_main
#include "a733-uart-platform.c"
#undef main
int main(void)
{
 setup();initialize_fixture(&first,A733_UART2_PB,0,1);ready(&first);
 unsigned before=reads;
 assert(a733_uart_platform_irq_sync(&first.binding,900)==A733_UART_PLATFORM_INVALID&&reads==before);
 setup();deferred_irq=1;initialize_fixture(&first,A733_UART2_PB,0,1);ready(&first);
 uint64_t deadline=now_us+500;struct a733_uart *u=a733_uart_platform_controller(&first.binding);
 *reg(irq_reg(2,0x100))=BIT(4);
 assert(a733_uart_platform_irq_sync(&first.binding,deadline)==A733_UART_PLATFORM_PENDING);
 *reg(irq_reg(2,0x100))=0;*reg(irq_reg(2,0x300))=BIT(4);
 assert(a733_uart_platform_irq_sync(&first.binding,deadline)==A733_UART_PLATFORM_PENDING);
 *reg(irq_reg(2,0x300))=0;first.gic_wait=1;
 assert(a733_uart_platform_irq_sync(&first.binding,deadline)==A733_UART_PLATFORM_PENDING);
 assert(a733_uart_platform_irq_sync(&first.binding,deadline)==A733_UART_PLATFORM_READY);
 unsigned sent=first.sent;assert(a733_uart_tx_irq(u,1,deadline)==A733_UART_READY&&first.ier==7&&first.sent==sent);
 struct a733_uart_byte rx;struct a733_uart_result r;uint8_t tx=0x67;first.cause=2;
 assert(a733_uart_interrupt(u,&rx,1,&tx,1,1,deadline,&r)==A733_UART_READY&&r.cause==2&&r.transmitted==1&&!r.received&&first.ier==5);
 first.cause=12;first.rx_count=1;first.rx=tx;
 assert(a733_uart_interrupt(u,&rx,1,NULL,0,1,deadline,&r)==A733_UART_READY&&r.cause==12&&r.received==1&&rx.value==tx);
 first.cause=6;first.rx_count=1;first.line_error=2;
 assert(a733_uart_interrupt(u,&rx,1,NULL,0,1,deadline,&r)==A733_UART_READY&&r.errors==2&&rx.status==2);
 first.line_error=0;closed(&first);
 setup();deferred_irq=1;initialize_fixture(&first,A733_UART2_PB,0,1);ready(&first);
 u=a733_uart_platform_controller(&first.binding);first.fail_write=writes+1;
 assert(a733_uart_tx_irq(u,1,now_us+100)==A733_UART_FAULT);retained(&first,1);
 puts("Deferred UART IRQ platform: real provider masked/inactive/RWP synchronization, THRE arm without TX, IIR TX/RX-timeout/overrun and retained uncertain arm passed.");
}
