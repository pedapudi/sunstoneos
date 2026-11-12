/* SPDX-License-Identifier: GPL-2.0-only */
/* Host threads, notifications and application calls for the protocol check.
 * With SHIM_SPURIOUS set, a notification wait returns early about half the time, as
 * if the OS had notified the task for its own purposes. The program runs the
 * application's init and main and writes the result it passes to ss_finish. */
#include "task.h"
#include "dram_scaling-app.h"
#include "../result.h"
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
/* The kit's task creation on the host: the same thread, no address space. */
void *ss_task_create_static_affinity_set(void (*function)(void *), const char *name, uint32_t depth, void *argument,
    unsigned long priority, void *stack, void *state, unsigned long affinity)
{ return xTaskCreateStaticAffinitySet(function, name, depth, argument, (unsigned)priority, stack, state, (uint32_t)affinity); }
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
static struct ss_span grant_spans[1];
static struct ss_grant grant = {.regions = grant_spans};
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
int main(void)
{
    spurious = getenv("SHIM_SPURIOUS") != 0;
    srand(7);
    grant.main_core = 6;
    grant.worker_cores = 0xbf;
    grant.region_count = 1;
    grant_spans[0].base = aligned_alloc(64, DS_BUFFER_BYTES);
    grant_spans[0].bytes = DS_BUFFER_BYTES;
    memset(grant_spans[0].base, 0xa5, DS_BUFFER_BYTES);
    dram_scaling_init();
    dram_scaling_main(&grant);
    return 3;
}
