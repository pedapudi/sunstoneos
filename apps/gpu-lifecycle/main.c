/* SPDX-License-Identifier: MIT */
#include "gpu_lifecycle-app.h"
#include "gpu_queue.h"
#include "../queue-add/program.h"
#include "FreeRTOS.h"
#include "task.h"

#define ELEMENTS 65u
#define WAIT_TICKS 4000u
#define CYCLES 10u
#define SUBMIT_WAIT_TICKS 500u
#ifndef GPU_LIFECYCLE_RECOVERY
#define GPU_LIFECYCLE_RECOVERY 0
#endif
#ifndef GPU_LIFECYCLE_PEER
#define GPU_LIFECYCLE_PEER 0
#endif

/* The fixed peer application is linked only into the sharing image. */
#if GPU_LIFECYCLE_PEER
#include "peer.h"
GPU_QUEUE_PEER_COMPOSITION(gpu_lifecycle_manifest, gpu_queue_program);
#endif

struct lifecycle_result {
    uint32_t magic, mode, opens, closes, jobs, sequence, checksum;
    uint32_t fault_result, fault_detail, recoveries, peer_jobs, elements;
    uint32_t kick_to_done_us[CYCLES];
};
_Static_assert(sizeof(struct lifecycle_result) == 88, "lifecycle result matches the manifest");

static int submit_ready(struct gpu_queue_context context, const struct gpu_queue_job *job)
{
    const TickType_t began = xTaskGetTickCount();
    int result;
    /* Thermal parking queues no job. Allow bounded startup or reopen settling
     * while the thermal owner runs; other results still stop the diagnostic. */
    while ((result = gpu_queue_submit(context, job)) == GPU_QUEUE_PARKED) {
        if (ss_live() < 0 || xTaskGetTickCount() - began >= SUBMIT_WAIT_TICKS) break;
        ss_progress();
        vTaskDelay(1);
    }
    return result;
}

static void inputs(uint32_t *input, uint32_t sequence)
{
    for (unsigned i = 0; i < ELEMENTS; ++i) {
        const unsigned at = (i + sequence - 1) % ELEMENTS;
        input[i] = UINT32_MAX - 3u * at;
        input[ELEMENTS + i] = 7u * at + 9u;
    }
}
static uint32_t checked_output(const volatile uint32_t *output, uint32_t sequence)
{
    uint32_t checksum = 0;
    for (unsigned i = 0; i < ELEMENTS; ++i) {
        const uint32_t value = output[i];
        if (value != 4u * ((i + sequence - 1) % ELEMENTS) + 8u) ss_fail(6);
        checksum += value;
    }
    return checksum;
}

void gpu_lifecycle_main(const struct ss_grant *grant)
{
    if (!(grant->devices & SS_DEVICE_GPU) || grant->region_count != 2 ||
        grant->regions[GPU_LIFECYCLE_REGION_INPUT].bytes < ELEMENTS * 8u ||
        grant->regions[GPU_LIFECYCLE_REGION_OUTPUT].bytes < ELEMENTS * 4u)
        ss_fail(1);
    uint32_t *input = grant->regions[GPU_LIFECYCLE_REGION_INPUT].base;
    const volatile uint32_t *output = grant->regions[GPU_LIFECYCLE_REGION_OUTPUT].base;
    const struct gpu_queue_context_config config = {
        .job_us = 200000, .program = &gpu_queue_program, .lease_count = 2,
        .lease = {{.region = GPU_LIFECYCLE_REGION_OUTPUT}, {.region = GPU_LIFECYCLE_REGION_INPUT}}
    };
    struct gpu_queue_context context;
    struct gpu_queue_lease_info info[2];
    if (gpu_queue_context_create(&config, WAIT_TICKS, &context, info) != GPU_QUEUE_OK) ss_fail(2);
    if (info[0].address != (uintptr_t)output || info[1].address != (uintptr_t)input) ss_fail(3);
    struct gpu_queue_job job = {
        .bindings = 2, .argument_bytes = sizeof(uint32_t), .groups = {3, 1, 1},
        .binding = {{.lease = 0, .sync_bytes = ELEMENTS * 4u},
                    {.lease = 1, .sync_bytes = ELEMENTS * 8u}}
    };
    job.arguments[0] = ELEMENTS;
    struct lifecycle_result result = {
        .magic = 0x314c5047, .mode = GPU_LIFECYCLE_RECOVERY ? 2 : 1,
        .opens = 1, .elements = ELEMENTS
    };
#if GPU_LIFECYCLE_RECOVERY
    inputs(input, 1);
    job.sequence = 1;
    ss_progress();
    if (submit_ready(context, &job) != GPU_QUEUE_OK) ss_fail(4);
#if GPU_LIFECYCLE_PEER
    gpu_queue_peer_release();
    if (!gpu_queue_peer_accepted(50)) ss_fail(12);
#endif
    struct gpu_queue_record fault;
    if (gpu_queue_wait(context, &fault, WAIT_TICKS) != GPU_QUEUE_FAULT ||
        fault.result != GPU_QUEUE_FAULT || fault.detail != GPU_QUEUE_HANG || fault.sequence != 1)
        ss_fail(9);
    result.fault_result = fault.result;
    result.fault_detail = fault.detail;
    ss_progress();
    if (gpu_queue_recover(context, WAIT_TICKS) != GPU_QUEUE_OK) ss_fail(10);
    result.recoveries = 1;
    ++result.opens;
#if GPU_LIFECYCLE_PEER
    if (!gpu_queue_peer_wait(WAIT_TICKS)) ss_fail(11);
    result.peer_jobs = 1;
#endif
    const uint32_t first = 2, last = 2;
#else
    const uint32_t first = 1, last = CYCLES;
#endif
    for (uint32_t sequence = first; sequence <= last; ++sequence) {
        inputs(input, sequence);
        job.sequence = sequence;
        ss_progress();
        if (submit_ready(context, &job) != GPU_QUEUE_OK) ss_fail(4);
#if GPU_LIFECYCLE_PEER && !GPU_LIFECYCLE_RECOVERY
        if (sequence == 1) gpu_queue_peer_release();
#endif
        struct gpu_queue_record record;
        if (gpu_queue_wait(context, &record, WAIT_TICKS) != GPU_QUEUE_OK ||
            record.result != GPU_QUEUE_OK || record.sequence != sequence) ss_fail(5);
        result.checksum = checked_output(output, sequence);
        result.kick_to_done_us[result.jobs++] = record.kick_to_done_us;
        result.sequence = sequence;
#if GPU_LIFECYCLE_PEER && !GPU_LIFECYCLE_RECOVERY
        if (sequence == 1) {
            if (!gpu_queue_peer_wait(WAIT_TICKS)) ss_fail(11);
            result.peer_jobs = 1;
        }
#endif
        ss_progress();
        if (gpu_queue_close(context, WAIT_TICKS) != GPU_QUEUE_OK) ss_fail(7);
        ++result.closes;
        if (sequence != last) {
            ss_progress();
            if (gpu_queue_open(context, WAIT_TICKS) != GPU_QUEUE_OK) ss_fail(8);
            ++result.opens;
        }
    }
    ss_finish(&result, sizeof(result));
}
