/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "clock_api-app.h"
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#ifdef SUNSTONE_OWNER_TICKLESS
#include "a733_tickless.h"
#endif
#include <stdatomic.h>
#include <stdbool.h>

static _Atomic unsigned calls;
static _Atomic unsigned increment_request, increment_done;
void __real_vApplicationIRQHandler(uint64_t interrupt);
void __wrap_vApplicationIRQHandler(uint64_t interrupt)
{
    if (!interrupt && portGET_CORE_ID() == 0 && atomic_exchange(&increment_request, 0)) {
        UBaseType_t mask = portSET_INTERRUPT_MASK_FROM_ISR();
        TickType_t tick = xTaskGetTickCountFromISR();
#ifdef SUNSTONE_OWNER_TICKLESS
        uint64_t before = xSunstoneTicklessOffset();
#endif
        (void)xTaskIncrementTick();
        configASSERT(xTaskGetTickCountFromISR() >= tick + 1);
#ifdef SUNSTONE_OWNER_TICKLESS
        configASSERT(xSunstoneTicklessOffset() == before + 1);
#endif
        atomic_store_explicit(&increment_done, 1, memory_order_release);
        portCLEAR_INTERRUPT_MASK_FROM_ISR(mask);
        /* The ordinary SGI handler requires the exception-entry I mask. */
        __asm__ volatile("msr daifset, #2" ::: "memory");
    }
    __real_vApplicationIRQHandler(interrupt);
}
static void callback(TimerHandle_t timer) { (void)timer; atomic_fetch_add(&calls, 1); }
static void require(bool value, uint32_t detail) { if (!value) ss_fail(detail); }
void clock_api_main(const struct ss_grant *grant)
{
    (void)grant;
    atomic_store_explicit(&increment_request, 1, memory_order_release);
    portYIELD_CORE(0);
    for (unsigned waited = 0; !atomic_load_explicit(&increment_done, memory_order_acquire); ++waited) {
        require(waited < 100, 0x1d19);
        vTaskDelay(1);
    }
    static StaticTimer_t storage;
    TimerHandle_t timer = xTimerCreateStatic("clock-api", 5, pdTRUE, NULL, callback, &storage);
    require(timer && xTimerStart(timer, 0) == pdPASS, 0x1d10);
    vTaskDelay(30);
    require(atomic_load(&calls) >= 3, 0x1d11);
    TickType_t before = xTaskGetTickCount();
    (void)xTaskCatchUpTicks(17);
    require(xTaskGetTickCount() >= before + 17, 0x1d12);
    require(xTimerStop(timer, 0) == pdPASS, 0x1d13);
    vTaskDelay(5);
    unsigned previous = atomic_load(&calls);
    /* Cross the 32-bit tick view without wrapping the native 64-bit era. */
    TickType_t now = xTaskGetTickCount();
    require(now < UINT64_C(4294967301), 0x1d14);
    (void)xTaskCatchUpTicks(UINT64_C(4294967301) - now);
    require(xTaskGetTickCount() >= UINT64_C(4294967301), 0x1d15);
    require(xTimerStart(timer, 0) == pdPASS, 0x1d16);
    vTaskDelay(25);
    require(atomic_load(&calls) >= previous + 3, 0x1d17);
    require(xTimerStop(timer, 0) == pdPASS, 0x1d18);
    static const char message[] = "PASS scheduler explicit advance and auto-reload timer";
    ss_finish(message, sizeof(message) - 1);
}
