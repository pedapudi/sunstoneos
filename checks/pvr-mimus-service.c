/* Host check of the GPU service with its mimus session component
 * (docs/gpu-mimus-service.md, docs/designs/gpu-recognition.md "Budgets").
 *
 * Two-core FreeRTOS under QEMU. The session is built by the real
 * a733_gpu_reserved_prepare and a733_gpu_mimus_prepare over the production
 * write-back and uncached pools, and the kernel ring runs through
 * platform/a733/gpu_memory.c. The firmware model of checks/pvr-gpu-service.c
 * answers start, control and stop; this file adds the GPU: on a client-ring
 * KICK it checks the two commands, and a model task on core 1 computes the
 * job from the job words after the duration apps/os/placement.py models for
 * it, writes sums, tags and the timeline, and raises SPI 95 on the QEMU GIC,
 * whose handler on core 0 calls pvr_gpu_service_interrupt. A client task at
 * EL0 stands in for the recognizer: OPEN, then the bench recording's 204
 * placed jobs, each checked against an int64 CPU sum.
 *
 * Fixture differences from the image: model weights are synthetic bytes at
 * the real tensor offsets; the output region stays writable at EL1 so that
 * the model can store; GPU registers are model callbacks; the worker runs on
 * core 0 with the client and the interrupt, the model on core 1. */
#define kernel_main firmware_fixture_main
#define owner firmware_fixture_owner
#define kick firmware_fixture_kick
#define event firmware_fixture_event
#define irq_read firmware_fixture_irq_read
#define irq_write firmware_fixture_irq_write
#include "pvr-gpu-service.c"
#undef kernel_main
#undef owner
#undef kick
#undef event
#undef irq_read
#undef irq_write
#include "pvr_mimus_service.h"
#include "a733_gpu_mimus.h"
#include "a733_gpu_backend.h"
#include "gicv3_spi.h"
#include "mimus-fixture.h"

/* 1 bench with exact sums; 2 bench with pattern sums (instrumented runs);
 * 3 as 2 with jobs that complete at once; 4 as 3 with one firmware event per
 * job; 5 refused jobs; 6 failed wake send; 7 missing tag; 8 hang; 9 context
 * reset; 10 OPEN only; 11 an unexpected timeline value before a submit; 12 the
 * completion's clean and invalidate fails; 13 completion at its deadline;
 * 14 output acquisition reaches the deadline; 15 completion just before it;
 * 16 failed final clock read; 17 backward final clock sample; 18 pending,
 * active and drained CLOSE; 19 reserved-slot CLOSE and abort; 20 lost CLOSE. */
volatile uint32_t mimus_service_case = 1;
/* Nonzero in the runner's instrumented runs, which execute many times slower
 * against wall-clock deadlines: OPEN then gets 60 s instead of 1 s. */
__attribute__((section(".data"))) volatile uint32_t mimus_service_slow = 0;
static struct a733_gpu_reserved firmware;
static struct a733_gpu_mimus session;
static struct pvr_mimus_service *mimus;
static struct pvr_mimus_tensor tensors[FIXTURE_TENSORS];
static StaticTask_t mimus_tasks[2];
static StackType_t mimus_stacks[2][4096];
static const struct gicv3_spi spi = {0x08000000, 95};
#define GICD 0x08000000u

/* GPU and firmware model state. */
static uint32_t kicked, run, gpu_status, irq_count, hook_wakes, kick_step_count;
static uint64_t kick_us;
static uint32_t last_sequence;
static atomic_uint hold_slot, hold_completion;
static unsigned refuse_stopped, fail_reopen_cache, fail_reopen_post, stopped_proofs;
BaseType_t __real_xQueueGenericSend(QueueHandle_t, const void *, TickType_t, BaseType_t);
BaseType_t __wrap_xQueueGenericSend(QueueHandle_t queue, const void *item, TickType_t wait, BaseType_t position)
{
    if (fail_reopen_post && service.epoch == 2 && queue == mimus->completions) {
        const struct pvr_mimus_record *record = item;
        configASSERT(!record->sequence && record->result == PVR_MIMUS_OK);
        fail_reopen_post = 0;
        return pdFALSE;
    }
    return __real_xQueueGenericSend(queue, item, wait, position);
}
/* The firmware fixture models the native stop proof. Physical register and
 * platform verification remain covered by the native backend's own checks. */
int __wrap_a733_gpu_backend_stopped(struct a733_gpu_backend *backend, struct pvr_gpu_service *s)
{
    (void)backend;
    ++stopped_proofs;
    return !refuse_stopped && s == &service && s->phase == PVR_GPU_SERVICE_STOPPED &&
        s->stop.phase == PVR_GPU_STOP_STOPPED && s->stop.isolation_observed &&
        s->stop.acknowledgement[0] == 1 && s->stop.acknowledgement[1] == 1 &&
        s->queue.phase == PVR_CCB_RETIRED && s->control.phase == PVR_GPU_CONTROL_RETIRED &&
        memory.phase == A733_GPU_MEMORY_OFFLINE && s->irq_masked && !s->irq_fault;
}
int __real_pvr_mimus_service_poll(struct pvr_mimus_service *);
int __wrap_pvr_mimus_service_poll(struct pvr_mimus_service *m)
{
    if (atomic_load(&hold_slot) && m->phase == PVR_MIMUS_SERVICE_IDLE) return 0;
    return __real_pvr_mimus_service_poll(m);
}
/* Selected boundary cases advance the actual service clock callback only
 * after the client ring retired and the modeled timeline completed. The
 * normal service clock wrapper still enforces monotonicity. */
