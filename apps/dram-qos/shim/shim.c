/* SPDX-License-Identifier: GPL-2.0-only */
/* Host threads, notifications and application calls for the protocol check.
 * With SHIM_SPURIOUS set, a notification wait returns early about half the time, as
 * if the OS had notified the task for its own purposes. The program runs the
 * application's init and main and writes the result it passes to ss_finish. */
#include "task.h"
#include "dram_qos-app.h"
#include "../result.h"
#include "sunstone_dma.h"
#include <string.h>
#include <time.h>
static __thread StaticTask_t *self;
static __thread unsigned my_core = 6;
static StaticTask_t main_task = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 6, 0, 0, 0};
static int spurious;
unsigned shim_core(void) { return my_core; }
static void *entry(void *p)
{
    StaticTask_t *t = p;
    self = t;
    my_core = t->core;
    t->function(t->argument);
    return 0;
}
TaskHandle_t xTaskCreateStaticAffinitySet(void (*function)(void *), const char *name, uint32_t depth,
    void *argument, unsigned priority, StackType_t *stack, StaticTask_t *state, uint32_t affinity)
{
    (void)name; (void)depth; (void)priority; (void)stack;
    pthread_mutex_init(&state->lock, 0);
    pthread_cond_init(&state->wake, 0);
    state->notifications = 0;
    state->core = (unsigned)__builtin_ctz(affinity);
    state->function = function;
    state->argument = argument;
    pthread_create(&state->thread, 0, entry, state);
    return state;
}
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return self ? self : &main_task; }
uint32_t ulTaskNotifyTake(int clear, TickType_t ticks)
{
    StaticTask_t *t = xTaskGetCurrentTaskHandle();
    if (spurious && rand() % 2) return 1;
    struct timespec limit;
    clock_gettime(CLOCK_REALTIME, &limit);
    if (ticks != portMAX_DELAY) {
        limit.tv_sec += ticks / 1000;
        limit.tv_nsec += (long)(ticks % 1000) * 1000000;
        if (limit.tv_nsec >= 1000000000) { limit.tv_sec++; limit.tv_nsec -= 1000000000; }
    }
    pthread_mutex_lock(&t->lock);
    while (!t->notifications) {
        if (ticks == portMAX_DELAY) pthread_cond_wait(&t->wake, &t->lock);
        else if (pthread_cond_timedwait(&t->wake, &t->lock, &limit)) break;
    }
    uint32_t count = t->notifications;
    t->notifications = clear ? 0 : (count ? count - 1 : 0);
    pthread_mutex_unlock(&t->lock);
    return count;
}
int xTaskNotifyGive(TaskHandle_t t)
{
    pthread_mutex_lock(&t->lock);
    ++t->notifications;
    pthread_cond_signal(&t->wake);
    pthread_mutex_unlock(&t->lock);
    return 1;
}
void vTaskDelay(TickType_t ticks) { struct timespec s = {0, (long)ticks * 1000000}; nanosleep(&s, 0); }
static struct ss_grant grant;
const struct ss_grant *ss_grant(void) { return &grant; }
int ss_live(void) { return 1; }
void ss_progress(void) {}
int ss_clock_ready(void) { return 1; }
int ss_throttled(void) { return 0; }
void ss_thermal_status(struct ss_thermal_status *status) { memset(status, 0, sizeof *status); }
void ss_activity_begin(void) {}
void ss_activity_end(int success) { (void)success; }
int ss_clock_info(char *line, size_t bytes)
{
    return snprintf(line, bytes, "CLOCK cpu_l_mhz=1794 cpu_b_mhz=2002 cpu_l_mv=1000 cpu_b_mv=1050 transitions=3 dsu_mhz=1274 sid=12340400 bin=0 core=6 pattern=0 parked_max_us=40\n");
}
void ss_finish(const void *result, size_t bytes)
{
    fwrite(result, 1, bytes, stdout);
    fflush(stdout);
    exit(0);
}
void ss_fail(uint32_t detail) { fprintf(stderr, "ss_fail %x\n", detail); exit(1); }
unsigned ss_npu_model_count(void) { return 1; }
int ss_npu_model_info(unsigned i, struct ss_npu_model *m) { (void)i; memset(m, 0, sizeof *m); m->input_bytes = 1024; m->output_bytes = 1024; return 1; }
int ss_npu_model_find(const char *n, unsigned *i) { (void)n; *i = 0; return 1; }
int ss_npu_run(unsigned model, const void *in, size_t ib, void *out, size_t ob, uint64_t t)
{ (void)model; (void)in; (void)ib; (void)out; (void)ob; (void)t; static unsigned n; return (++n % 50) == 0 ? 1 : 0; }
int ss_npu_close(void) { return 1; }
static unsigned dma_step;
int ss_dma_open(struct ss_dma_buffers *o) { (void)o; return 1; }
int ss_dma_submit(void) { return 1; }
int ss_dma_complete(void) { return (++dma_step % 3) ? 0 : 1; }
int ss_dma_reap(void) { return 1; }
int ss_dma_close(void) { return 1; }
int main(void)
{
    spurious = getenv("SHIM_SPURIOUS") != 0;
    srand(7);
    grant.main_core = 6;
    grant.worker_cores = 0xbf;
    grant.region_count = 1;
    grant.regions[0].base = aligned_alloc(64, DQ_BUFFER_BYTES);
    grant.regions[0].bytes = DQ_BUFFER_BYTES;
    memset(grant.regions[0].base, 0xa5, DQ_BUFFER_BYTES);
    dram_qos_init();
    dram_qos_main(&grant);
    return 3;
}
