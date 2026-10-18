#include "replay_runtime.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "platform.h"
#include "mimus/asr/platform.h"
#include <cstdio>
#ifdef SUNSTONE_MIMUS_NATIVE
#include "native.h"
#endif

namespace {
struct Job { sunstone::Replay *replay; uint64_t start_ns; };
StaticQueue_t job_state, done_state;
uint8_t job_storage[sizeof(Job)], done_storage[sizeof(uint32_t)];
QueueHandle_t jobs, done;
StaticTask_t task_state;
StackType_t stack[4096];
void capture(void *) {
    for (;;) {
        Job job;
        configASSERT(xQueueReceive(jobs, &job, portMAX_DELAY) == pdTRUE);
#ifdef SUNSTONE_MIMUS_SUPERVISED
        mimus_native_checkpoint();
#endif
        while (job.replay->state() == sunstone::Replay::State::Running) {
#ifdef SUNSTONE_MIMUS_SUPERVISED
            mimus_native_checkpoint();
#endif
            uint64_t now = (mimus::asr::platform::MonotonicNanos() - job.start_ns) / 1000;
#ifdef SUNSTONE_MIMUS_NATIVE
            const auto previous_due = job.replay->next_due_us();
#endif
            job.replay->Pump(now);
#ifdef SUNSTONE_MIMUS_NATIVE
            if(job.replay->next_due_us()!=previous_due)mimus_native_input_progress();
#endif
            if (job.replay->state() != sunstone::Replay::State::Running) break;
            const uint64_t due = job.replay->next_due_us();
            // Absolute sample deadlines prevent per-frame scheduling drift.
            // Yield for at least one tick even while draining late frames.
            const uint32_t wait_ms = due > now ? uint32_t((due - now + 999) / 1000) : 1;
            vTaskDelay(pdMS_TO_TICKS(wait_ms));
        }
#ifdef SUNSTONE_MIMUS_SUPERVISED
        mimus_native_checkpoint();
#endif
        uint32_t finished = 1;
        // No use of the borrowed Replay object after publishing completion.
        configASSERT(xQueueSend(done, &finished, portMAX_DELAY) == pdTRUE);
    }
}
}
void mimus_replay_init() {
    jobs = xQueueCreateStatic(1, sizeof(Job), job_storage, &job_state);
    done = xQueueCreateStatic(1, sizeof(uint32_t), done_storage, &done_state);
    configASSERT(jobs && done);
#ifdef SUNSTONE_SMP_NEWLIB
    configASSERT(xTaskCreateStaticAffinitySet(capture, "replay", 4096, nullptr, 4,
                                              stack, &task_state, 2));
#else
    configASSERT(xTaskCreateStatic(capture, "replay", 4096, nullptr, 4, stack, &task_state));
#endif
}
bool mimus_replay_run(std::span<const int16_t> pcm, ReplayConsumer consumer, void *context) {
    sunstone::Replay replay(pcm);
    if (replay.state() != sunstone::Replay::State::Running) return false;
    Job job{&replay, mimus::asr::platform::MonotonicNanos()};
    configASSERT(xQueueSend(jobs, &job, 0) == pdTRUE);
    bool success = true;
    uint32_t consumed = 0;
    for (;;) {
#ifdef SUNSTONE_MIMUS_SUPERVISED
        mimus_native_checkpoint();
#endif
        const auto state = replay.state();
        if (state != sunstone::Replay::State::Running && state != sunstone::Replay::State::Complete) {
            success = false;
            break;
        }
        const auto segment = replay.Take();
#ifdef SUNSTONE_MIMUS_NATIVE
        mimus_native_waiting(!segment.has_value());
#endif
        if (segment) {
            if (!consumer(context, pcm.subspan(segment->begin, segment->end - segment->begin),
                          *segment, job.start_ns)) {
                success = false;
                replay.Cancel();
                break;
            }
            ++consumed;
        } else if (state == sunstone::Replay::State::Complete) break;
        else vTaskDelay(pdMS_TO_TICKS(1));
    }
#ifdef SUNSTONE_MIMUS_NATIVE
    mimus_native_waiting(0);
#endif
#ifdef SUNSTONE_MIMUS_SUPERVISED
    mimus_native_checkpoint();
#endif
    uint32_t finished;
    configASSERT(xQueueReceive(done, &finished, portMAX_DELAY) == pdTRUE);
#ifdef SUNSTONE_MIMUS_SUPERVISED
    mimus_native_checkpoint();
#endif
    success = success && replay.state() == sunstone::Replay::State::Complete;
    const uint64_t elapsed = (mimus::asr::platform::MonotonicNanos() - job.start_ns) / 1000;
    char line[224];
    snprintf(line, sizeof(line), "REPLAY samples=%u segments=%u consumed=%u max_lateness_us=%llu elapsed_us=%llu state=%u\n",
             replay.samples(), replay.segments(), consumed,
             static_cast<unsigned long long>(replay.max_lateness_us()),
             static_cast<unsigned long long>(elapsed), unsigned(replay.state()));
    platform_puts(line);
    if (!success) platform_puts("ERROR paced replay incomplete\n");
    return success;
}