static unsigned completion_clock, completion_clock_samples;
static int boundary_now(void *ctx, uint64_t *out)
{
    if (!completion_clock) return now(ctx, out);
    callback(ctx);
    if (completion_clock == 3) return 0;
    *out = completion_clock == 2 ? mimus->deadline :
           completion_clock == 4 ? mimus->deadline - 2 : mimus->deadline - 1;
    return 1;
}
int __real_pvr_gpu_service_now(struct pvr_gpu_service *, uint64_t *);
int __wrap_pvr_gpu_service_now(struct pvr_gpu_service *s, uint64_t *out)
{
    unsigned c = mimus_service_case;
    if (((c >= 13 && c <= 17) || c == 20) && mimus && s == &service &&
        mimus->phase == PVR_MIMUS_SERVICE_IN_FLIGHT && mimus->client.phase == PVR_CCCB_IDLE &&
        *(const volatile uint32_t *)mimus->config->firmware[5].storage.cpu == mimus->sequence) {
        if (!completion_clock) completion_clock = c == 13 || c == 20 ? 2 : 1;
        ++completion_clock_samples;
    }
    return __real_pvr_gpu_service_now(s, out);
}

static uint64_t now_us(void)
{
    uint64_t c, f;
    __asm__ volatile("isb\n mrs %0, cntpct_el0\n mrs %1, cntfrq_el0" : "=r"(c), "=r"(f));
    return c / f * 1000000 + c % f * 1000000 / f;
}
static int mask(void *ctx) { (void)ctx; return gicv3_spi_mask(&spi); }
static int rearm(void *ctx) { (void)ctx; return gicv3_spi_rearm(&spi); }
static int mode(void *ctx) { callback(ctx); return 1; }
/* GPU interrupt status register: the completion interrupt or a pending
 * firmware event; the control poll clears it. */
static int irq_read(void *ctx, uint32_t off, uint32_t *out)
{
    callback(ctx); configASSERT(off == 0x8a8);
    *out = get(&gpu_status) || get((uint32_t *)resource(3)) != get((uint32_t *)(resource(3) + 128));
    return 1;
}
static int irq_write(void *ctx, uint32_t off, uint32_t value)
{ callback(ctx); configASSERT(off == 0x8b0 && value == 1); set(&gpu_status, 0); return 1; }
static int event(void *ctx, const unsigned char command[88]) { (void)ctx; (void)command; return 0; }
void fixture_gpu_irq(void)
{
    BaseType_t woken = pdFALSE;
    __atomic_fetch_add(&irq_count, 1, __ATOMIC_RELAXED);
    configASSERT(pvr_gpu_service_interrupt(&service, &woken));
    portYIELD_FROM_ISR(woken);
}
/* The client-ring KICK: both commands as the session wrote them, the update
 * naming the timeline with the next sequence; the kernel return; then the
 * job goes to the model. */
#ifdef SUNSTONE_A733_OS_PERF
static int perf_kick(void *,unsigned,unsigned,unsigned char *);
#endif
static int kick(void *ctx, uint32_t off, uint32_t value)
{
    unsigned next = (unsigned)word(resource(0), 4), slot = (next - 1) & 127;
    unsigned char *command = resource(1) + slot * 88;
#ifdef SUNSTONE_A733_OS_PERF
    if(perf_kick(ctx,next,slot,command))return 1;
#endif
    if (word(command, 4) != (PVR_CCB_MAGIC | 101u)) return firmware_fixture_kick(ctx, off, value);
    callback(ctx); configASSERT(off == 0xb00 && !value && running);
    const struct pvr_mimus_service_config *c = mimus->config;
    configASSERT(word(command + 8, 4) == c->firmware_address[0]);
    configASSERT(word(command + 16, 4) == PVR_MIMUS_SERVICE_RING_BYTES - 1);
    uint32_t cursor = (uint32_t)word(command + 12, 4);
    configASSERT(cursor >= 184 && cursor <= PVR_MIMUS_SERVICE_RING_BYTES);
    unsigned char *ring = (unsigned char *)c->firmware[3].storage.cpu + cursor - 184;
    configASSERT(word(ring, 4) == (PVR_CCB_MAGIC | PVR_CCCB_TASK | 205u) && word(ring + 4, 4) == 96);
    configASSERT(!memcmp(ring + 40, mimus->command, 96));
    configASSERT(word(ring + 136, 4) == (PVR_CCB_MAGIC | 213u) && word(ring + 140, 4) == 8);
    configASSERT(word(ring + 176, 4) == c->firmware_address[5]);
    uint32_t sequence = (uint32_t)word(ring + 180, 4);
    configASSERT(sequence == last_sequence + 1);
    last_sequence = sequence;
    unsigned char *control = c->firmware[2].storage.cpu;
    put(control, 4, cursor); put(control + 4, 4, cursor % PVR_MIMUS_SERVICE_RING_BYTES);
    put(resource(0) + 128, 4, next); put(resource(2) + slot * 4, 4, 1);
    kick_us = now_us();
    ++kick_step_count;
    __atomic_store_n(&kicked, sequence, __ATOMIC_RELEASE);
    return 1;
}
static uint32_t lane_tags(uint32_t rows, uint32_t n) { return pvr_mimus_tags(rows, n); }
static int64_t pattern(uint32_t sequence, uint32_t r, uint32_t c)
{ return (int64_t)(sequence * 131u + r * 7u + c) - 5000; }
/* Weights of a GPU address, at the fixture's model regions. */
static const int8_t *weights_cpu(uint64_t gpu)
{ return (const int8_t *)(uintptr_t)(0x44000000u + (gpu - PVR_MIMUS_MODEL_GPU)); }
static void model(void *unused)
{
    (void)unused;
    for (;;) {
        while (__atomic_load_n(&kicked, __ATOMIC_ACQUIRE) == run) {}
        uint32_t sequence = __atomic_load_n(&kicked, __ATOMIC_ACQUIRE);
        uint64_t started = kick_us;
        const unsigned char *words = mimus->config->session.job_cpu + PVR_MIMUS_WORDS;
        uint64_t w_gpu = word(words, 8), x_gpu = word(words + 8, 8);
        uint32_t k = (uint32_t)word(words + 16, 2), n = (uint32_t)word(words + 18, 2);
        uint32_t rows = (uint32_t)word(words + 20, 4), ws = (uint32_t)word(words + 24, 2);
        uint32_t xs = (uint32_t)word(words + 26, 2);
        configASSERT((uint32_t)word(words + 28, 4) == sequence);
        configASSERT(x_gpu >= PVR_MIMUS_ARENA_GPU && w_gpu >= PVR_MIMUS_MODEL_GPU);
        unsigned job = sequence - 1, c = mimus_service_case;
        configASSERT(job < FIXTURE_JOBS && fixture_jobs[job].rows == rows && fixture_jobs[job].k == k &&
                     fixture_jobs[job].n_gpu == n);
        const int8_t *wt = weights_cpu(w_gpu);
        const int16_t *x = (const int16_t *)(uintptr_t)x_gpu;
        unsigned char *sums = (unsigned char *)(uintptr_t)(PVR_MIMUS_OUTPUT_GPU + PVR_MIMUS_TAG_BYTES);
        unsigned width = k > 512 ? 8 : 4;
        for (uint32_t r = 0; r < rows; ++r)
            for (uint32_t ch = 0; ch < n; ++ch) {
                int64_t s = 0;
                if (c == 1) {
                    const int16_t *xr = x + (size_t)r * xs / 2; const int8_t *wr = wt + (size_t)ch * ws;
                    for (uint32_t i = 0; i < k; ++i) s += (int32_t)xr[i] * wr[i];
                } else s = pattern(sequence, r, ch);
                unsigned char *p = sums + ((size_t)r * n + ch) * width;
                if (width == 4) *(int32_t *)p = (int32_t)s; else *(int64_t *)p = s;
            }
        uint32_t tags = lane_tags(rows, n);
        uint32_t *tag = (uint32_t *)(uintptr_t)(PVR_MIMUS_OUTPUT_GPU + PVR_MIMUS_TAG_BYTES) - tags;
        for (uint32_t i = 0; i < tags; ++i) tag[i] = (c == 7 && i == 0) ? sequence - 1 : sequence;
        uint64_t duration = c == 3 || c == 4 ? 0 : fixture_jobs[job].duration_us;
        while (now_us() < started + duration) {}
        while (atomic_load(&hold_completion)) {}
        __asm__ volatile("dsb sy" ::: "memory");
        if (c == 4 || c == 9 || c == 30) {
            unsigned write = get((uint32_t *)resource(PVR_FW_EVENT_CONTROL));
            unsigned char *e = resource(PVR_FW_EVENTS) + (write & 31) * 88;
            memset(e, 0, 88);
            if (c == 4) { put(e, 4, PVR_CCB_MAGIC | 107); put(e + 8, 4, 1); put(e + 16, 4, 1); }
            else { put(e, 4, PVR_CCB_MAGIC | 105); put(e + 8, 4, mimus->config->firmware_address[0]); }
            __atomic_store_n((uint32_t *)resource(PVR_FW_EVENT_CONTROL), (write + 1) & 31, __ATOMIC_RELEASE);
        }
        if (c != 8 && c != 9 && c != 30) put(mimus->config->firmware[5].storage.cpu, 4, sequence);
        __asm__ volatile("dsb sy" ::: "memory");
        set(&gpu_status, 1);
        /* Level interrupt: pending until the handler masks it. */
        *(volatile uint32_t *)(uintptr_t)(GICD + 0x200 + 4 * (95 / 32)) = 1u << (95 % 32);
        __asm__ volatile("dsb sy" ::: "memory");
        run = sequence;
    }
}

