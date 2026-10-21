/* Actual private IRQ state machine; GIC writes and RTOS serialization modeled.
 * No CPU startup, exception delivery or PCIe hardware execution. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define SUNSTONE_PCIE_TEST_IRQ_H
#define taskSCHEDULER_NOT_STARTED 0
#define taskSCHEDULER_RUNNING 2
typedef void *TaskHandle_t;
typedef unsigned UBaseType_t;
static unsigned critical, scheduler, masks;
static int enable_result=1, mask_result=1;
static TaskHandle_t current;
static unsigned xTaskGetSchedulerState(void){return scheduler;}
static TaskHandle_t xTaskGetCurrentTaskHandle(void){return current;}
static void enter(void){assert(!critical);critical=1;}
static void leave(void){assert(critical);critical=0;}
#define taskENTER_CRITICAL() enter()
#define taskEXIT_CRITICAL() leave()
#define taskENTER_CRITICAL_FROM_ISR() (enter(),0u)
#define taskEXIT_CRITICAL_FROM_ISR(saved) do{assert((saved)==0);leave();}while(0)
#include "gicv3_spi.h"
int gicv3_spi_mask(const struct gicv3_spi *spi)
{assert(critical&&spi->intid==185&&spi->distributor==0x03400000);++masks;return mask_result;}
int gicv3_spi_rearm(const struct gicv3_spi *spi)
{assert(critical&&spi->intid==185);return enable_result;}
#include "../apps/pcie-test/irq.c"
static void fresh(void)
{
 memset(&irq,0,sizeof(irq));critical=scheduler=masks=0;enable_result=mask_result=1;
 current=(TaskHandle_t)(uintptr_t)0x1234;
 assert(!pcie_test_irq_bind(NULL));assert(pcie_test_irq_bind(current));
 assert(!pcie_test_irq_bind(current));scheduler=taskSCHEDULER_RUNNING;
}
int main(void)
{
 uint32_t count=99;
 fresh();assert(pcie_test_irq_event(&count)==0&&count==0);
 enable_result=0;assert(pcie_test_irq_arm()==0);assert(pcie_test_irq_event(&count)==0);
 enable_result=1;assert(pcie_test_irq_arm()==1);
 pcie_test_application_irq();assert(masks==1);assert(pcie_test_irq_event(&count)==1&&count==1);
 /* Level re-delivery before the disable propagates is re-masked, not counted. */
 pcie_test_application_irq();assert(masks==2);assert(pcie_test_irq_event(&count)==1&&count==1);
 assert(pcie_test_irq_arm()==-1);assert(pcie_test_irq_stop());assert(pcie_test_irq_arm()==-1);
 assert(pcie_test_irq_event(&count)==1&&count==1);
 pcie_test_application_irq();assert(pcie_test_irq_event(&count)==-1&&count==2);
 fresh();current=(TaskHandle_t)(uintptr_t)0x5678;count=99;
 assert(pcie_test_irq_arm()==-1&&!pcie_test_irq_stop()&&pcie_test_irq_event(&count)==-1&&count==99);
 current=(TaskHandle_t)(uintptr_t)0x1234;assert(pcie_test_irq_arm()==1);
 fresh();pcie_test_application_irq();assert(pcie_test_irq_event(&count)==-1);assert(pcie_test_irq_arm()==-1);
 fresh();assert(pcie_test_irq_stop());assert(pcie_test_irq_arm()==-1);assert(pcie_test_irq_event(&count)==0);
 fresh();enable_result=-1;assert(pcie_test_irq_arm()==-1);assert(pcie_test_irq_event(&count)==-1);
 fresh();assert(pcie_test_irq_arm()==1);mask_result=0;pcie_test_application_irq();
 assert(pcie_test_irq_event(&count)==-1&&count==1);assert(!pcie_test_irq_stop());
 fresh();assert(pcie_test_irq_arm()==1);pcie_test_application_irq();mask_result=0;pcie_test_application_irq();
 assert(pcie_test_irq_event(&count)==-1&&count==2);
 puts("PASS pending arm, one event, owner refusal, permanent stop, benign level re-delivery, unexpected IRQ and mask/enable faults");
}
