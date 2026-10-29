#include "parallel_runtime.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "platform.h"
#include "mimus/asr/parallel.h"
#include <cstdio>
#ifdef SUNSTONE_A733_OS_CPU_RETURN
#include <atomic>
extern "C" uint64_t a733_smp_counter(void);
extern "C" uint64_t os_cpu_return_cutoff(void);
#endif
#ifdef SUNSTONE_A733_OS
#include "a733_smp_placement.h"
#endif
#ifdef SUNSTONE_MIMUS_NATIVE
#include "native.h"
#endif

namespace {
using mimus::asr::RangeFn;
using mimus::asr::TaskFn;
// A job is either a range of a ParallelFor (function) or one async item
// (task); exactly one of the two is set.
struct Job { RangeFn function; TaskFn task; void *context; size_t begin, end; uint64_t sequence; };
constexpr unsigned kWorkers = SUNSTONE_MIMUS_WORKERS;
static_assert(kWorkers == mimus::asr::kMaxWorkers);
static_assert(kWorkers == 2 || kWorkers == 8);
static_assert(kWorkers <= configNUMBER_OF_CORES);
constexpr unsigned worker_priority(unsigned core) {
#if defined(SUNSTONE_A733_OS) && SUNSTONE_MIMUS_WORKERS == 8
    // The OS PMIC owner on core 0 and NPU client on core 1 run at priority 3.
    if (core < 6) return 2;
#else
    (void)core;
#endif
    return kMimusOwnerPriority;
}
#ifdef SUNSTONE_A733_OS
constexpr unsigned inference_mask = kWorkers == 8 ? 0xffu :
    (1u << A733_SMP_RECOGNITION_CORE) | (1u << A733_SMP_INFERENCE_CORE);
// Read from the linked ELF by the OS link check and host preparation.
extern "C" __attribute__((used,retain)) const uint32_t mimus_parallel_configuration[7] = {
    1, kWorkers, A733_SMP_RECOGNITION_CORE, inference_mask, kMimusOwnerPriority,
    inference_mask & 0x3fu ? worker_priority(0) : 0,
    inference_mask & 0xc0u ? worker_priority(6) : 0
};
#endif
#if defined(SUNSTONE_MIMUS_NATIVE) && SUNSTONE_MIMUS_WORKERS == 8 && !defined(A733_A76_ADMITTED)
#error Eight native inference workers require admitted A76 placement
#endif
struct alignas(64) Worker {
    StaticQueue_t jobs_state, done_state;
    uint8_t jobs_storage[sizeof(Job)], done_storage[sizeof(uint64_t)];
    QueueHandle_t jobs, done;
    StaticTask_t state;
    unsigned index, core;
    uint64_t expected;
};
static_assert(sizeof(Worker) * (kWorkers - 1) < 16 * 1024);
Worker slots[kWorkers - 1];
alignas(64) StackType_t worker_stacks[kWorkers - 1][8192];
TaskHandle_t owner;
unsigned owner_core, completed_core_mask;
uint64_t submitted, completed, async_jobs;
bool busy, async_pending;
#ifdef SUNSTONE_A733_OS_CPU_RETURN
std::atomic<unsigned> parked{0};
void park_worker(void *) {
    parked.fetch_add(1, std::memory_order_release);
    for (;;) vTaskSuspend(nullptr);
}
#endif

void worker(void *argument) {
    auto &slot = *static_cast<Worker *>(argument);
    uint64_t previous = 0;
    for (;;) {
        Job job;
        configASSERT(xQueueReceive(slot.jobs, &job, portMAX_DELAY) == pdTRUE);
        configASSERT(portGET_CORE_ID() == slot.core);
#ifdef SUNSTONE_MIMUS_SUPERVISED
        mimus_native_checkpoint();
#endif
        configASSERT(job.sequence > previous);
        previous = job.sequence;
        if (job.task) job.task(job.context);
        else job.function(job.context, job.begin, job.end, slot.index);
#ifdef SUNSTONE_MIMUS_SUPERVISED
        mimus_native_checkpoint();
#endif
        // Publishing completion releases all callback stores. The caller
        // keeps the borrowed context and buffers alive until this message.
        configASSERT(xQueueSend(slot.done, &job.sequence, portMAX_DELAY) == pdTRUE);
    }
}
void dispatch(size_t count, RangeFn function, void *context, bool /*sync*/) {
    configASSERT(owner == xTaskGetCurrentTaskHandle() && !busy && !async_pending);
    configASSERT(portGET_CORE_ID() == owner_core);
#ifdef SUNSTONE_MIMUS_SUPERVISED
    mimus_native_checkpoint();
#endif
    busy = true;
    const unsigned active = count < kWorkers ? unsigned(count) : kWorkers;
    if (active < 2) {
        function(context, 0, count, 0);
    } else {
        // Preserve the original lower/upper half split with two workers.
        // Queue publication releases callback inputs; every private completion
        // acquires the corresponding stores before this stack context returns.
        auto boundary = [=](unsigned i) {
            return count / active * i + count % active * i / active;
        };
        for (unsigned i = 1; i < active; ++i) {
            auto &slot = slots[i - 1];
            configASSERT(submitted != UINT64_MAX);
            slot.expected = ++submitted;
            const Job job{function, nullptr, context, boundary(i),
                          boundary(i + 1), slot.expected};
            configASSERT(xQueueSend(slot.jobs, &job, 0) == pdTRUE);
        }
        function(context, 0, count / active, 0);
        for (unsigned i = 1; i < active; ++i) {
            auto &slot = slots[i - 1];
            uint64_t sequence;
            configASSERT(xQueueReceive(slot.done, &sequence, portMAX_DELAY) == pdTRUE);
            // Failure is terminal: never return a borrowed context early.
            configASSERT(sequence == slot.expected);
            ++completed;
            completed_core_mask |= 1u << slot.core;
        }
    }
    completed_core_mask |= 1u << owner_core;
#ifdef SUNSTONE_MIMUS_SUPERVISED
    mimus_native_checkpoint();
#endif
    busy = false;
#ifdef SUNSTONE_MIMUS_NATIVE
    mimus_native_inference_progress();
#endif
}
int workers() { return kWorkers; }

// The engine's two-core pipeline (attention scores/softmax split, input
// handoff) runs one item on the worker while the owner continues. While an
// item is pending the engine runs ParallelFor sequentially, so the worker
// queue never holds a range and an item at the same time. The caller keeps
// the item's context alive until async_wait, as for dispatch.
bool async_available() {
    return kWorkers == 2 && owner == xTaskGetCurrentTaskHandle() && !busy && !async_pending;
}
bool async_start(TaskFn task, void *context, bool /*sync*/) {
    if (!async_available()) return false;
    configASSERT(task && portGET_CORE_ID() == owner_core);
#ifdef SUNSTONE_MIMUS_SUPERVISED
    mimus_native_checkpoint();
#endif
    configASSERT(submitted != UINT64_MAX);
    auto &slot = slots[0];
    slot.expected = ++submitted;
    const Job job{nullptr, task, context, 0, 0, slot.expected};
    configASSERT(xQueueSend(slot.jobs, &job, 0) == pdTRUE);
    async_pending = true;
    ++async_jobs;
    return true;
}
void async_wait() {
    configASSERT(owner == xTaskGetCurrentTaskHandle() && async_pending && !busy);
    auto &slot = slots[0];
    uint64_t sequence;
    configASSERT(xQueueReceive(slot.done, &sequence, portMAX_DELAY) == pdTRUE);
    configASSERT(sequence == slot.expected);
    ++completed;
    completed_core_mask |= (1u << owner_core) | (1u << slot.core);
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
#ifdef SUNSTONE_A733_OS
    configASSERT(owner_index == A733_SMP_RECOGNITION_CORE && worker_index == A733_SMP_INFERENCE_CORE);
#elif defined(SUNSTONE_MIMUS_NATIVE) && SUNSTONE_MIMUS_WORKERS == 8
    configASSERT(owner_index == 6 && worker_index == 7);
#endif
    unsigned assigned = (1u << owner_index) | (1u << worker_index);
    for (unsigned i = 1; i < kWorkers; ++i) {
        auto &slot = slots[i - 1];
        slot.index = i;
        slot.core = worker_index;
        if (i > 1) {
            for (slot.core = 0; assigned & (1u << slot.core); ++slot.core) {}
            configASSERT(slot.core < configNUMBER_OF_CORES);
            assigned |= 1u << slot.core;
        }
        slot.jobs = xQueueCreateStatic(1, sizeof(Job), slot.jobs_storage, &slot.jobs_state);
        slot.done = xQueueCreateStatic(1, sizeof(uint64_t), slot.done_storage, &slot.done_state);
        configASSERT(slot.jobs && slot.done);
        configASSERT(xTaskCreateStaticAffinitySet(worker, "asr_worker", 8192, &slot, worker_priority(slot.core),
                    worker_stacks[i - 1], &slot.state, UBaseType_t{1} << slot.core));
    }
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
    snprintf(line, sizeof(line), "WORKERS count=%u jobs=%llu core_mask=%x\n", kWorkers,
             static_cast<unsigned long long>(completed), completed_core_mask);
    platform_puts(line);
    // Separate record: existing checks match the WORKERS line exactly.
    snprintf(line, sizeof(line), "WORKERS async=%llu\n",
             static_cast<unsigned long long>(async_jobs));
    platform_puts(line);
}
#ifdef SUNSTONE_A733_OS_CPU_RETURN
void mimus_parallel_stop() {
    configASSERT(owner == xTaskGetCurrentTaskHandle() && !busy && !async_pending &&
                 completed == submitted && !parked.load(std::memory_order_acquire));
    for (auto &slot : slots) {
        configASSERT(submitted != UINT64_MAX);
        const Job stop{nullptr, park_worker, nullptr, 0, 0, ++submitted};
        configASSERT(xQueueSend(slot.jobs, &stop, 0) == pdTRUE);
    }
    while (parked.load(std::memory_order_acquire) != kWorkers - 1) {
        configASSERT(a733_smp_counter() < os_cpu_return_cutoff());
        vTaskDelay(1);
    }
}
#endif