/* Client: the recognizer's side, as apps/os/gpu.c and apps/os/gpu_hook.cc. */
static uint64_t seed = 0x9e3779b97f4a7c15u;
static uint64_t next_random(void) { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; }
static struct pvr_mimus_record receive(void)
{
    struct pvr_mimus_record r;
    TickType_t start = xTaskGetTickCount();
    while (!pvr_mimus_service_receive(mimus, &r, 10))
        configASSERT(xTaskGetTickCount() - start < (mimus_service_slow ? 60000u : 3000u));
    return r;
}
static void wait_record_time(uint64_t *us, struct pvr_mimus_record *r) { *r = receive(); *us = now_us(); }
static uint32_t sums_checked;
static int sums_equal(unsigned job, uint32_t sequence)
{
    const struct pvr_mimus_tensor *t = &tensors[fixture_jobs[job].ordinal];
    uint32_t rows = fixture_jobs[job].rows, n = t->n_gpu, k = t->k, width = k > 512 ? 8 : 4;
    const unsigned char *base = (const unsigned char *)(uintptr_t)(PVR_MIMUS_OUTPUT_GPU + PVR_MIMUS_TAG_BYTES);
    const int8_t *wt = weights_cpu(t->weights);
    for (uint32_t r = 0; r < rows; ++r)
        for (uint32_t c = 0; c < n; ++c) {
            int64_t s = 0;
            if (mimus_service_case == 1) {
                const int16_t *x = (const int16_t *)(uintptr_t)(PVR_MIMUS_ARENA_GPU + (size_t)r * 2 * k);
                const int8_t *w = wt + (size_t)c * t->stride;
                for (uint32_t i = 0; i < k; ++i) s += (int64_t)x[i] * w[i];
            } else s = pattern(sequence, r, c);
            const unsigned char *p = base + ((size_t)r * n + c) * width;
            int64_t g = width == 4 ? *(const volatile int32_t *)p : *(const volatile int64_t *)p;
            if (g != s) return 0;
            ++sums_checked;
        }
    return 1;
}
static uint32_t completed;
/* One job of the bench: 1 when it completed with every tag and sum. */
static int bench_job(unsigned job)
{
    const unsigned c = mimus_service_case;
    uint32_t k = fixture_jobs[job].k, rows = fixture_jobs[job].rows;
    if (c == 1) {
        int16_t *x = (int16_t *)(uintptr_t)PVR_MIMUS_ARENA_GPU;
        for (size_t i = 0; i < (size_t)rows * k; ++i)
            x[i] = i < k ? 32767 : (int16_t)(next_random() % 65535 - 32767);
    }
    const struct pvr_mimus_request request = {0, fixture_jobs[job].ordinal, rows, 2 * k};
    uint64_t submitted = now_us();
    configASSERT(pvr_mimus_service_submit(mimus, &request) == 1);
    __atomic_fetch_add(&hook_wakes, 1, __ATOMIC_RELAXED);
    struct pvr_mimus_record r = receive();
    if (r.result != PVR_MIMUS_OK || r.sequence != completed + 1) {
        metric("failed-job", job); metric("failed-result", r.result); metric("failed-sequence", r.sequence);
        metric("component-error", mimus->error); metric("service-error", service.error);
        metric("control-error", service.control.error); metric("queue-phase", service.queue.phase);
        metric("ring-error", mimus->client.error);
        metric("kicks-seen", kick_step_count); metric("since-kick-us", now_us() - kick_us);
        metric("model-run", run); metric("irqs", irq_count); metric("since-submit-us", now_us() - submitted);
        metric("kick-after-submit-us", kick_us - submitted);
    }
    configASSERT(r.result == PVR_MIMUS_OK && r.sequence == completed + 1);
    completed = r.sequence;
    if (!pvr_mimus_tags_complete(PVR_MIMUS_OUTPUT_GPU, rows, fixture_jobs[job].n_gpu, r.sequence)) return 0;
    configASSERT(sums_equal(job, r.sequence));
    return 1;
}
/* Pages the fixture's pools have handed out; only the GPU uses them here. */
static void pool_pages(unsigned *wb, unsigned *nc)
{
    *wb = *nc = 0;
    struct device_memory_pool *w = a733_memory_reservations_init(), *n = a733_memory_uncached_init();
    for (unsigned i = 0; i < DEVICE_MEMORY_MAX_ALLOCATIONS; ++i) {
        if (w->allocations[i].state != DEVICE_MEMORY_FREE) *wb += w->allocations[i].pages;
        if (n->allocations[i].state != DEVICE_MEMORY_FREE) *nc += n->allocations[i].pages;
    }
}
/* --wrap counters of the two architecture helpers and the session code.
 * Every maintenance call on the output region must clean and invalidate:
 * DC IVAC is checked as a write and faults on the image's read-only view.
 * Case 12 makes the first such call of a job fail once. */
