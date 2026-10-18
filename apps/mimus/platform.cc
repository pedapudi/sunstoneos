#include "FreeRTOS.h"
#include "task.h"
#include "timing.h"
#include "mimus/asr/platform.h"

namespace mimus::asr::platform {
uint64_t MonotonicNanos()
{
    uint64_t ticks = counter();
    return (ticks / COUNTER_HZ) * 1000000000ULL +
           (ticks % COUNTER_HZ) * 1000000000ULL / COUNTER_HZ;
}
uintptr_t TaskIdentity()
{
    return reinterpret_cast<uintptr_t>(xTaskGetCurrentTaskHandle());
}
}
