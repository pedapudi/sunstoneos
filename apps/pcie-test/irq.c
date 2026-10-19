#include "irq.h"
#include "gicv3_spi.h"
static const struct gicv3_spi source={0x03400000,185};
enum phase {UNBOUND,BOUND,ARMED,OBSERVED,CLOSED};
static struct {TaskHandle_t owner;enum phase phase;unsigned count,fault;} irq;
/* Bind precedes delivery. The existing SMP critical section serializes every
 * subsequent state access between the privileged owner and ISR. */
int pcie_test_irq_bind(TaskHandle_t owner)
{
 if(!owner||xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED||irq.phase!=UNBOUND||irq.fault)return 0;
 irq.owner=owner;irq.phase=BOUND;return 1;
}
static int owner(void)
{return irq.owner&&xTaskGetSchedulerState()==taskSCHEDULER_RUNNING&&xTaskGetCurrentTaskHandle()==irq.owner;}
int pcie_test_irq_arm(void)
{
 if(!owner())return -1;
 int result=-1;taskENTER_CRITICAL();
 if(!irq.fault&&irq.phase==ARMED)result=1;
 else if(!irq.fault&&irq.phase==BOUND){
  result=gicv3_spi_rearm(&source);
  if(result==1)irq.phase=ARMED;
  else if(result<0){irq.fault=1;irq.phase=CLOSED;}
 }
 taskEXIT_CRITICAL();return result;
}
void pcie_test_application_irq(void)
{
 UBaseType_t saved=taskENTER_CRITICAL_FROM_ISR();
 int masked=gicv3_spi_mask(&source);
 if(!masked||irq.phase!=ARMED||irq.count)irq.fault=1;
 if(irq.count<2)++irq.count;
 irq.phase=OBSERVED;taskEXIT_CRITICAL_FROM_ISR(saved);
}
int pcie_test_irq_event(uint32_t *notifications)
{
 if(!notifications||!owner())return -1;
 taskENTER_CRITICAL();*notifications=irq.count;
 int result=irq.fault?-1:irq.count==1;
 taskEXIT_CRITICAL();return result;
}
int pcie_test_irq_stop(void)
{
 if(!owner())return 0;
 taskENTER_CRITICAL();irq.phase=CLOSED;
 int result=gicv3_spi_mask(&source);if(!result)irq.fault=1;
 taskEXIT_CRITICAL();return result;
}