uint64_t cache_calls, cache_lines, translations, output_cache_calls;
uint32_t submit_calls, complete_calls, ring_polls, fail_output_cache;
int __real_aarch64_device_cache(enum aarch64_cache_op, uintptr_t, size_t);
int __wrap_aarch64_device_cache(enum aarch64_cache_op op, uintptr_t address, size_t bytes)
{
    ++cache_calls; cache_lines += (bytes + 63) / 64;
    if (fail_reopen_cache && service.epoch == 2) { fail_reopen_cache = 0; return 0; }
    if (address < PVR_MIMUS_OUTPUT_GPU + PVR_MIMUS_OUTPUT_BYTES && address + bytes > PVR_MIMUS_OUTPUT_GPU) {
        configASSERT(op == AARCH64_CACHE_CLEAN_INVALIDATE && address >= PVR_MIMUS_OUTPUT_GPU &&
                     address + bytes <= PVR_MIMUS_OUTPUT_GPU + PVR_MIMUS_OUTPUT_BYTES);
        ++output_cache_calls;
        if (__atomic_exchange_n(&fail_output_cache, 0, __ATOMIC_ACQ_REL)) return 0;
    }
    return __real_aarch64_device_cache(op, address, bytes);
}
int __real_pvr_mimus_submit(struct pvr_mimus_session *, const struct pvr_mimus_tensor *, const struct pvr_mimus_job *);
int __wrap_pvr_mimus_submit(struct pvr_mimus_session *s, const struct pvr_mimus_tensor *t, const struct pvr_mimus_job *j)
{ ++submit_calls; return __real_pvr_mimus_submit(s, t, j); }
int __real_pvr_mimus_complete(struct pvr_mimus_session *);
int __wrap_pvr_mimus_complete(struct pvr_mimus_session *s)
{
    ++complete_calls;
    int result = __real_pvr_mimus_complete(s);
    if (mimus_service_case >= 14 && mimus_service_case <= 17) {
        configASSERT(completion_clock == 1 && completion_clock_samples == 1);
        if (mimus_service_case == 14) completion_clock = 2;
        if (mimus_service_case == 16) completion_clock = 3;
        if (mimus_service_case == 17) completion_clock = 4;
    }
    return result;
}
enum pvr_cccb_result __real_pvr_cccb_poll(struct pvr_cccb *);
enum pvr_cccb_result __wrap_pvr_cccb_poll(struct pvr_cccb *c) { ++ring_polls; return __real_pvr_cccb_poll(c); }
int __real_aarch64_device_translate(uintptr_t, int, struct aarch64_translation *);
int __wrap_aarch64_device_translate(uintptr_t a, int w, struct aarch64_translation *t)
{ ++translations; return __real_aarch64_device_translate(a, w, t); }
/* Marker the runner's instruction trace splits job windows by. */
__attribute__((noinline)) void fixture_mark(uint32_t what) { __asm__ volatile("" :: "r"(what) : "memory"); }

static struct pvr_gpu_service_reply close_reply(void)
{
    const struct pvr_gpu_service_request close = {PVR_GPU_SERVICE_CLOSE, 1000000};
    struct pvr_gpu_service_reply reply;
    uint64_t ticket;
    configASSERT(pvr_gpu_service_send(&service, &close, 10, &ticket));
    TickType_t start = xTaskGetTickCount();
    while (!pvr_gpu_service_collect(&service, ticket, &reply, 10))
        configASSERT(xTaskGetTickCount() - start < 3000);
    return reply;
}
static struct pvr_gpu_service_reply reopen_reply(uint64_t epoch)
{
    const struct pvr_gpu_service_request open = {PVR_GPU_SERVICE_OPEN, 1000000};
    struct pvr_gpu_service_reply reply;
    uint64_t ticket;
    configASSERT(pvr_gpu_service_send_epoch(&service, epoch, &open, 10, &ticket));
    TickType_t start = xTaskGetTickCount();
    while (!pvr_gpu_service_collect(&service, ticket, &reply, 10))
        configASSERT(xTaskGetTickCount() - start < 3000);
    return reply;
}
static void closed(void)
{
    configASSERT(close_reply().result == PVR_GPU_SERVICE_OK);
    TickType_t start = xTaskGetTickCount();
    while (service.phase != PVR_GPU_SERVICE_STOPPED) {
        configASSERT(service.phase != PVR_GPU_SERVICE_FAILED && xTaskGetTickCount() - start < 3000);
        vTaskDelay(1);
    }
}

