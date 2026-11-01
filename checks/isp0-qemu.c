#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
extern int isp0_rearm_check(unsigned);
static StaticTask_t task;
static StackType_t stack[2048];
static void check(void *unused)
{
 (void)unused;
 for(unsigned fault=0;fault<8;fault++){
  int line=isp0_rearm_check(fault);
  if(line)platform_panic("ISP0 retained rearm",(unsigned)line);
  vTaskDelay(1);
 }
 platform_puts("PASS ISP0: retained rearm and fault refusal under FreeRTOS\n");
 platform_exit(0);
}
void vApplicationStackOverflowHook(TaskHandle_t handle,char *name)
{(void)handle;platform_panic(name,0);}
void kernel_main(void)
{
 platform_init();
 configASSERT(xTaskCreateStatic(check,"isp0-check",2048,NULL,3,stack,&task));
 vTaskStartScheduler();platform_panic("ISP0 scheduler returned",0);
}
