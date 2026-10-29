#include "irq.h"
#include "pcie.h"
#include "gicv3_spi.h"
static const struct gicv3_spi source={0x03400000,185};
enum phase {UNBOUND,BOUND,ARMED,OBSERVED,CLOSED};
static struct {TaskHandle_t owner;enum phase phase;unsigned count,fault;} irq;
int pcie_nvme_irq_bind(TaskHandle_t owner)
{
 if(!owner||xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED||irq.phase!=UNBOUND)return 0;
 irq.owner=owner;irq.phase=BOUND;return 1;
}
static int owner(void)
{return irq.owner&&xTaskGetSchedulerState()==taskSCHEDULER_RUNNING&&xTaskGetCurrentTaskHandle()==irq.owner;}
int pcie_nvme_irq_health(void)
{
 if(!owner())return 0;
 taskENTER_CRITICAL();int healthy=!irq.fault;taskEXIT_CRITICAL();return healthy;
}
static int arm(void *cookie)
{
 (void)cookie;if(!owner())return -1;
 taskENTER_CRITICAL();int result=-1;
 if(!irq.fault&&irq.phase==ARMED)result=1;
 else if(!irq.fault&&irq.phase==BOUND){
  result=gicv3_spi_rearm(&source);
  if(result==1)irq.phase=ARMED;
  else if(result<0){irq.fault=1;irq.phase=CLOSED;}
 }
 taskEXIT_CRITICAL();return result;
}
void pcie_nvme_application_irq(void)
{
 BaseType_t woken=pdFALSE;UBaseType_t saved=taskENTER_CRITICAL_FROM_ISR();
 int masked=gicv3_spi_mask(&source);
 if(!masked||!(irq.phase==ARMED||irq.phase==OBSERVED)){
  irq.fault=1;irq.phase=CLOSED;pcie_cancel();
 }else if(irq.phase==ARMED){irq.count=1;irq.phase=OBSERVED;}
 /* Repeated delivery while disable propagates is the same wake hint. */
 if(irq.owner)vTaskNotifyGiveFromISR(irq.owner,&woken);
 taskEXIT_CRITICAL_FROM_ISR(saved);portYIELD_FROM_ISR(woken);
}
static int event(void *cookie,uint32_t *count)
{
 (void)cookie;if(!count||!owner())return -1;
 taskENTER_CRITICAL();
 *count=irq.count;irq.count=0;
 /* function_begin has acknowledged the controller before this callback.
  * Never clear GIC pending: a newly arriving MSI must survive rearming. */
 int result=irq.fault||irq.phase==CLOSED?-1:0;
 if(result>=0&&(irq.phase==BOUND||irq.phase==OBSERVED)){
  result=gicv3_spi_rearm(&source);
  if(result==1)irq.phase=ARMED;
  else if(result<0){irq.fault=1;irq.phase=CLOSED;}
 }
 taskEXIT_CRITICAL();return result;
}
static void stop(void *cookie)
{
 (void)cookie;
 if(!owner())return;
 taskENTER_CRITICAL();irq.phase=CLOSED;
 if(!gicv3_spi_mask(&source))irq.fault=1;
 taskEXIT_CRITICAL();
}
const struct a733_pcie_irq_ops pcie_nvme_irq_ops={.arm=arm,.event=event,.stop=stop};