#ifdef SUNSTONE_A733_OS_PERF
#include "gpu-performance-service.inc"
#endif
static void client(void *unused)
{
    (void)unused;
    const unsigned c = mimus_service_case;
    uint64_t lines0 = cache_lines, translations0 = translations;
    const struct pvr_gpu_service_request open = {PVR_GPU_SERVICE_OPEN, mimus_service_slow ? 60000000 : 1000000};
    struct pvr_gpu_service_reply reply; uint64_t ticket;
    configASSERT(level() == 0 && pvr_gpu_service_send(&service, &open, 10, &ticket));
    while (!pvr_gpu_service_collect(&service, ticket, &reply, 10)) {}
    configASSERT(reply.result == PVR_GPU_SERVICE_OK);
    struct pvr_mimus_record r = receive();
    configASSERT(r.result == PVR_MIMUS_OK && !r.sequence && mimus->phase == PVR_MIMUS_SERVICE_IDLE);
    configASSERT(service.phase == PVR_GPU_SERVICE_READY && session.vm.phase == A733_GPU_VM_MEMORY_PUBLISHED);
    metric("open-cache-lines", cache_lines - lines0);
    metric("open-translations", translations - translations0);
    unsigned wb, nc; pool_pages(&wb, &nc);
    metric("pool-wb-pages", wb); metric("pool-nc-pages", nc);
    metric("table-pages", session.requirements.table_pages);
    configASSERT(wb <= 80 && nc <= 52);
#ifdef SUNSTONE_A733_OS_PERF
    if(c>=25)performance_client();
#endif
    if (c >= 21 && c <= 24) {
        configASSERT(bench_job(0));
        closed();
        const uint64_t old_epoch = service.epoch;
        const struct device_memory_lease payload = session.payload_lease, tables = session.table_lease;
        unsigned wb_before, nc_before; pool_pages(&wb_before, &nc_before);
        uint64_t cache_before = cache_lines;
        if (c == 22) {
            refuse_stopped = 1;
            configASSERT(reopen_reply(old_epoch).result == PVR_GPU_SERVICE_BUSY);
            refuse_stopped = 0;
            struct pvr_mimus_record stale = {1, PVR_MIMUS_OK};
            configASSERT(xQueueSend(mimus->completions, &stale, 0) == pdTRUE);
            configASSERT(reopen_reply(old_epoch).result == PVR_GPU_SERVICE_BUSY);
            configASSERT(pvr_mimus_service_receive(mimus, &stale, 0));
            ++session.payload_lease.generation;
            configASSERT(reopen_reply(old_epoch).result == PVR_GPU_SERVICE_BUSY);
            session.payload_lease = payload;
            configASSERT(cache_lines == cache_before && service.epoch == old_epoch &&
                         atomic_load(&mimus->slot_state) == PVR_MIMUS_SLOT_CLOSED);
        }
        if (c == 23) fail_reopen_cache = 1;
        if (c == 24) fail_reopen_post = 1;
        reply = reopen_reply(old_epoch);
        if (c == 23 || c == 24) {
            configASSERT(reply.result == (c == 23 ? PVR_GPU_SERVICE_FAULT : PVR_GPU_SERVICE_OK));
            r = receive();
            configASSERT(r.result == PVR_MIMUS_LOST && !fail_reopen_cache && !fail_reopen_post &&
                         service.phase == PVR_GPU_SERVICE_FAILED && atomic_load(&mimus->lost) &&
                         !pvr_mimus_service_receive(mimus, &r, 0));
            configASSERT(reopen_reply(service.epoch).result == PVR_GPU_SERVICE_FAULT);
        } else {
            configASSERT(reply.result == PVR_GPU_SERVICE_OK && reply.epoch == old_epoch + 1);
            r = receive();
            configASSERT(r.result == PVR_MIMUS_OK && r.sequence == 0 && mimus->sequence == 1 &&
                         atomic_load(&mimus->slot_state) == PVR_MIMUS_SLOT_IDLE);
            metric("first-reopen-cache-lines", cache_lines - cache_before);
            configASSERT(reopen_reply(old_epoch).result == PVR_GPU_SERVICE_INVALID);
            configASSERT(bench_job(1) && mimus->sequence == 2);
            if (c == 21) {
                closed();
                configASSERT(reopen_reply(service.epoch).result == PVR_GPU_SERVICE_OK);
                r = receive();
                configASSERT(r.result == PVR_MIMUS_OK && r.sequence == 0);
                configASSERT(bench_job(2) && mimus->sequence == 3);
            }
        }
        unsigned wb_after, nc_after; pool_pages(&wb_after, &nc_after);
        configASSERT(wb_after == wb_before && nc_after == nc_before &&
                     !memcmp(&payload, &session.payload_lease, sizeof(payload)) &&
                     !memcmp(&tables, &session.table_lease, sizeof(tables)) &&
                     session.vm.phase == A733_GPU_VM_MEMORY_PUBLISHED);
        metric("stopped-proofs", stopped_proofs);
        metric("restart-cache-lines", cache_lines - cache_before);
        metric("session-epoch", service.epoch);
        metric("job-sequence", mimus->sequence);
        platform_puts("PASS mimus service case "); platform_hex(c);
        platform_puts("; retained healthy reopen with monotonic job sequence\n"); platform_exit(0);
    }
    if (c >= 18 && c <= 20) {
        const struct pvr_mimus_request job = {0, fixture_jobs[0].ordinal,
            fixture_jobs[0].rows, 2 * fixture_jobs[0].k};
        if (c == 18) {
            atomic_store(&hold_slot, 1);
            atomic_store(&hold_completion, 1);
            configASSERT(pvr_mimus_service_submit(mimus, &job) == 1);
            configASSERT(close_reply().result == PVR_GPU_SERVICE_BUSY);
            configASSERT(service.phase == PVR_GPU_SERVICE_READY && !stopping && !kick_step_count &&
                         atomic_load(&mimus->slot_state) == PVR_MIMUS_SLOT_SUBMITTED);
            atomic_store(&hold_slot, 0);
            const struct pvr_gpu_service_message wake = {.client = PVR_GPU_SERVICE_CLIENTS,
                                                         .ticket = PVR_GPU_SERVICE_MIMUS_WAKE};
            configASSERT(xQueueSend(service.requests, &wake, 0) == pdTRUE);
            TickType_t wait_start = xTaskGetTickCount();
            while (mimus->phase != PVR_MIMUS_SERVICE_IN_FLIGHT) {
                configASSERT(xTaskGetTickCount() - wait_start < 3000);
                vTaskDelay(1);
            }
            configASSERT(close_reply().result == PVR_GPU_SERVICE_BUSY);
            configASSERT(service.phase == PVR_GPU_SERVICE_READY && !stopping && kick_step_count == 1 &&
                         mimus->phase == PVR_MIMUS_SERVICE_IN_FLIGHT &&
                         atomic_load(&mimus->slot_state) == PVR_MIMUS_SLOT_TAKEN);
            atomic_store(&hold_completion, 0);
            r = receive();
            configASSERT(r.result == PVR_MIMUS_OK && r.sequence == 1 && complete_calls == 1);
            configASSERT(close_reply().result == PVR_GPU_SERVICE_OK);
            configASSERT(atomic_load(&mimus->slot_state) == PVR_MIMUS_SLOT_CLOSED &&
                         pvr_mimus_service_submit(mimus, &job) == 0);
            TickType_t start = xTaskGetTickCount();
            while (service.phase != PVR_GPU_SERVICE_STOPPED) {
                configASSERT(service.phase != PVR_GPU_SERVICE_FAILED && xTaskGetTickCount() - start < 3000);
                vTaskDelay(1);
            }
            configASSERT(pvr_mimus_service_submit(mimus, &job) == 0 && !mimus->owed &&
                         !pvr_mimus_service_receive(mimus, &r, 0) && kick_step_count == 1);
        } else if (c == 19) {
            /* State injection models a producer paused after reservation.
             * It does not resume an interrupted submit stack. */
            atomic_store(&mimus->slot_state, PVR_MIMUS_SLOT_RESERVED);
            configASSERT(close_reply().result == PVR_GPU_SERVICE_BUSY);
            configASSERT(service.phase == PVR_GPU_SERVICE_READY && !stopping && !kick_step_count &&
                         atomic_load(&mimus->slot_state) == PVR_MIMUS_SLOT_RESERVED);
            pvr_mimus_service_abort(mimus);
            configASSERT(atomic_load(&mimus->lost) && !mimus->owed &&
                         atomic_load(&mimus->slot_state) == PVR_MIMUS_SLOT_RESERVED &&
                         pvr_mimus_service_submit(mimus, &job) == -1 &&
                         !pvr_mimus_service_receive(mimus, &r, 0));
        } else {
            configASSERT(pvr_mimus_service_submit(mimus, &job) == 1);
            r = receive();
            configASSERT(r.result == PVR_MIMUS_LOST && r.sequence == 1 &&
                         service.phase == PVR_GPU_SERVICE_FAILED);
            configASSERT(close_reply().result == PVR_GPU_SERVICE_FAULT);
            configASSERT(atomic_load(&mimus->slot_state) == PVR_MIMUS_SLOT_TAKEN &&
                         pvr_mimus_service_submit(mimus, &job) == -1 && !stopping &&
                         !pvr_mimus_service_receive(mimus, &r, 0));
        }
        unsigned wb_after, nc_after; pool_pages(&wb_after, &nc_after);
        configASSERT(wb_after == wb && nc_after == nc);
        platform_puts("PASS mimus service case "); platform_hex(c);
        platform_puts("; checked CLOSE admission and retained slot ownership\n"); platform_exit(0);
    }
    if (c == 10) { fixture_mark(3); platform_puts("PASS mimus service case "); platform_hex(c); platform_puts("; OPEN only\n"); platform_exit(0); }
    uint32_t waits0 = service.ready_timed_waits, expiries0 = service.ready_wait_expiries;
    uint64_t jobs_lines = cache_lines, jobs_translations = translations, jobs_cache = cache_calls;
    if (c >= 13 && c <= 17) {
        if (c == 15) {
            configASSERT(bench_job(0));
            configASSERT(completion_clock_samples == 2 && complete_calls == 1 &&
                         mimus->phase == PVR_MIMUS_SERVICE_IDLE && !atomic_load(&mimus->lost));
        } else {
            const struct pvr_mimus_request job = {0, fixture_jobs[0].ordinal,
                fixture_jobs[0].rows, 2 * fixture_jobs[0].k};
            configASSERT(pvr_mimus_service_submit(mimus, &job) == 1);
            r = receive();
            configASSERT(r.result == PVR_MIMUS_LOST && r.sequence == 1 &&
                         service.phase == PVR_GPU_SERVICE_FAILED &&
                         mimus->phase == PVR_MIMUS_SERVICE_FAILED && atomic_load(&mimus->lost));
            configASSERT(mimus->error == (c >= 16 ? PVR_MIMUS_SERVICE_CLOCK : PVR_MIMUS_SERVICE_DEADLINE));
            configASSERT(completion_clock_samples == (c == 13 ? 1u : 2u) &&
                         complete_calls == (c == 13 ? 0u : 1u));
            configASSERT(atomic_load(&mimus->slot_state) == PVR_MIMUS_SLOT_TAKEN &&
                         !pvr_mimus_service_receive(mimus, &r, 0) &&
                         pvr_mimus_service_submit(mimus, &job) == -1);
            unsigned wb_after, nc_after; pool_pages(&wb_after, &nc_after);
            configASSERT(wb_after == wb && nc_after == nc);
        }
        configASSERT(kick_step_count == 1 && submit_calls == 1);
        metric("completion-clock-samples", completion_clock_samples);
        metric("completion-cache-acquires", complete_calls);
        platform_puts("PASS mimus service case "); platform_hex(c);
        platform_puts("; one job with checked completion clock boundary\n"); platform_exit(0);
    }
    if (c <= 4) {
        fixture_mark(1);
        for (unsigned j = 0; j < FIXTURE_JOBS; ++j) { fixture_mark(2); configASSERT(bench_job(j)); }
        fixture_mark(3);
        metric("jobs", FIXTURE_JOBS);
        metric("sums-checked", sums_checked);
        metric("job-cache-calls", cache_calls - jobs_cache);
        metric("job-cache-lines", cache_lines - jobs_lines);
        metric("job-translations", translations - jobs_translations);
        metric("ready-timed-waits", service.ready_timed_waits - waits0);
        metric("ready-wait-expiries", service.ready_wait_expiries - expiries0);
        metric("interrupts", irq_count); metric("hook-wakes", hook_wakes);
        metric("kicks", kick_step_count); metric("ring-polls", ring_polls);
        metric("completions", complete_calls); metric("output-cache-calls", output_cache_calls);
        configASSERT(submit_calls == FIXTURE_JOBS && kick_step_count == FIXTURE_JOBS &&
                     complete_calls == FIXTURE_JOBS && output_cache_calls == FIXTURE_JOBS);
        platform_puts("PASS mimus service case "); platform_hex(c);
        platform_puts("; 204 bench jobs with every tag and sum\n"); platform_exit(0);
    }
    if (c == 5) {
        /* Rows above the tensor's bound and an ordinal outside the table are
         * refused before the ring: REFUSED, slot IDLE, no kick. */
        const struct pvr_mimus_tensor *t = &tensors[fixture_jobs[0].ordinal];
        const struct pvr_mimus_request above = {0, fixture_jobs[0].ordinal, t->job_rows + 1, 2 * t->k},
            outside = {0, FIXTURE_TENSORS, 32, 2 * t->k};
        configASSERT(pvr_mimus_service_submit(mimus, &above) == 1);
        r = receive(); configASSERT(r.result == PVR_MIMUS_REFUSED && r.sequence == 1);
        configASSERT(pvr_mimus_service_submit(mimus, &outside) == 1);
        r = receive(); configASSERT(r.result == PVR_MIMUS_REFUSED && r.sequence == 1);
        configASSERT(atomic_load(&mimus->slot_state) == PVR_MIMUS_SLOT_IDLE && !kick_step_count);
        /* The bad ordinal never reaches the session code; the row count does. */
        configASSERT(submit_calls == 1 && bench_job(0) && bench_job(1));
        platform_puts("PASS mimus service case "); platform_hex(c);
        platform_puts("; refused jobs leave the slot idle and later jobs run\n"); platform_exit(0);
    }
    if (c == 6) {
        /* A full request queue: the send fails, the slot is taken back. */
        vTaskSuspend(service.worker);
        const struct pvr_gpu_service_message wake = {.client = PVR_GPU_SERVICE_CLIENTS,
                                                     .ticket = PVR_GPU_SERVICE_MIMUS_WAKE};
        while (xQueueSend(service.requests, &wake, 0) == pdTRUE) {}
        const struct pvr_mimus_request job = {0, fixture_jobs[0].ordinal, fixture_jobs[0].rows, 2 * fixture_jobs[0].k};
        configASSERT(pvr_mimus_service_submit(mimus, &job) == 0);
        configASSERT(atomic_load(&mimus->slot_state) == PVR_MIMUS_SLOT_IDLE);
        vTaskResume(service.worker);
        vTaskDelay(5);
        configASSERT(!kick_step_count && !submit_calls && bench_job(0));
        platform_puts("PASS mimus service case "); platform_hex(c);
        platform_puts("; a failed wake send refuses the job with the slot idle\n"); platform_exit(0);
    }
    if (c == 7) {
        configASSERT(!bench_job(0));
        configASSERT(mimus->phase == PVR_MIMUS_SERVICE_IDLE && service.phase == PVR_GPU_SERVICE_READY);
        platform_puts("PASS mimus service case "); platform_hex(c);
        platform_puts("; a missing tag reaches the client with the session idle\n"); platform_exit(0);
    }
    /* 8, 9, 11, 12: the job is LOST; afterwards submit reports the loss. */
    unsigned lost_job = 0;
    if (c == 11) {
        /* A timeline the session did not expect, written while it is idle:
         * the next job is refused at its timeline check as LOST, before any
         * ring write. */
        configASSERT(bench_job(0));
        __atomic_store_n((uint32_t *)mimus->config->firmware[5].storage.cpu, completed + 5, __ATOMIC_RELEASE);
        lost_job = 1;
    }
    if (c == 12) __atomic_store_n(&fail_output_cache, 1, __ATOMIC_RELEASE);
    const struct pvr_mimus_request job = {0, fixture_jobs[lost_job].ordinal, fixture_jobs[lost_job].rows,
                                          2 * fixture_jobs[lost_job].k};
    uint32_t kicks_before = kick_step_count;
    configASSERT(pvr_mimus_service_submit(mimus, &job) == 1);
    uint64_t at; wait_record_time(&at, &r);
    configASSERT(r.result == PVR_MIMUS_LOST && service.phase == PVR_GPU_SERVICE_FAILED);
    configASSERT(mimus->error == (c == 8 ? PVR_MIMUS_SERVICE_DEADLINE : c == 9 ? PVR_MIMUS_SERVICE_RESET :
                                  c == 11 ? PVR_MIMUS_SERVICE_TIMELINE : PVR_MIMUS_SERVICE_MAINTENANCE));
    if (c == 11) configASSERT(kick_step_count == kicks_before && submit_calls == 1);
    if (c == 12) configASSERT(complete_calls == 1 && !__atomic_load_n(&fail_output_cache, __ATOMIC_ACQUIRE));
    metric("lost-after-kick-us", at - kick_us);
    if (c == 8) configASSERT(at - kick_us >= 1000000 && at - kick_us <= 1000000 + 1000 + 2000);
    configASSERT(pvr_mimus_service_submit(mimus, &job) == -1);
    platform_puts("PASS mimus service case "); platform_hex(c);
    platform_puts(c == 8 ? "; a hang ends in LOST after max_job_us and submit then reports the loss\n" :
                  c == 9 ? "; a context reset ends in LOST and submit then reports the loss\n" :
                  c == 11 ? "; an unexpected timeline ends the next job in LOST and submit then reports the loss\n" :
                  "; a failed completion clean-and-invalidate ends in LOST and submit then reports the loss\n");
    platform_exit(0);
}
/* Unused parts of the included model. */
void fixture_unused(void)
{ (void)firmware_fixture_owner; (void)firmware_fixture_event; (void)firmware_fixture_irq_read;
  (void)firmware_fixture_irq_write; (void)peer; (void)supervisor; }

