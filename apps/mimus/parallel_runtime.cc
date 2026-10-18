#include "parallel_runtime.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "platform.h"
#include "mimus/asr/parallel.h"
#include <cstdio>
#ifdef SUNSTONE_MIMUS_NATIVE
#include "native.h"
#endif

namespace {
using mimus::asr::RangeFn;
struct Job { RangeFn function; void *context; size_t begin, end; uint64_t sequence; };
StaticQueue_t jobs_state, done_state;
uint8_t jobs_storage[sizeof(Job)], done_storage[sizeof(uint64_t)];
QueueHandle_t jobs, done;
StaticTask_t worker_state;
StackType_t worker_stack[8192];
TaskHandle_t owner;
uint64_t submitted, completed;
bool busy;

void worker(void *) {
    for (;;) {
        Job job;
        configASSERT(xQueueReceive(jobs, &job, portMAX_DELAY) == pdTRUE);
        configASSERT(portGET_CORE_ID() == 1);
#ifdef SUNSTONE_MIMUS_SUPERVISED
        mimus_native_checkpoint();
#endif
        job.function(job.context, job.begin, job.end, 1);
#ifdef SUNSTONE_MIMUS_SUPERVISED
        mimus_native_checkpoint();
#endif
        // Publishing completion releases all callback stores. The caller
        // keeps the borrowed context and buffers alive until this message.
        configASSERT(xQueueSend(done, &job.sequence, portMAX_DELAY) == pdTRUE);
    }
}
void dispatch(size_t count, RangeFn function, void *context, bool /*sync*/) {
    configASSERT(owner == xTaskGetCurrentTaskHandle() && !busy);
    configASSERT(portGET_CORE_ID() == 0);
#ifdef SUNSTONE_MIMUS_SUPERVISED
    mimus_native_checkpoint();
#endif
    busy = true;
    if (count < 2) {
        function(context, 0, count, 0);
    } else {
        const size_t middle = count / 2;
        const Job job{function, context, middle, count, ++submitted};
        // Cacheable, inner-shareable memory and the FreeRTOS queue barriers
        // publish CPU stores; this target has no noncoherent DMA producer.
        configASSERT(xQueueSend(jobs, &job, 0) == pdTRUE);
        function(context, 0, middle, 0);
        uint64_t sequence;
        configASSERT(xQueueReceive(done, &sequence, portMAX_DELAY) == pdTRUE);
        configASSERT(sequence == submitted);
        completed = sequence;
    }
#ifdef SUNSTONE_MIMUS_SUPERVISED
    mimus_native_checkpoint();
#endif
    busy = false;
#ifdef SUNSTONE_MIMUS_NATIVE
    mimus_native_inference_progress();
#endif
}
int workers() { return 2; }
}

void mimus_parallel_init() {
    jobs = xQueueCreateStatic(1, sizeof(Job), jobs_storage, &jobs_state);
    done = xQueueCreateStatic(1, sizeof(uint64_t), done_storage, &done_state);
    configASSERT(jobs && done);
    configASSERT(xTaskCreateStaticAffinitySet(worker, "asr_worker", 8192, nullptr, 3,
                                              worker_stack, &worker_state, 2));
}
void mimus_parallel_bind() {
    configASSERT(!owner && portGET_CORE_ID() == 0);
    owner = xTaskGetCurrentTaskHandle();
    mimus::asr::SetParallelFor(dispatch, workers);
}
void mimus_parallel_report() {
    configASSERT(owner == xTaskGetCurrentTaskHandle() && !busy && completed == submitted);
    char line[96];
    snprintf(line, sizeof(line), "WORKERS count=2 jobs=%llu core_mask=3\n",
             static_cast<unsigned long long>(completed));
    platform_puts(line);
}
