#include "application.h"
int a733_storage_native_task_privileged(void (*entry)(void *),void *context)
{return gpu_display_task_privileged(entry,context);}
void a733_storage_maintenance_stop(void)
{hdmi_fail(NULL);}
