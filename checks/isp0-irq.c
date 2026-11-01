/* Exercise the native arm state machine with modeled GIC propagation. */
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include "gicv3_spi.h"
typedef unsigned UBaseType_t;
typedef unsigned TickType_t;
typedef int BaseType_t;
#define pdFALSE 0
#define pdTRUE 1
#define SUNSTONE_ISP0_IRQ 1
static struct {void *handle;unsigned waiting;atomic_uint interrupts[3];} isp0;
static unsigned rwp,active,masks[3],clears[3],writes[3],enables[3],notified,waited,delayed;
static uint32_t programmed[3];
static int owner(void){return 1;}
static int board_live(void *unused){(void)unused;return 1;}
static uint64_t deadline(void){return 22345;}
static int now(void *unused,uint64_t *value){(void)unused;*value=10000;return 1;}
static void vTaskDelay(unsigned ticks){delayed+=ticks;}
static unsigned ulTaskNotifyTake(int clear,unsigned ticks){(void)clear;waited=ticks;return 0;}
static void vTaskNotifyGiveFromISR(void *handle,BaseType_t *woken)
{(void)handle;*woken=0;++notified;}
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
#define taskENTER_CRITICAL_FROM_ISR() 0u
#define taskEXIT_CRITICAL_FROM_ISR(saved) ((void)(saved))
#define portYIELD_FROM_ISR(woken) ((void)(woken))
static int index_of(unsigned id){return id==149?0:id==153?1:id==161?2:-1;}
int gicv3_spi_mask(const struct gicv3_spi *source)
{int index=index_of(source->intid);if(index<0)return 0;++masks[index];enables[index]=0;rwp=1;return 1;}
int gicv3_spi_rearm(const struct gicv3_spi *source)
{int index=index_of(source->intid);if(index<0)return -1;if(rwp)return 0;enables[index]=1;return 1;}
static int load(uintptr_t address,uint32_t *value)
{
 if(address==0x03400000){*value=rwp?1u<<31:0;return 1;}
 if(address==0x03400310||address==0x03400314){*value=active;return 1;}
 unsigned index=address==0x05830250?0:address==0x05910020?1:address==0x05900028?2:3;
 if(index==3)return 0;*value=programmed[index];return 1;
}
static int store(uintptr_t address,uint32_t value)
{
 if(address==0x03400290||address==0x03400294){
  unsigned id=address==0x03400290?(value==1u<<21?149:153):161;
  int index=index_of(id);if(index<0)return 0;++clears[index];return 1;
 }
 unsigned index=address==0x05830250?0:address==0x05910020?1:address==0x05900028?2:3;
 if(index==3)return 0;++writes[index];programmed[index]=value;return 1;
}
#include "../apps/isp0/irq.h"
#define CHECK(x) do{if(!(x)){printf("FAIL IRQ line %u\n",__LINE__);return 1;}}while(0)
int main(void)
{
 isp0.handle=&isp0;
 int result=0;unsigned rearm_deferred=0;
 for(unsigned tick=0;tick<100&&result==0;tick++){
  /* Disable asserts RWP on every write. Clearing it only between polls
   * exposes retries that accidentally issue disable again before checking. */
  rwp=0;
  if(irq_phase==1&&irq_index==1&&tick<9)active=1u<<25;else active=0;
  if(irq_phase==3&&irq_index==2&&!rearm_deferred){rwp=1;++rearm_deferred;}
  result=isp0_irq_arm();
 }
 CHECK(result==1&&irq_index==0&&irq_phase==0);
 CHECK(rearm_deferred==1);
 for(unsigned i=0;i<3;i++)CHECK(masks[i]==1&&clears[i]==1&&writes[i]==1&&enables[i]==1);
 CHECK(programmed[0]==0x2fe045&&programmed[1]==0x100&&programmed[2]==0x7f0e04);
 CHECK(!(programmed[2]&8)); /* LOAD cannot consume the SAVE interrupt. */
 isp0_dma_interrupt();isp0_vipp_interrupt();isp0_save_interrupt();
 CHECK(notified==3&&isp0_irq_healthy());
 for(unsigned i=0;i<3;i++)CHECK(masks[i]==2&&atomic_load(&isp0.interrupts[i])==1);
 isp0.waiting=1;isp0_wait();CHECK(waited==13);
 isp0.waiting=0;isp0_wait();CHECK(delayed==1);
 irq_bound=0;isp0_dma_interrupt();CHECK(!isp0_irq_healthy());
 puts("PASS ISP0 IRQ: deferred RWP, source ownership, notifications and deadline");return 0;
}
