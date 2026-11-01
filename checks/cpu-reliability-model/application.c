/* Model-only OS API fixtures. This executable is never a board image. */
#include "sunstone_app.h"
#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include <string.h>
static const struct ss_grant grant={.main_core=6,.worker_cores=0xbf,.finish_by_us=10000000};
static StaticTask_t main_state;
static StackType_t main_stack[2048];
void cpu_reliability_init(void);
void cpu_reliability_main(const struct ss_grant *);
const struct ss_grant *ss_grant(void){return &grant;}
int ss_live(void){return 1;}
void ss_progress(void){}
int ss_throttled(void){return 0;}
int ss_clock_ready(void){return 1;}
void ss_activity_begin(void){}
void ss_activity_end(int success){configASSERT(success);}
int ss_clock_info(char *line,size_t bytes)
{
    const char text[]="CLOCK cpu_l_mhz=1794 cpu_b_mhz=2002 cpu_l_mv=1000 cpu_b_mv=1050 transitions=3";
    configASSERT(bytes>=sizeof(text));memcpy(line,text,sizeof(text));return sizeof(text)-1;
}
_Noreturn void ss_fail(uint32_t detail){platform_exit(detail);for(;;);}
_Noreturn void ss_finish(const void *result,size_t bytes){(void)result;(void)bytes;platform_exit(0);for(;;);}
static void main_entry(void *unused){(void)unused;cpu_reliability_main(&grant);}
void a733_smp_diagnostic_start(void)
{
    configASSERT(xTaskCreateStaticAffinitySet(main_entry,"cpu-rel-main",2048,NULL,2,
        main_stack,&main_state,1UL<<6));
    cpu_reliability_init();
    vTaskStartScheduler();platform_exit(99);
}
void vApplicationStackOverflowHook(TaskHandle_t task,char *name)
{(void)task;platform_panic(name,2);}
