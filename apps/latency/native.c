/* The same bounded CPU workload through the existing returning OS lifetime.
 * Native DMA is absent until the OS admits and drains a real DMA0 service. */
#include "sunstone_app.h"
#include "FreeRTOS.h"
#include "task.h"
#include "workload.h"

static struct latency_result result;
void latency_workload_progress(void)
{
    if (ss_live() < 0 || ss_throttled()) ss_fail(0xb01);
    ss_progress();
}
void latency_init(void)
{
    const struct ss_grant *grant = ss_grant();
    if (!grant || grant->main_core != 6 || grant->worker_cores != 0xbf) ss_fail(0xb02);
    uint64_t frequency;
    __asm__ volatile("mrs %0,cntfrq_el0" : "=r"(frequency));
    ss_latency_init(frequency);
    latency_workload_init();
}
static int at_top(void)
{
    char clock[256];
    const char prefix[] = "CLOCK cpu_l_mhz=1794 cpu_b_mhz=2002 cpu_l_mv=1000 cpu_b_mv=1050 ";
    if (ss_clock_info(clock, sizeof(clock)) < (int)(sizeof(prefix) - 1)) return 0;
    for (unsigned i = 0; i < sizeof(prefix) - 1; ++i) if (clock[i] != prefix[i]) return 0;
    return 1;
}
void latency_main(const struct ss_grant *grant)
{
    (void)grant;
    while (!ss_clock_ready()) { latency_workload_progress(); vTaskDelay(1); }
    if (!at_top()) ss_fail(0xb03);
    ss_activity_begin();
    latency_workload_run(&result);
    if (!at_top()) ss_fail(0xb03);
    ss_activity_end(1);
    ss_finish(&result, sizeof(result));
}
/* The returning profile has no UART output authority. */
void platform_puts(const char *text)
{ (void)text; ss_fail(0xb04); }
