/* SPDX-License-Identifier: GPL-2.0-only */
/* Host-only producer of a synthetic result for the layout check. */
#include "result.h"
#include <stdio.h>
#include <string.h>
int main(void)
{
    static struct dq_result r;
    memcpy(r.magic, DQ_MAGIC, 8);
    r.counter_hz = 24000000; r.buffer_base = 0x44000000u; r.load = 1; r.readiness = 15;
    snprintf(r.clock_before, sizeof r.clock_before, "CLOCK cpu_l_mhz=1794 cpu_b_mhz=2002 cpu_l_mv=1000 cpu_b_mv=1050 transitions=3 dsu_mhz=1274 sid=1 bin=4 core=6 pattern=0 parked_max_us=40\n");
    for (unsigned s = 0; s < DQ_SETTINGS; ++s)
        for (unsigned p = 0; p < DQ_PROBES; ++p) {
            struct dq_probe *q = &r.setting[s].probe[p];
            for (unsigned i = 0; i < 1000 + 10 * s; ++i) { const uint32_t c = 150 + (i * 37u) % 400 + (i % 997 == 0 ? 20000u : 0u); q->hist[dq_bin(c)]++; if (c > q->max_cycles) q->max_cycles = c; q->samples++; }
            q->overhead = 40;
        }
    r.setting[1].npu_jobs = 3; r.setting[1].npu_ticks_sum = 3 * 240000; r.setting[1].npu_ticks_max = 250000; r.setting[1].npu_ticks_min = 230000; r.setting[1].npu_ticks_window = 24000000;
    return fwrite(&r, sizeof r, 1, stdout) != 1;
}
