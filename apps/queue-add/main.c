/* SPDX-License-Identifier: MIT */
#include "queue_add-app.h"
#include "gpu_queue.h"
#include "program.h"

#define ELEMENTS 65u
#define WAIT_TICKS 500u
struct queue_add_result { uint32_t elements, checksum, sequence, reserved; };

void queue_add_main(const struct ss_grant *grant)
{
    if (!(grant->devices & SS_DEVICE_GPU) || grant->region_count != 2 ||
        grant->regions[QUEUE_ADD_REGION_INPUT].bytes < ELEMENTS * 8u ||
        grant->regions[QUEUE_ADD_REGION_OUTPUT].bytes < ELEMENTS * 4u)
        ss_fail(1);
    uint32_t *input = grant->regions[QUEUE_ADD_REGION_INPUT].base;
    const volatile uint32_t *output = grant->regions[QUEUE_ADD_REGION_OUTPUT].base;
    for (unsigned i = 0; i < ELEMENTS; ++i) {
        input[i] = UINT32_MAX - 3u * i;
        input[ELEMENTS + i] = 7u * i + 9u;
    }
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
    ss_progress();
    if (gpu_queue_submit(context, &job) != GPU_QUEUE_OK) ss_fail(4);
    struct gpu_queue_record record;
    if (gpu_queue_wait(context, &record, WAIT_TICKS) != GPU_QUEUE_OK ||
        record.result != GPU_QUEUE_OK || record.sequence != job.sequence)
        ss_fail(5);
    uint32_t checksum = 0;
    for (unsigned i = 0; i < ELEMENTS; ++i) {
        const uint32_t value = output[i];
        if (value != 4u * i + 8u) ss_fail(6);
        checksum += value;
    }
    if (gpu_queue_close(context, WAIT_TICKS) != GPU_QUEUE_OK) ss_fail(7);
    const struct queue_add_result result = {ELEMENTS, checksum, record.sequence, 0};
    ss_finish(&result, sizeof(result));
}
