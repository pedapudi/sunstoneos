/* SPDX-License-Identifier: MIT
 * A modeled record of the accel-overlap counter build (apps/accel-overlap/result.h, struct accel_overlap_counter_result),
 * written through the C layout so that checks/accel-overlap-counters.py can decode it with apps/accel-overlap/host.py.
 * Every phase latches like the board: period k's count appears at its end and holds until the next. Job counts follow a
 * steady rate while each period's traffic grows by 0.1%, so consecutive periods differ. argv[1] is the
 * output path, argv[2] the form of the writes at 144 values per row ("A", "B", "C", or "none" for counts that fit no
 * form), argv[3] the factor on the 176 control's write units (1 repeats run 3bc9fbc0). */
#include "result.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PERIOD 2400000u
static struct accel_overlap_counter_result r;

struct traffic { double rq_rd, rq_wr, dt_rd, dt_wr; };
static void phase(unsigned i, uint32_t devices, uint32_t stream, uint32_t mode, double rate, int npu, struct traffic t)
{
    struct accel_overlap_large_phase *l = &r.large.phase[i];
    l->run.devices = devices; l->stream_mode = stream; l->sweep.stream_mask = stream & 0xffu;
    l->run.ticks = 13000000u;
    if (devices & ACCEL_OVERLAP_GPU) { l->slot_jobs = 4; l->run.gpu.pipelined_jobs = (uint32_t)(rate * 0.52); }
    if (devices & ACCEL_OVERLAP_NPU) l->run.npu.pipelined_jobs = (uint32_t)(rate * 0.52);
    if (devices & ACCEL_OVERLAP_STREAM) { l->run.sweeps = 40; l->run.stream_ticks = 12000000u; }
    struct accel_overlap_period_phase *p = &r.phase[i];
    p->mode = mode; p->samples = 2u * ACCEL_OVERLAP_PERIODS;
    p->begin[0][0] = 0; p->begin[0][1] = 240; p->begin[1][0] = 300; p->begin[1][1] = 540;
    p->start = 1000000u; p->first_begin = p->start + 480000u; p->end = p->first_begin + 5u * PERIOD + 100000u;
    const unsigned bank = npu ? 1u : 0u;
    double jobs_at = 0;
    uint32_t previous[6] = {0};
    for (unsigned k = 0; k < ACCEL_OVERLAP_PERIODS; ++k) {
        struct accel_overlap_period_sample *a = &p->pre[k], *b = &p->post[k];
        a->ticks = (k + 1u) * PERIOD - 72000u + 1000u;
        b->ticks = 540u + (k + 1u) * PERIOD + 48000u + 1000u;
        /* Each period differs a little from the one before, as measured periods do. */
        const double ja = rate * a->ticks / 24e6, jb = rate * b->ticks / 24e6, jobs = rate * 0.1 * (1.0 + 0.001 * k);
        jobs_at += jobs;
        *(npu ? &a->npu_jobs : &a->gpu_jobs) = (uint32_t)ja;
        *(npu ? &b->npu_jobs : &b->gpu_jobs) = (uint32_t)jb;
        const uint32_t now[6] = {(uint32_t)(t.rq_rd * jobs), (uint32_t)(t.rq_wr * jobs), (uint32_t)(t.dt_rd * jobs),
                                 (uint32_t)(t.dt_wr * jobs), (uint32_t)(t.rq_rd * jobs * 250), (uint32_t)(t.rq_wr * jobs * 48)};
        memcpy(a->value[bank], previous, sizeof(previous));
        memcpy(b->value[bank], now, sizeof(now));
        memcpy(previous, now, sizeof(now));
    }
    memcpy(p->value[bank], previous, sizeof(previous)); memcpy(p->held[bank], previous, sizeof(previous));
    (void)jobs_at;
}

int main(int argc, char **argv)
{
    if (argc != 4) return 2;
    const char *form = argv[2];
    const double control = atof(argv[3]);
    struct accel_overlap_large_result *g = &r.large;
    g->magic = ACCEL_OVERLAP_MAGIC; g->version = ACCEL_OVERLAP_COUNTERS_VERSION; g->bytes = sizeof(r);
    g->phases = ACCEL_OVERLAP_COUNTER_PHASES; g->initial_top = g->final_top = 1;
    g->gpu_elements = 262144; g->npu_tensor_bytes = 524288;
    phase(0, 0, 0, 0, 0, 0, (struct traffic){0});
    /* Both GPU modes and the NPU at 176, 128 and 256 values per row repeat run 3bc9fbc0 per job. */
    phase(1, ACCEL_OVERLAP_GPU, 0, 1, 699.9, 0, (struct traffic){102767, 32800, 201687, 131107});
    phase(2, ACCEL_OVERLAP_GPU, 0, 0, 1080.2, 0, (struct traffic){37219, 32797, 136118, 131096});
    phase(3, ACCEL_OVERLAP_NPU, 0, 0, 10189, 1, (struct traffic){3228, 2688, 36145, 32765 * control});
    phase(4, ACCEL_OVERLAP_NPU, 0, 1, 14654, 1, (struct traffic){3100, 2048, 24884, 20480});
    /* At 144: A writes 3 requests of 10 units per row, B one request of 10 units per 64 values, C 32 units per row;
     * "none" scales the 176 counts by 144 / 176. The rate follows the job-time fit, 14.3 us + 2.63 ns per write unit. */
    double rq = 2688.0 * 144 / 176, dt = 32765.0 * 144 / 176;
    if (!strcmp(form, "A")) { rq = 3072; dt = 30720; }
    else if (!strcmp(form, "B")) { rq = 2304; dt = 23040; }
    else if (!strcmp(form, "C")) { rq = 2690; dt = 32768; }
    phase(5, ACCEL_OVERLAP_NPU, 0, 2, 1e6 / (14.3 + 0.00263 * dt), 1, (struct traffic){3100, rq, 28000, dt});
    phase(6, ACCEL_OVERLAP_NPU, 0, 3, 8189, 1, (struct traffic){5149, 4097, 45373, 40968});
    phase(7, ACCEL_OVERLAP_STREAM, 0x01, 0, 0, 0, (struct traffic){0});
    FILE *f = fopen(argv[1], "wb");
    if (!f || fwrite(&r, sizeof(r), 1, f) != 1 || fclose(f)) return 1;
    return 0;
}
