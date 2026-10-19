/* Actual application mask/claim/rearm code; only kernel and GIC observations
 * are modeled. The composed driver check exercises the actual xHCI service. */
#define SUNSTONE_BOARD_APPLICATION_PRIVATE_H
#include "a733_usb2_native.h"
#include "gicv3_spi.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
struct {void *handles[8];struct a733_usb2_native host;} board;
typedef unsigned UBaseType_t;
typedef int BaseType_t;
#define pdFALSE 0
#define pdTRUE 1
#define taskSCHEDULER_NOT_STARTED 0
#define configMAX_API_CALL_INTERRUPT_PRIORITY 11
#define portPRIORITY_SHIFT 4
static unsigned critical,alive=1,started,notifies,waits,begins,finishes,rearm_fail,inject_exit;
static volatile uint32_t *regs;
static int board_live(void *v){(void)v;return alive;}
static int xTaskGetSchedulerState(void){return started;}
static void enter(void){assert(!critical);critical=1;}
static void leave(void);
#define taskENTER_CRITICAL() enter()
#define taskEXIT_CRITICAL() leave()
#define taskENTER_CRITICAL_FROM_ISR() (enter(),0)
#define taskEXIT_CRITICAL_FROM_ISR(saved) do{(void)(saved);leave();}while(0)
#define portYIELD_FROM_ISR(woken) ((void)(woken))
static void vTaskNotifyGiveFromISR(void *task,BaseType_t *woken)
{assert(critical&&task==board.handles[4]);++notifies;*woken=1;}
static unsigned ulTaskNotifyTake(unsigned clear,unsigned ticks)
{assert(!critical&&clear==1&&ticks==1);++waits;return 0;}
int gicv3_spi_mask(const struct gicv3_spi *s)
{assert(critical&&s->intid==187);regs[0x114/4]&=~(1u<<27);return 1;}
int gicv3_spi_rearm(const struct gicv3_spi *s)
{assert(critical&&s->intid==187);if(regs[0]>>31)return 0;if(rearm_fail)return -1;regs[0x114/4]|=1u<<27;return 1;}
int a733_xhci_interrupt_start(struct a733_xhci *x)
{assert(!critical&&x==&board.host.core);x->irq_mode=1;return 1;}
int a733_xhci_interrupt_begin(struct a733_xhci *x)
{assert(!critical&&!(regs[0]>>31)&&!((regs[0x114/4]|regs[0x314/4])&(1u<<27)));++begins;x->irq_service=1;return 1;}
int a733_xhci_interrupt_finish(struct a733_xhci *x)
{assert(!critical&&x->irq_service);++finishes;x->irq_service=0;return 1;}
#include "../apps/board/host_irq.c"
static void leave(void)
{assert(critical);critical=0;if(inject_exit){inject_exit=0;board_host_irq();regs[0]=1u<<31;}}
static void setup(void)
{
 memset((void *)regs,0,4096);memset(&irq,0,sizeof(irq));memset(&board,0,sizeof(board));
 board.handles[4]=(void *)1;board.host.core_started=1;regs[0x4b8/4]=0xb0000000;
 critical=started=notifies=waits=begins=finishes=rearm_fail=inject_exit=0;alive=1;
 assert(board_host_irq_bind());started=1;irq.armed=1;
}
int main(void)
{
 regs=mmap((void *)0x03400000,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
 assert(regs==(void *)0x03400000);setup();
 board_host_irq();assert(notifies==1&&atomic_load(&irq.pending));
 regs[0]=1u<<31;assert(board_host_irq_before()==0&&!begins&&atomic_load(&irq.pending));
 regs[0]=0;regs[0x314/4]=1u<<27;assert(board_host_irq_before()==0&&!begins);
 regs[0x314/4]=0;assert(board_host_irq_before()==1&&begins==1&&!atomic_load(&irq.pending));
 assert(board_host_irq_after()==1&&finishes==1&&!irq.masked&&(regs[0x114/4]&(1u<<27)));
 setup();inject_exit=1;assert(board_host_irq_before()==1&&!begins&&atomic_load(&irq.pending));
 assert(board_host_irq_before()==0&&!begins); /* Arrival after critical exit was not claimed. */
 regs[0]=0;assert(board_host_irq_before()==1&&begins==1);
 setup();atomic_store(&irq.pending,1);assert(board_host_irq_after()==1&&irq.masked);
 assert(board_host_irq_before()==1&&begins==1);rearm_fail=1;assert(board_host_irq_after()<0&&irq.fault);
 board_host_irq_stop();assert(irq.masked&&irq.stopped&&!(regs[0x114/4]&(1u<<27)));
 setup();alive=0;board_host_irq();assert(irq.fault&&board_host_irq_before()<0);
 setup();regs[0]=1u<<31;atomic_store(&irq.pending,1);
 for(unsigned i=0;i<32;++i)assert(board_host_irq_before()==0);
 assert(board_host_irq_before()<0);board_host_irq_stop();assert(irq.masked);
 setup();board_host_irq_wait();assert(waits==1);
 irq.draining=1;board_host_irq_wait();assert(waits==1);
 puts("PASS actual owner IRQ mask propagation, inactive wait, claim race, pending rearm exclusion, notification, bounded failure retention");
}
