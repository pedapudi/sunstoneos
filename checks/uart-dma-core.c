#include "a733_uart.h"
#include "a733_uart_platform_native.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint32_t regs[0xa8/4];static unsigned bad_write,writes;
static int now(void *v,uint64_t *n){(void)v;*n=1;return 1;}
static int read32(void *v,uintptr_t a,uint32_t *n){(void)v;*n=regs[(a-0x02502000)/4];return 1;}
static int write32(void *v,uintptr_t a,uint32_t n)
{(void)v;++writes;if(bad_write)return 0;regs[(a-0x02502000)/4]=n;return 1;}
int main(void)
{
 struct a733_uart u={.plan={.base=0x02502000},.ops={read32,write32,now},.state=A733_UART_READY};
 assert(a733_uart_dma_prepare(&u,100)==A733_UART_READY&&u.dma_owned);
 assert(regs[0x88/4]==0xe5&&regs[8/4]==9&&regs[0xa4/4]==0xc2&&!regs[0x8c/4]);
 unsigned before=writes;struct a733_uart_result out;uint8_t tx=7;
 assert(a733_uart_exchange(&u,NULL,0,&tx,1,1,100,&out)==-1);
 assert(a733_uart_tx_irq(&u,1,100)==-1&&a733_uart_drain(&u,100)==-1&&writes==before);
 assert(a733_uart_dma_requests(&u,1,100)==A733_UART_READY);
 assert(a733_uart_dma_release(&u,1,100)==-1);
 assert(a733_uart_dma_requests(&u,0,100)==A733_UART_READY);
 regs[0x14/4]=0x61;regs[0x7c/4]=6;assert(a733_uart_dma_release(&u,1,100)==0&&u.dma_owned);
 regs[0x14/4]=0x20;assert(a733_uart_dma_release(&u,1,100)==0);
 regs[0x14/4]=0x60;regs[0x7c/4]=7;assert(a733_uart_dma_release(&u,1,100)==0);
 regs[0x7c/4]=6;assert(a733_uart_dma_release(&u,1,100)==A733_UART_READY&&!u.dma_owned);
 /* Release is terminal: IER stays 0, so only drain and close may follow. */
 before=writes;
 assert(a733_uart_exchange(&u,NULL,0,&tx,1,1,100,&out)==-1&&a733_uart_interrupt(&u,NULL,0,&tx,1,1,100,&out)==-1);
 assert(a733_uart_tx_irq(&u,1,100)==-1&&a733_uart_dma_prepare(&u,100)==-1&&writes==before&&!regs[4/4]);
 assert(a733_uart_drain(&u,100)==A733_UART_DRAINING);
 u.state=A733_UART_READY;u.dma_released=0;
 assert(a733_uart_platform_native_address(2,0x02502088,1));
 assert(a733_uart_platform_native_address(2,0x0250208c,0));
 assert(!a733_uart_platform_native_address(1,0x02501088,1));
 bad_write=1;assert(a733_uart_dma_prepare(&u,100)==A733_UART_FAULT&&u.dma_owned);
 puts("PASS DMA lease exclusion; terminal release; request-disable, true TEMT, RX-empty, idle and uncertainty retention");
}
