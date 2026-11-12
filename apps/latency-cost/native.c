/* Force two switches per round trip, including exception save and restore. */
#include "sunstone_app.h"
#include "sunstone_latency.h"
#include "FreeRTOS.h"
#include "task.h"
#include "result.h"
#include <stdatomic.h>

static struct cost_result result;
struct cost_irq_control latency_cost_irq;
static StaticTask_t sender_state[8], receiver_state[8];
static StackType_t sender_stack[8][1024], receiver_stack[8][1024];
static TaskHandle_t sender_task[8], receiver_task[8];
static struct { _Alignas(64) volatile uint64_t value; } switch_count[8];
static atomic_uint done[8], received[8];

void latency_cost_count_switch(unsigned core)
{
    ++switch_count[core].value;
}
static uint64_t now(void)
{
    uint64_t v;
    __asm__ volatile("isb\n mrs %0,cntvct_el0" : "=r"(v) :: "memory");
    return v;
}
static void receiver(void *argument)
{
    const unsigned c = (unsigned)(uintptr_t)argument;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        configASSERT(portGET_CORE_ID() == c);
        atomic_fetch_add_explicit(&received[c], 1, memory_order_relaxed);
    }
}
static void sender(void *argument)
{
    const unsigned c = (unsigned)(uintptr_t)argument;
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    /* The higher-priority receiver runs immediately and blocks before each
     * notification returns. No user callback executes in the IRQ recorder. */
    unsigned expected = 0;
    for (unsigned sample = 0; sample < COST_SAMPLES; ++sample) {
        const TickType_t tick = xTaskGetTickCount();
        const uint64_t switches = switch_count[c].value;
        const uint64_t start = now();
        for (unsigned i = 0; i < COST_ROUNDTRIPS; ++i) {
            xTaskNotifyGive(receiver_task[c]);
            ++expected;
            configASSERT(atomic_load_explicit(&received[c], memory_order_relaxed) == expected);
        }
        result.core[c].ticks[sample] = now() - start;
        result.core[c].switches[sample] = switch_count[c].value - switches;
        if (xTaskGetTickCount() != tick) result.core[c].timer_mask |= UINT64_C(1) << sample;
    }
    atomic_store_explicit(&done[c], 1, memory_order_release);
    vTaskSuspend(NULL);
    configASSERT(0);
}
void latency_cost_init(void)
{
    const struct ss_grant *grant = ss_grant();
    if (!grant || grant->main_core != 6 || grant->worker_cores != 0xbf) ss_fail(0xc01);
    __asm__ volatile("mrs %0,cntfrq_el0" : "=r"(result.frequency));
#ifdef SUNSTONE_LATENCY
    result.enabled = 1;
    ss_latency_init(result.frequency);
#endif
    for (unsigned c = 0; c < 8; ++c) {
        receiver_task[c] = ss_task_create_static_affinity_set(receiver, "cost-receive", 1024,
            (void *)(uintptr_t)c, 4, receiver_stack[c], &receiver_state[c], 1u << c);
        sender_task[c] = ss_task_create_static_affinity_set(sender, "cost-send", 1024,
            (void *)(uintptr_t)c, 3, sender_stack[c], &sender_state[c], 1u << c);
        configASSERT(receiver_task[c] && sender_task[c]);
    }
}
static int at_top(void)
{
    char clock[256];
    const char prefix[] = "CLOCK cpu_l_mhz=1794 cpu_b_mhz=2002 cpu_l_mv=1000 cpu_b_mv=1050 ";
    if (ss_clock_info(clock, sizeof(clock)) < (int)(sizeof(prefix) - 1)) return 0;
    for (unsigned i = 0; i < sizeof(prefix) - 1; ++i) if (clock[i] != prefix[i]) return 0;
    return 1;
}
void latency_cost_main(const struct ss_grant *grant)
{
    (void)grant;
    while (!ss_clock_ready() || ss_throttled()) {
        if (ss_live() < 0) ss_fail(0xc02);
        ss_progress(); vTaskDelay(1);
    }
    if (!at_top()) ss_fail(0xc03);
    result.counter_pair_min = UINT64_MAX;
    for (unsigned i = 0; i < 128; ++i) {
        uint64_t start = now(), elapsed = now() - start;
        if (elapsed < result.counter_pair_min) result.counter_pair_min = elapsed;
    }
    ss_activity_begin();
    __atomic_store_n(&latency_cost_irq.enabled, 1, __ATOMIC_RELEASE);
    while (__atomic_load_n(&latency_cost_irq.count, __ATOMIC_ACQUIRE) < COST_IRQ_SAMPLES) {
        if (ss_live() < 0 || ss_throttled()) ss_fail(0xc06);
        ss_progress(); vTaskDelay(1);
    }
    __atomic_store_n(&latency_cost_irq.enabled, 0, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&latency_cost_irq.acknowledged, __ATOMIC_ACQUIRE)) {
        if (ss_live() < 0) ss_fail(0xc07);
        ss_progress(); vTaskDelay(1);
    }
    for (unsigned i = 0; i < COST_IRQ_SAMPLES; ++i) {
        result.irq_ticks[i] = latency_cost_irq.ticks[i];
        result.irq_os_ticks[i] = latency_cost_irq.os_ticks[i];
    }
    for (unsigned c = 0; c < 8; ++c) {
        xTaskNotifyGive(sender_task[c]);
        while (!atomic_load_explicit(&done[c], memory_order_acquire)) {
            if (ss_live() < 0 || ss_throttled()) ss_fail(0xc04);
            ss_progress(); vTaskDelay(1);
        }
    }
    if (!at_top()) ss_fail(0xc03);
    ss_activity_end(1);
    result.magic = COST_MAGIC;
    result.samples = COST_SAMPLES;
    result.roundtrips = COST_ROUNDTRIPS;
    result.core_mask = 0xff;
    ss_finish(&result, sizeof(result));
}
#ifndef LATENCY_COST_MODEL
void platform_puts(const char *text)
{ (void)text; ss_fail(0xc05); }
#endif