void kernel_main(void)
{
    platform_init(); first_publish = 0;
    configASSERT(mimus_service_case >= 1 && mimus_service_case <= 30);
    /* Synthetic weights over both 8 MiB model regions. */
    uint64_t *weights = (uint64_t *)(uintptr_t)0x44000000u;
    if (mimus_service_case == 1) for (size_t i = 0; i < (16u << 20) / 8; ++i) weights[i] = next_random();
    memset((void *)(uintptr_t)PVR_MIMUS_OUTPUT_GPU, 0, PVR_MIMUS_TAG_BYTES);
    /* SPI 95: group 1, an API-safe priority, routed to core 0, disabled. */
    volatile uint32_t *d = (volatile uint32_t *)(uintptr_t)GICD;
    d[(0x80 + 4 * (95 / 32)) / 4] |= 1u << (95 % 32);
    *(volatile uint8_t *)(uintptr_t)(GICD + 0x400 + 95) = portLOWEST_USABLE_INTERRUPT_PRIORITY << portPRIORITY_SHIFT;
    *(volatile uint64_t *)(uintptr_t)(GICD + 0x6000 + 8 * 95) = 0;
    d[(0x180 + 4 * (95 / 32)) / 4] = 1u << (95 % 32);
    __asm__ volatile("dsb sy" ::: "memory");
    for (unsigned i = 0; i < FIXTURE_GPU_TENSORS; ++i) {
        const struct fixture_tensor *t = &fixture_tensors[i];
        configASSERT(pvr_mimus_tensor_open(&tensors[t->ordinal], t->region, t->offset, t->stride, t->k, t->n_gpu));
        configASSERT(tensors[t->ordinal].job_rows >= t->job_rows);
    }
    struct pvr_gpu_service_config *rc = a733_gpu_reserved_prepare(&firmware, 9, gpu_firmware,
        (size_t)(gpu_firmware_end - gpu_firmware), 400000000);
    configASSERT(rc);
    const struct a733_gpu_mimus_layout layout = {{0x44000000u, 0x44800000u}, PVR_MIMUS_ARENA_GPU,
        PVR_MIMUS_OUTPUT_GPU, tensors, FIXTURE_TENSORS, 1000000};
    mimus = a733_gpu_mimus_prepare(&session, &firmware, &service, 10, &layout);
    metric("prepare-stage", session.stage); metric("prepare-table-pages", session.requirements.table_pages);
    configASSERT(mimus && mimus->phase == PVR_MIMUS_SERVICE_BOUND && firmware.config.mimus == mimus);
    config = firmware.config;
    config.queue = (struct pvr_ccb_ops){read_mem, write_mem, barrier, kick};
    config.start = (struct pvr_gpu_start_ops){boundary_now, read_reg, write_reg, started, publish};
    config.control = (struct pvr_gpu_control_ops){boundary_now, irq_read, irq_write, compat, event};
    config.stop = (struct pvr_gpu_stop_ops){boundary_now, read_reg, write_reg, sync_write, sync_read, isolate, NULL};
    config.enable = enable; config.context = &config; config.compute_mode = mode;
    config.irq = (struct pvr_gpu_service_irq_ops){mask, rearm};
    configASSERT(a733_gpu_memory_init(&memory, config.firmware_buffers, config.resources));
    /* The component was bound to the reserved configuration; the service
     * copies this one, which carries the same pointer. */
#ifdef SUNSTONE_A733_OS_PERF
    config.performance_clock=(struct pvr_gpu_clock_ops){boundary_now,sync_write,sync_read,perf_state,perf_publish};
    config.performance_begin=perf_begin;config.performance_poll=perf_poll;config.performance_rate=perf_current;config.performance_client=1;
#endif
    configASSERT(pvr_gpu_service_init(&service, &config, 4, 1));
    TaskHandle_t a = xTaskCreateStaticAffinitySet(client, "client", 4096, NULL, 3, mimus_stacks[0], &mimus_tasks[0], 1);
    TaskHandle_t m = xTaskCreateStaticAffinitySet(model, "model", 4096, NULL, 6, mimus_stacks[1], &mimus_tasks[1], 2);
    configASSERT(a && m && pvr_gpu_service_bind(&service, 0, a));
#ifdef SUNSTONE_A733_OS_PERF
    TaskHandle_t thermal=xTaskCreateStaticAffinitySet(performance_owner,"thermal",4096,NULL,5,perf_stack,&perf_task,1);
    configASSERT(thermal && pvr_gpu_service_bind(&service,1,thermal));
#endif
    vTaskStartScheduler(); platform_panic("scheduler returned", 0);
}
