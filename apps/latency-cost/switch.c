/* Image-side probe glue; both comparison images count the same switch entry. */
#include "FreeRTOS.h"
#include "task.h"
extern void __real_vTaskSwitchContext(BaseType_t core);
extern void latency_cost_count_switch(unsigned core);
void __wrap_vTaskSwitchContext(BaseType_t core)
{
    latency_cost_count_switch((unsigned)core);
    __real_vTaskSwitchContext(core);
}
