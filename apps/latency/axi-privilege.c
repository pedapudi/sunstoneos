/* SPDX-License-Identifier: GPL-2.0-only */
/* Image-side diagnostic hook of the stage 3a cap image: the CPU0 task that accesses the AXI2HIF bridges runs at EL1,
 * as the table-walks and interconnect studies do. The application keeps the public OS ABI. */
#include "axi-cap.h"
extern int __real_os_application_task_privileged(void (*)(void *), void *);
int __wrap_os_application_task_privileged(void (*entry)(void *), void *arg)
{
    return entry == latency_axi_worker || __real_os_application_task_privileged(entry, arg);
}
