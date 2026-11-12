/* NPU job timing: the time of single jobs and of pipelined jobs of each graph the OS takes from the models region (timing.h). */
#include "sunstone_app.h"
#include "sunstone_npu_queue.h"
#include "sunstone_npu_queue_service.h"
#include "FreeRTOS.h"
#include "task.h"
#include "timing.h"
#include "npu_timing-app.h"
#include <string.h>

#define TIMEOUT_US 1000000u
#define DEPTH NPU_TIMING_NPU_IN_FLIGHT
static struct npu_timing_result result;
static uint32_t ticks[NPU_TIMING_JOBS];
static struct ss_npu_queue *queue;
static ss_npu_handle handle[NPU_TIMING_GRAPHS];
static struct ss_npu_fence fences[DEPTH];

/* The graph being timed, in the top byte of every fault value: a failure record names its graph. */
static unsigned current;
static _Noreturn void fault(unsigned site, uint32_t value) { ss_fail_value((0xe0u << 8) | site, current << 24 | (value & 0xffffffu)); }
void npu_timing_init(void) {}

static void write_inputs(unsigned g, const uint8_t *data, uint64_t bytes)
{
    uint64_t offset = 0;
    for (unsigned i = 0;; ++i) {
        struct ss_npu_tensor t;
        if (!ss_npu_queue_tensor(queue, handle[g], SS_NPU_INPUT, i, &t)) break;
        if (offset + t.bytes > bytes) fault(0x10, g);
        memcpy(t.data, data + offset, t.bytes);
        if (!ss_npu_queue_dirty(queue, handle[g], SS_NPU_INPUT, i, 0, t.bytes)) fault(0x11, g);
        offset += t.bytes;
    }
    if (offset != bytes) fault(0x12, g);
}
static void submit(unsigned g, struct ss_npu_fence *fence, unsigned job)
{
    int r;
    while ((r = ss_npu_queue_submit(queue, handle[g], (struct ss_npu_fence){0}, TIMEOUT_US, fence)) == 0) ss_progress();
    if (r < 0) fault(0x20, job);
}
static void finish(struct ss_npu_fence fence, unsigned job)
{
    int r;
    while ((r = ss_npu_queue_status(queue, fence)) == 0) if (ss_live() < 0) fault(0x21, job);
    if (r < 0) fault(0x22, job);
    if (ss_npu_queue_release(queue, fence) != 1) fault(0x23, job);
}
static void acquire(unsigned g)
{
    const uint64_t begin = ss_counter_ticks();
    int r;
    while ((r = ss_npu_queue_acquire_handle(queue, handle[g])) != 1) {
        if (r < 0 || (ss_counter_ticks() - begin) / 24 > 2000000) fault(0x24, g);
        ss_progress(); vTaskDelay(1);
    }
}
static int below(const void *a, const void *b) { uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return (x > y) - (x < y); }
static void sort(uint32_t *v, unsigned n)
{
    for (unsigned i = 1; i < n; ++i) {     /* insertion sort: at most 1000 values, once per graph */
        const uint32_t x = v[i];
        unsigned j = i;
        while (j && below(&v[j - 1], &x) > 0) { v[j] = v[j - 1]; --j; }
        v[j] = x;
    }
}
static void time_graph(unsigned g, const uint8_t *region, const struct npu_timing_graph_desc *d)
{
    struct npu_timing_graph_result *r = &result.graph[g];
    current = g;
    if (!d->jobs || d->jobs > NPU_TIMING_JOBS || d->depth > DEPTH) fault(0x30, g);
    queue->idle_wait = (struct ss_npu_queue_idle_wait){0};     /* no job is in flight here */
    write_inputs(g, region + d->input_offset, d->input_bytes);
    r->input_bytes = d->input_bytes;
    for (unsigned job = 0; job < 3; ++job) { struct ss_npu_fence f; submit(g, &f, job); finish(f, job); }   /* warm-up */
    for (unsigned job = 0; job < d->jobs; ++job) {
        struct ss_npu_fence f;
        const uint64_t t0 = ss_counter_ticks();
        submit(g, &f, job);
        int done;
        while ((done = ss_npu_queue_status(queue, f)) == 0) if (ss_live() < 0) fault(0x21, job);
        ticks[job] = (uint32_t)(ss_counter_ticks() - t0);
        if (done < 0 || ss_npu_queue_release(queue, f) != 1) fault(0x22, job);
        ss_progress();      /* outside the timed span: a subsampling job takes 15 ms, and the progress window is 1 s */
    }
    uint64_t sum = 0;
    for (unsigned job = 0; job < d->jobs; ++job) sum += ticks[job];
    sort(ticks, d->jobs);
    r->jobs = d->jobs; r->sum = sum; r->min = ticks[0]; r->max = ticks[d->jobs - 1];
    r->p50 = ticks[d->jobs / 2]; r->p99 = ticks[(uint64_t)d->jobs * 99 / 100];
    if (d->depth) {
        unsigned submitted = 0, completed = 0;
        const uint64_t t0 = ss_counter_ticks();
        while (completed < d->jobs) {
            while (submitted < d->jobs && submitted - completed < d->depth) { submit(g, &fences[submitted % DEPTH], submitted); ++submitted; }
            finish(fences[completed % DEPTH], completed);
            ++completed;
            ss_progress();
        }
        r->depth = d->depth; r->pipelined_ticks = ss_counter_ticks() - t0;
    }
    /* The outputs of the last job against the simulator's. */
    acquire(g);
    uint64_t offset = 0, differ = 0;
    for (unsigned i = 0;; ++i) {
        struct ss_npu_tensor t;
        if (!ss_npu_queue_tensor(queue, handle[g], SS_NPU_OUTPUT, i, &t)) break;
        if (offset + t.bytes > d->reference_bytes) fault(0x31, g);
        const uint8_t *want = region + d->reference_offset + offset, *got = t.data;
        for (size_t b = 0; b < t.bytes; ++b) differ += got[b] != want[b];
        offset += t.bytes;
    }
    r->output_bytes = offset; r->differing_bytes = differ;
    const struct ss_npu_queue_idle_wait w = queue->idle_wait;
    r->idle_waits_busy = (w.waits > UINT32_MAX ? UINT32_MAX : w.waits) | (uint64_t)w.busy_bits << 32;
    result.idle_wait_ticks[g] = (w.max_ticks > UINT32_MAX ? UINT32_MAX : w.max_ticks) |
                                (uint64_t)(w.ticks > UINT32_MAX ? UINT32_MAX : w.ticks) << 32;
}
void npu_timing_main(const struct ss_grant *grant)
{
    uint64_t begin = ss_counter_ticks();
    while (!ss_clock_ready() || ss_throttled()) {
        if (ss_live() < 0 || ss_counter_ticks() - begin > 240000000) fault(0x01, 0);
        vTaskDelay(1);
    }
    if (grant->region_count < 2) fault(0x02, grant->region_count);
    const uint8_t *region = grant->regions[1].base;
    const struct npu_timing_header *h = (const void *)region;
    if (h->magic != NPU_TIMING_MAGIC || h->version != NPU_TIMING_VERSION || !h->count || h->count > NPU_TIMING_GRAPHS) fault(0x03, h->magic);
    for (unsigned g = 0; g < h->count; ++g) {
        const struct npu_timing_graph_desc *d = &h->graph[g];
        if (d->input_offset > grant->regions[1].bytes || d->input_bytes > grant->regions[1].bytes - d->input_offset ||
            d->reference_offset > grant->regions[1].bytes || d->reference_bytes > grant->regions[1].bytes - d->reference_offset)
            fault(0x04, g);
    }
    begin = ss_counter_ticks();
    int opened;
    while ((opened = ss_npu_queue_open(DEPTH, &queue)) != 1) {
        if (opened < 0 || (ss_counter_ticks() - begin) / 24 > 8000000) fault(0x05, (uint32_t)opened);
        ss_progress(); vTaskDelay(1);
    }
    for (unsigned g = 0; g < h->count; ++g) if (!ss_npu_queue_register(queue, g, &handle[g])) fault(0x06, g);
    result.open_ticks = ss_counter_ticks() - begin;
    result.magic = NPU_TIMING_RESULT_MAGIC; result.version = NPU_TIMING_RESULT_VERSION; result.bytes = sizeof(result); result.count = h->count;
    {
        char line[192]; unsigned long v[7] = {0};
        static const char *const keys[] = {"cpu_l_mhz=", "cpu_b_mhz=", "cpu_l_mv=", "cpu_b_mv=", "transitions=", "dsu_mhz=", "core="};
        if (ss_clock_info(line, sizeof(line)) > 0)
            for (unsigned i = 0; i < 7; ++i) { const char *at = strstr(line, keys[i]); if (at) { unsigned long x = 0; at += strlen(keys[i]); while (*at >= '0' && *at <= '9') x = x * 10 + (unsigned long)(*at++ - '0'); v[i] = x; } }
        result.clock[0] = v[0] | v[1] << 16 | v[6] << 32 | v[4] << 40; result.clock[1] = v[2] | v[3] << 16 | v[5] << 32;
    }
    for (unsigned g = 0; g < h->count; ++g) time_graph(g, region, &h->graph[g]);
    begin = ss_counter_ticks();
    int closed;
    while ((closed = ss_npu_queue_close(queue)) != 1) {
        if (closed < 0 || (ss_counter_ticks() - begin) / 24 > 2000000) fault(0x60, (uint32_t)closed);
        ss_progress(); vTaskDelay(1);
    }
    ss_finish(&result, sizeof(result));
}
void platform_puts(const char *text) { (void)text; ss_fail(0x8e0); }
