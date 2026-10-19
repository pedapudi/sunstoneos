/* Single-thread application fixtures never execute kernel critical sections. */
__attribute__((weak)) void vTaskEnterCritical(void){}
__attribute__((weak)) void vTaskExitCritical(void){}
