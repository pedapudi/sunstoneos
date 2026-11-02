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
#ifndef configMAX_API_CALL_INTERRUPT_PRIORITY
#define configMAX_API_CALL_INTERRUPT_PRIORITY 11
#endif
#ifndef portPRIORITY_SHIFT
#define portPRIORITY_SHIFT 4
#endif
#include "host_rtos.h"
static unsigned alive=1,bind_phase,waits,begins,finishes,rearm_fail,work_pending;
static uint64_t gives_base;
static volatile uint32_t *regs;
static TaskHandle_t owner;
static unsigned notifies(void){return (unsigned)(host_gives_total()-gives_base);}
static int board_live(void *v){(void)v;return alive;}
/* The binding runs before the scheduler starts; the fixture rebinds between cases. */
BaseType_t __real_xTaskGetSchedulerState(void);
BaseType_t __wrap_xTaskGetSchedulerState(void)
{return bind_phase?taskSCHEDULER_NOT_STARTED:__real_xTaskGetSchedulerState();}
/* The owner's only blocking wait is one tick, taken outside a critical section. */
uint32_t __real_ulTaskGenericNotifyTake(UBaseType_t index,BaseType_t clear,TickType_t ticks);
uint32_t __wrap_ulTaskGenericNotifyTake(UBaseType_t index,BaseType_t clear,TickType_t ticks)
{
    if(ticks&&xTaskGetCurrentTaskHandle()==owner){assert(!host_critical_depth()&&clear==pdTRUE&&ticks==1);++waits;}
    return __real_ulTaskGenericNotifyTake(index,clear,ticks);
}
int a733_xhci_work_pending(const struct a733_xhci *x)
{assert(!host_critical_depth()&&x==&board.host.core);return work_pending;}
int gicv3_spi_mask(const struct gicv3_spi *s)
{assert((host_critical_depth()||host_in_isr())&&s->intid==187);regs[0x114/4]&=~(1u<<27);return 1;}
int gicv3_spi_rearm(const struct gicv3_spi *s)
{assert((host_critical_depth()||host_in_isr())&&s->intid==187);if(regs[0]>>31)return 0;if(rearm_fail)return -1;regs[0x114/4]|=1u<<27;return 1;}
int a733_xhci_interrupt_start(struct a733_xhci *x)
{assert(!host_critical_depth()&&x==&board.host.core);x->irq_mode=1;return 1;}
int a733_xhci_interrupt_begin(struct a733_xhci *x)
{assert(!host_critical_depth()&&!(regs[0]>>31)&&!((regs[0x114/4]|regs[0x314/4])&(1u<<27)));++begins;x->irq_service=1;return 1;}
int a733_xhci_interrupt_finish(struct a733_xhci *x)
{assert(!host_critical_depth()&&x->irq_service);++finishes;x->irq_service=0;return 1;}
#ifndef SUNSTONE_HOST_IRQ_SOURCE
#define SUNSTONE_HOST_IRQ_SOURCE "../apps/board/host_irq.c"
#endif
#include SUNSTONE_HOST_IRQ_SOURCE
static void irq_isr(void *unused){(void)unused;board_host_irq();}
/* An interrupt that arrives while the claim code holds its critical section is
 * delivered when the section ends, and then sets the GIC status bit. */
static void late_irq(void *unused){board_host_irq();regs[0]=1u<<31;(void)unused;}
static void setup(void)
{
 memset((void *)regs,0,4096);memset(&irq,0,sizeof(irq));memset(&board,0,sizeof(board));
 board.handles[4]=owner;board.host.core_started=1;regs[0x4b8/4]=0xb0000000;
 waits=begins=finishes=rearm_fail=work_pending=0;alive=1;gives_base=host_gives_total();
 bind_phase=1;assert(board_host_irq_bind());bind_phase=0;irq.armed=1;
}
static void test(void *unused)
{
 (void)unused;owner=xTaskGetCurrentTaskHandle();setup();
 host_isr(irq_isr,NULL);assert(notifies()==1&&atomic_load(&irq.pending));
 regs[0]=1u<<31;assert(board_host_irq_before()==0&&!begins&&atomic_load(&irq.pending));
 regs[0]=0;regs[0x314/4]=1u<<27;assert(board_host_irq_before()==0&&!begins);
 regs[0x314/4]=0;assert(board_host_irq_before()==1&&begins==1&&!atomic_load(&irq.pending));
 assert(board_host_irq_after()==1&&finishes==1&&!irq.masked&&(regs[0x114/4]&(1u<<27)));
 setup();host_irq_at(host_time_us(),late_irq,NULL);assert(board_host_irq_before()==1&&!begins&&atomic_load(&irq.pending));
 assert(board_host_irq_before()==0&&!begins); /* Arrival after critical exit was not claimed. */
 regs[0]=0;assert(board_host_irq_before()==1&&begins==1);
 setup();atomic_store(&irq.pending,1);assert(board_host_irq_after()==1&&irq.masked);
 assert(board_host_irq_before()==1&&begins==1);rearm_fail=1;assert(board_host_irq_after()<0&&irq.fault);
 board_host_irq_stop();assert(irq.masked&&irq.stopped&&!(regs[0x114/4]&(1u<<27)));
 setup();alive=0;host_isr(irq_isr,NULL);assert(irq.fault&&board_host_irq_before()<0);
 setup();regs[0]=1u<<31;atomic_store(&irq.pending,1);
 for(unsigned i=0;i<32;++i)assert(board_host_irq_before()==0);
 assert(board_host_irq_before()<0);board_host_irq_stop();assert(irq.masked);
 setup();board_host_irq_wait();assert(waits==1);
 irq.draining=1;board_host_irq_wait();assert(waits==1);
 setup();work_pending=1;
 for(unsigned i=0;i<31;++i)board_host_irq_wait();
 assert(!waits);board_host_irq_wait();assert(waits==1);
 puts("PASS actual owner IRQ mask propagation, inactive wait, claim race, pending rearm exclusion, notification, bounded failure retention");
}
int main(void)
{
 regs=mmap((void *)0x03400000,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
 assert(regs==(void *)0x03400000);
 host_rtos_run(test,NULL,2,1000000);
 return 0;
}
