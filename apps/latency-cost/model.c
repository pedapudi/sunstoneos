/* OS calls are modeled; notifications and context switches execute natively. */
#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "sunstone_app.h"
#include "result.h"
extern void latency_cost_init(void);
extern void latency_cost_main(const struct ss_grant *);
static struct ss_grant grant = {.main_core = 6, .worker_cores = 0xbf};
static StaticTask_t state;
static StackType_t stack[2048];
const struct ss_grant *ss_grant(void) { return &grant; }
int ss_live(void) { return 1; }
int ss_throttled(void) { return 0; }
int ss_clock_ready(void) { return 1; }
void ss_progress(void) {}
void ss_activity_begin(void) {}
void ss_activity_end(int success) { configASSERT(success); }
int ss_clock_info(char *line, size_t bytes)
{
    const char clock[] = "CLOCK cpu_l_mhz=1794 cpu_b_mhz=2002 cpu_l_mv=1000 cpu_b_mv=1050 ";
    configASSERT(bytes >= sizeof(clock));
    for (unsigned i = 0; i < sizeof(clock); ++i) line[i] = clock[i];
    return sizeof(clock) - 1;
}
_Noreturn void ss_fail(uint32_t detail) { platform_panic("cost failed", detail); }
_Noreturn void ss_finish(const void *data, size_t bytes)
{
    const struct cost_result *r = data;
    configASSERT(bytes == sizeof(*r) && r->magic == COST_MAGIC && r->core_mask == 255);
    unsigned clean = 0, extra = 0;
    for (unsigned i = 0; i < COST_IRQ_SAMPLES; ++i)
        configASSERT(r->irq_ticks[i] && r->irq_os_ticks[i] == 1);
    for (unsigned c = 0; c < 8; ++c)
        for (unsigned s = 0; s < COST_SAMPLES; ++s) {
            configASSERT(r->core[c].ticks[s] > 0);
            configASSERT(r->core[c].switches[s] >= 2 * COST_ROUNDTRIPS);
            if (r->core[c].switches[s] == 2 * COST_ROUNDTRIPS) ++clean;
            else ++extra;
        }
    platform_puts("MODEL latency cost; emulated clocks; no board timing\nclean_batches=");
    platform_hex(clean); platform_puts(" extra_switch_batches="); platform_hex(extra);
    const uint64_t *words = data;
    for (unsigned i = 0; i < bytes / 8; ++i) {
        platform_puts("\ncost_data="); platform_hex(words[i]);
    }
    platform_puts("\nPASS latency cost full notification/context-switch path on eight cores\n");
    platform_exit(0);
}
static void controller(void *unused)
{ (void)unused; latency_cost_main(&grant); }
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{ (void)task; platform_panic(name, 1); }
void kernel_main(void)
{
    platform_init();
    latency_cost_init();
    configASSERT(xTaskCreateStaticAffinitySet(controller, "cost-control", 2048, 0, 2,
        stack, &state, 1u << 6));
    vTaskStartScheduler();
    platform_panic("cost scheduler returned", 0);
}
