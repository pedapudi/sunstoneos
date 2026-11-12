/* SPDX-License-Identifier: BSD-3-Clause */
/* Returning control: run the checksum at the granted clocks inside the PMU window. */
#include "sunstone_app.h"
#include "FreeRTOS.h"
#include "task.h"
extern void __real_hello_compute_main(const struct ss_grant *);
void __wrap_hello_compute_main(const struct ss_grant *grant)
{
    while (!ss_clock_ready() || ss_throttled()) {
        if (ss_live() < 0) ss_fail(0x7a10);
        ss_progress();
        vTaskDelay(1);
    }
    ss_activity_begin();
    __real_hello_compute_main(grant);
}
