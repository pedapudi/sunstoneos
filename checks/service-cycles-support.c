/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Phase bounds for checks/scheduler-cause.py, which writes them before each case (runtime/latency.c owns them in an image). */
#include <stdint.h>
uint64_t ss_service_cycles_test_bounds[3][2];
void ss_latency_phase_bounds(uint64_t out[3][2])
{
    for (unsigned p = 0; p < 3; ++p) {
        out[p][0] = ss_service_cycles_test_bounds[p][0];
        out[p][1] = ss_service_cycles_test_bounds[p][1];
    }
}
