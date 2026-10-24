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
using mimus::asr::TaskFn;
// A job is either a range of a ParallelFor (function) or one async item
// (task); exactly one of the two is set.
struct Job { RangeFn function; TaskFn task; void *context; size_t begin, end; uint64_t sequence; };
StaticQueue_t jobs_state, done_state;
uint8_t jobs_storage[sizeof(Job)], done_storage[sizeof(uint64_t)];
QueueHandle_t jobs, done;
StaticTask_t worker_state;
StackType_t worker_stack[8192];
TaskHandle_t owner;
// Cores of the owner (the recognition task) and of the worker.
unsigned owner_core, worker_core = 1;
uint64_t submitted, completed, async_jobs;
bool busy, async_pending;

void worker(void *) {
    for (;;) {
        Job job;
        configASSERT(xQueueReceive(jobs, &job, portMAX_DELAY) == pdTRUE);
        configASSERT(portGET_CORE_ID() == worker_core);
#ifdef SUNSTONE_MIMUS_SUPERVISED
        mimus_native_checkpoint();
#endif
        if (job.task) job.task(job.context);
        else job.function(job.context, job.begin, job.end, 1);
#ifdef SUNSTONE_MIMUS_SUPERVISED
        mimus_native_checkpoint();
#endif
        // Publishing completion releases all callback stores. The caller
        // keeps the borrowed context and buffers alive until this message.
        configASSERT(xQueueSend(done, &job.sequence, portMAX_DELAY) == pdTRUE);
    }
}
void dispatch(size_t count, RangeFn function, void *context, bool /*sync*/) {
    configASSERT(owner == xTaskGetCurrentTaskHandle() && !busy && !async_pending);
    configASSERT(portGET_CORE_ID() == owner_core);
#ifdef SUNSTONE_MIMUS_SUPERVISED
    mimus_native_checkpoint();
#endif
    busy = true;
    if (count < 2) {
        function(context, 0, count, 0);
    } else {
        const size_t middle = count / 2;
        const Job job{function, nullptr, context, middle, count, ++submitted};
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

// The engine's two-core pipeline (attention scores/softmax split, input
// handoff) runs one item on the worker while the owner continues. While an
// item is pending the engine runs ParallelFor sequentially, so the worker
// queue never holds a range and an item at the same time. The caller keeps
// the item's context alive until async_wait, as for dispatch.
bool async_available() {
    return owner == xTaskGetCurrentTaskHandle() && !busy && !async_pending;
}
bool async_start(TaskFn task, void *context, bool /*sync*/) {
    if (!async_available()) return false;
    configASSERT(task && portGET_CORE_ID() == owner_core);
#ifdef SUNSTONE_MIMUS_SUPERVISED
    mimus_native_checkpoint();
#endif
    const Job job{nullptr, task, context, 0, 0, ++submitted};
    configASSERT(xQueueSend(jobs, &job, 0) == pdTRUE);
    async_pending = true;
    ++async_jobs;
    return true;
}
void async_wait() {
    configASSERT(owner == xTaskGetCurrentTaskHandle() && async_pending && !busy);
    uint64_t sequence;
    configASSERT(xQueueReceive(done, &sequence, portMAX_DELAY) == pdTRUE);
    configASSERT(sequence == submitted);
    completed = sequence;
    async_pending = false;
#ifdef SUNSTONE_MIMUS_SUPERVISED
    mimus_native_checkpoint();
#endif
}
}

void mimus_parallel_init(unsigned owner_index, unsigned worker_index) {
    configASSERT(owner_index != worker_index && owner_index < configNUMBER_OF_CORES &&
                 worker_index < configNUMBER_OF_CORES);
    owner_core = owner_index;
    worker_core = worker_index;
    jobs = xQueueCreateStatic(1, sizeof(Job), jobs_storage, &jobs_state);
    done = xQueueCreateStatic(1, sizeof(uint64_t), done_storage, &done_state);
    configASSERT(jobs && done);
    configASSERT(xTaskCreateStaticAffinitySet(worker, "asr_worker", 8192, nullptr, 3,
                                              worker_stack, &worker_state,
                                              UBaseType_t{1} << worker_core));
}
void mimus_parallel_bind() {
    configASSERT(!owner && portGET_CORE_ID() == owner_core);
    owner = xTaskGetCurrentTaskHandle();
    mimus::asr::SetParallelFor(dispatch, workers);
#ifndef SUNSTONE_MIMUS_NO_ASYNC
    mimus::asr::SetAsync(async_start, async_wait, async_available);
#endif
}
void mimus_parallel_report() {
    configASSERT(owner == xTaskGetCurrentTaskHandle() && !busy && !async_pending &&
                 completed == submitted);
    char line[96];
    snprintf(line, sizeof(line), "WORKERS count=2 jobs=%llu core_mask=%x\n",
             static_cast<unsigned long long>(completed), (1u << owner_core) | (1u << worker_core));
    platform_puts(line);
    // Separate record: existing checks match the WORKERS line exactly.
    snprintf(line, sizeof(line), "WORKERS async=%llu\n",
             static_cast<unsigned long long>(async_jobs));
    platform_puts(line);
}
