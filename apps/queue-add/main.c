/* SPDX-License-Identifier: MIT */
#include "queue_add-app.h"
#include "gpu_queue.h"
#include "program.h"

#define ELEMENTS 65u
#define WAIT_TICKS 500u
#ifndef QUEUE_ADD_JOBS
#define QUEUE_ADD_JOBS 1u
#endif
#define MAX_JOBS 8u
_Static_assert(QUEUE_ADD_JOBS >= 1 && QUEUE_ADD_JOBS <= MAX_JOBS, "one to eight serial jobs");
struct queue_add_result {
    uint32_t jobs, elements, checksum, sequence;
    uint32_t kick_to_done_us[MAX_JOBS];
};
_Static_assert(sizeof(struct queue_add_result) == 48, "queue-add result fits the manifest");

void queue_add_main(const struct ss_grant *grant)
{
    if (!(grant->devices & SS_DEVICE_GPU) || grant->region_count != 2 ||
        grant->regions[QUEUE_ADD_REGION_INPUT].bytes < ELEMENTS * 8u ||
        grant->regions[QUEUE_ADD_REGION_OUTPUT].bytes < ELEMENTS * 4u)
        ss_fail(1);
    uint32_t *input = grant->regions[QUEUE_ADD_REGION_INPUT].base;
    const volatile uint32_t *output = grant->regions[QUEUE_ADD_REGION_OUTPUT].base;
    const struct gpu_queue_context_config config = {
        .job_us = 200000, .program = &gpu_queue_program, .lease_count = 2,
        .lease = {{.region = QUEUE_ADD_REGION_OUTPUT}, {.region = QUEUE_ADD_REGION_INPUT}}
    };
    struct gpu_queue_context context;
    struct gpu_queue_lease_info info[2];
    if (gpu_queue_context_create(&config, WAIT_TICKS, &context, info) != GPU_QUEUE_OK)
        ss_fail(2);
    if (info[0].address != (uintptr_t)output || info[1].address != (uintptr_t)input)
        ss_fail(3);
    struct gpu_queue_job job = {
        .sequence = 1, .bindings = 2, .argument_bytes = sizeof(uint32_t), .groups = {3, 1, 1},
        .binding = {{.lease = 0, .sync_bytes = ELEMENTS * 4u},
                    {.lease = 1, .sync_bytes = ELEMENTS * 8u}}
    };
    const uint32_t count = ELEMENTS;
    for (unsigned i = 0; i < sizeof(count); ++i)
        job.arguments[i] = (unsigned char)(count >> (8u * i));
    struct queue_add_result result = {.elements = ELEMENTS};
    for (uint32_t sequence = 1; sequence <= QUEUE_ADD_JOBS; ++sequence) {
        /* Rotating each vector changes the expected output every job while
         * retaining the same checksum. A preceding job's output must fail. */
        for (unsigned i = 0; i < ELEMENTS; ++i) {
            const unsigned at = (i + sequence - 1) % ELEMENTS;
            input[i] = UINT32_MAX - 3u * at;
            input[ELEMENTS + i] = 7u * at + 9u;
        }
        job.sequence = sequence;
        ss_progress();
        const int submitted = gpu_queue_submit(context, &job);
        /* Preserve the job and public refusal result in the retained fault. */
        if (submitted != GPU_QUEUE_OK) ss_fail(0x400u | (sequence << 4) | (unsigned)submitted);
        struct gpu_queue_record record;
        if (gpu_queue_wait(context, &record, WAIT_TICKS) != GPU_QUEUE_OK ||
            record.result != GPU_QUEUE_OK || record.sequence != job.sequence)
            ss_fail(5);
        uint32_t checksum = 0;
        for (unsigned i = 0; i < ELEMENTS; ++i) {
            const uint32_t value = output[i];
            if (value != 4u * ((i + sequence - 1) % ELEMENTS) + 8u) ss_fail(6);
            checksum += value;
        }
        result.jobs = sequence;
        result.sequence = record.sequence;
        result.checksum = checksum;
        result.kick_to_done_us[sequence - 1] = record.kick_to_done_us;
    }
    if (gpu_queue_close(context, WAIT_TICKS) != GPU_QUEUE_OK) ss_fail(7);
    ss_finish(&result, sizeof(result));
}
