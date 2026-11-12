/* Two-core FreeRTOS GPU queue check. Native firmware allocation, GPU maps,
 * cache maintenance and client-ring production execute. The firmware and
 * shader model accepts commands, computes a synthetic linear grid, publishes
 * output and timeline, and raises SPI 95. The client checks tags and every
 * sum. Model effects establish software behavior, not physical qualification. */
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
#include "pvr_compute_queue.h"
#include "a733_gpu_queue.h"
#include "a733_gpu_backend.h"
#include "gicv3_spi.h"
#include "queue-fixture.h"
#include "sunstone_latency.h"

/* 1 grid with exact sums; 2 grid with pattern sums (instrumented runs);
 * 3 as 2 with jobs that complete at once; 4 as 3 with one firmware event per
 * job; 5 refused jobs; 6 coalesced wake; 7 missing tag; 8 hang; 9 context
 * reset; 10 OPEN only; 11 an unexpected timeline value before a submit; 12 the
 * completion's clean and invalidate fails; 13 completion at its deadline;
 * 14 output acquisition reaches the deadline; 15 completion just before it;
 * 16 failed final clock read; 17 backward final clock sample; 18 pending,
 * active and drained CLOSE; 19 reserved-slot CLOSE and abort; 20 lost CLOSE. */
volatile uint32_t gpu_queue_case = 1;
/* Nonzero in the runner's instrumented runs, which execute many times slower
 * against wall-clock deadlines: OPEN then gets 60 s instead of 1 s. */
__attribute__((section(".data"))) volatile uint32_t gpu_queue_slow = 0;
static struct a733_gpu_reserved firmware;
static struct a733_gpu_queue session;
static struct pvr_compute_queue *compute;
static struct fixture_tensor tensors[FIXTURE_TENSORS];
static struct gpu_queue_context context;
#define FIXTURE_WEIGHTS UINT64_C(0x44000000)
#define FIXTURE_INPUT UINT64_C(0x45000000)
#define FIXTURE_OUTPUT UINT64_C(0x46000000)
#define FIXTURE_OUTPUT_BYTES (2u << 20)
#define FIXTURE_TAG_BYTES 65536u
#define FIXTURE_RING_BYTES 1024u
__attribute__((section("sunstone_gpu_program"),aligned(4),used))
static const uint32_t program_code[] = {0};
extern const unsigned char __start_sunstone_gpu_program[],__stop_sunstone_gpu_program[];
static const struct gpu_queue_program program = {
    .abi=GPU_QUEUE_ABI_GRID,.code_bytes=sizeof(program_code),.temps=1,
    .coefficients=3,.local_size={32,1,1},.code=program_code
};
static struct ss_region regions[5] = {
    {.address=FIXTURE_OUTPUT,.bytes=FIXTURE_OUTPUT_BYTES,.kind=SS_REGION_OUTPUT,
     .access=SS_ACCESS_READ|SS_ACCESS_GPU_WRITE},
    {.address=FIXTURE_WEIGHTS,.bytes=8u<<20,.kind=SS_REGION_INPUT,
     .access=SS_ACCESS_READ|SS_ACCESS_GPU_READ},
    {.address=FIXTURE_WEIGHTS+(8u<<20),.bytes=8u<<20,.kind=SS_REGION_INPUT,
     .access=SS_ACCESS_READ|SS_ACCESS_GPU_READ},
    {.address=FIXTURE_INPUT,.bytes=12u<<20,.kind=SS_REGION_WORK,
     .access=SS_ACCESS_READ|SS_ACCESS_WRITE|SS_ACCESS_GPU_READ}
};
static StaticTask_t queue_tasks[3];
static StackType_t queue_stacks[3][4096];
static atomic_uint peer_release,peer_submitted,peer_finished;
static unsigned reset_readbacks, fault_records;
static const struct gicv3_spi spi = {0x08000000, 95};
#define GICD 0x08000000u
static int fast_kick(void *,unsigned,unsigned,unsigned char *);
static void fast_model(void *);
static void fast_cases(unsigned);
static void fast_peer(void);
static struct gpu_queue_job fast_job(unsigned,unsigned);

/* GPU and firmware model state. */
static uint32_t kicked, run, gpu_status, irq_count, hook_wakes, kick_step_count;
static uint64_t kick_us;
static uint32_t last_sequence;
static atomic_uint hold_slot, hold_completion;
static uint32_t submit_calls, complete_calls;
static unsigned refuse_stopped, fail_reopen_cache, fail_reopen_post, stopped_proofs;
BaseType_t __real_xQueueGenericSend(QueueHandle_t, const void *, TickType_t, BaseType_t);
BaseType_t __wrap_xQueueGenericSend(QueueHandle_t queue, const void *item, TickType_t wait, BaseType_t position)
{
    if (gpu_queue_case>=41 && gpu_queue_case<=50 && compute && queue==compute->completions &&
        ((const struct gpu_queue_record *)item)->result==GPU_QUEUE_FAULT) {
        configASSERT(service.phase==PVR_GPU_SERVICE_RECOVERING && reset_readbacks && isolated &&
            service.stop.phase==PVR_GPU_STOP_STOPPED && memory.phase==A733_GPU_MEMORY_OFFLINE &&
            service.irq_masked && !service.irq_fault && compute->owed==0);
        ++fault_records;
    }
    if (fail_reopen_post && service.epoch == 1 && queue == compute->completions) {
        const struct gpu_queue_record *record = item;
        configASSERT(!record->sequence && record->result == GPU_QUEUE_OK);
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
    return !refuse_stopped && s == &service && (s->phase == PVR_GPU_SERVICE_STOPPED || s->phase==PVR_GPU_SERVICE_RECOVERING
#ifdef GPU_QUEUE_RETURNING
        || s->phase==PVR_GPU_SERVICE_PARKING || s->phase==PVR_GPU_SERVICE_PARKED
#endif
        ) &&
        s->stop.phase == PVR_GPU_STOP_STOPPED && s->stop.isolation_observed &&
        (s->stop.unacknowledged ?
            (reset_readbacks && s->queue.phase==PVR_CCB_FAILED && s->control.phase==PVR_GPU_CONTROL_FAILED) :
            (s->stop.acknowledgement[0]==1 && s->stop.acknowledgement[1]==1 &&
             s->queue.phase==PVR_CCB_RETIRED && s->control.phase==PVR_GPU_CONTROL_RETIRED)) &&
        memory.phase == A733_GPU_MEMORY_OFFLINE && s->irq_masked && !s->irq_fault;
}
int __real_pvr_compute_queue_poll(struct pvr_compute_queue *);
int __wrap_pvr_compute_queue_poll(struct pvr_compute_queue *m)
{
    if (atomic_load(&hold_slot) && m->phase == PVR_COMPUTE_QUEUE_READY) return 0;
    unsigned before=m->phase;
    int result=__real_pvr_compute_queue_poll(m);
    if(before==PVR_COMPUTE_QUEUE_READY && m->phase==PVR_COMPUTE_QUEUE_IN_FLIGHT)++submit_calls;
    return result;
}
/* Selected boundary cases advance the actual service clock callback only
 * after the client ring retired and the modeled timeline completed. The
 * normal service clock wrapper still enforces monotonicity. */
static unsigned completion_clock, completion_clock_samples, in_ring_poll;
static int boundary_now(void *ctx, uint64_t *out)
{
    if (!completion_clock) return now(ctx, out);
    callback(ctx);
    if (completion_clock == 3) return 0;
    *out = completion_clock == 2 ? compute->deadline :
           completion_clock == 4 ? compute->deadline - 2 : compute->deadline - 1;
    return 1;
}
int __real_pvr_gpu_service_now(struct pvr_gpu_service *, uint64_t *);
int __wrap_pvr_gpu_service_now(struct pvr_gpu_service *s, uint64_t *out)
{
    unsigned c = gpu_queue_case;
    if (((c >= 13 && c <= 17) || c == 20) && compute && !in_ring_poll && s == &service &&
        compute->phase == PVR_COMPUTE_QUEUE_IN_FLIGHT && compute->client.phase == PVR_CCCB_IDLE &&
        *(const volatile uint32_t *)compute->firmware[5].storage.cpu == compute->sequence) {
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
static int recovery_admit(void *ctx,uint64_t *limit)
{
    callback(ctx); configASSERT(service.phase==PVR_GPU_SERVICE_READY && service.irq_masked);
    stopping=1;
    *limit=now_us()+(gpu_queue_case==49 ? 1 : UINT64_C(3000000));
    return 1;
}
static int recovery_read(void *ctx,uint32_t off,uint32_t width,uint64_t *out)
{
    if (!read_reg(ctx,off,width,out)) return 0;
    if (service.phase==PVR_GPU_SERVICE_RECOVERING) {
        if (gpu_queue_case==44 && off==0x3c8) *out=0;
        if (off==0x100) {
            if (gpu_queue_case==45) return 0;
            configASSERT(*out==UINT64_C(0xffeffffffffffc3d));
            *out=0xe8000020; /* A completed read need not echo the write mask. */
            ++reset_readbacks;
        }
    }
    return 1;
}
static int recovery_isolate(void *ctx)
{
    if (!service.stop.unacknowledged) return isolate(ctx);
    callback(ctx);
    configASSERT(service.phase==PVR_GPU_SERVICE_RECOVERING && reset_readbacks && stopping &&
        word(registers+0x100,8)==UINT64_C(0xffeffffffffffc3d) &&
        service.queue.phase==PVR_CCB_FAILED && service.control.phase==PVR_GPU_CONTROL_FAILED &&
        service.irq_masked && !fault_records && atomic_load(&compute->client_owed) &&
        !uxQueueMessagesWaiting(compute->completions));
    if (gpu_queue_case==46) return 0;
    running=0; __atomic_fetch_add(&isolated,1,__ATOMIC_RELEASE);
    return a733_gpu_memory_reclaim(&memory);
}
static int recovery_enable(void *ctx)
{ return gpu_queue_case==47 && service.epoch==2 ? 0 : enable(ctx); }
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
static int job_irq_ack(void *ctx)
{ configASSERT(ctx==&config); set(&gpu_status,0); __asm__ volatile("dsb sy" ::: "memory"); return 1; }
static int job_now(void *ctx,uint64_t *out)
{ configASSERT(ctx==&config);*out=now_us();return 1; }
static int job_kick(void *ctx)
{
    unsigned slot=((uint32_t)word(resource(0),4)-1)&127;
    return fast_kick(ctx,0xb00,0,resource(1)+slot*88);
}
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
#ifdef PVR_GPU_PERFORMANCE
static int perf_kick(void *,unsigned,unsigned,unsigned char *);
#endif
static int kick(void *ctx, uint32_t off, uint32_t value)
{
    unsigned next = (unsigned)word(resource(0), 4), slot = (next - 1) & 127;
    unsigned char *command = resource(1) + slot * 88;
    if (gpu_queue_case>=54 && (gpu_queue_case<=68||gpu_queue_case==73||gpu_queue_case==75) && word(command,4)==(PVR_CCB_MAGIC|101u)) return fast_kick(ctx,off,value,command);
#ifdef PVR_GPU_PERFORMANCE
    if(perf_kick(ctx,next,slot,command))return 1;
#endif
    if (word(command, 4) != (PVR_CCB_MAGIC | 101u)) {
        if (gpu_queue_case==48 && service.epoch==2) return 1;
        return firmware_fixture_kick(ctx, off, value);
    }
    callback(ctx); configASSERT(off == 0xb00 && !value && running);
    const struct pvr_compute_queue *c = compute;
    configASSERT(word(command + 8, 4) == c->firmware_address[0]);
    configASSERT(word(command + 16, 4) == FIXTURE_RING_BYTES - 1);
    uint32_t cursor = (uint32_t)word(command + 12, 4);
    configASSERT(cursor >= 184 && cursor <= FIXTURE_RING_BYTES);
    unsigned char *ring = (unsigned char *)c->firmware[3].storage.cpu + cursor - 184;
    configASSERT(word(ring, 4) == (PVR_CCB_MAGIC | PVR_CCCB_TASK | 205u) && word(ring + 4, 4) == 96);
    configASSERT(!memcmp(ring + 40, compute->command, 96));
    configASSERT(word(ring + 136, 4) == (PVR_CCB_MAGIC | 213u) && word(ring + 140, 4) == 8);
    configASSERT(word(ring + 176, 4) == c->firmware_address[5]);
    uint32_t sequence = (uint32_t)word(ring + 180, 4);
    configASSERT(sequence == last_sequence + 1);
    last_sequence = sequence;
    unsigned char *control = c->firmware[2].storage.cpu;
    put(control, 4, cursor); put(control + 4, 4, cursor % FIXTURE_RING_BYTES);
    if (!(gpu_queue_case==50 && sequence==1)) {
        put(resource(0)+128,4,next);put(resource(2)+slot*4,4,1);fixture_executed();
    }
    kick_us = now_us();
    ++kick_step_count;
    __atomic_store_n(&kicked, sequence, __ATOMIC_RELEASE);
    return 1;
}
static uint32_t lane_tags(uint32_t rows, uint32_t n) { return ((n + 15u) / 16u) * ((rows + 31u) / 32u) * 32u; }
static int64_t pattern(uint32_t sequence, uint32_t r, uint32_t c)
{ return (int64_t)(sequence * 131u + r * 7u + c) - 5000; }
/* Weights of a GPU address, at the fixture's model regions. */
static const int8_t *weights_cpu(uint64_t gpu)
{ return (const int8_t *)(uintptr_t)gpu; }
static void model(void *unused)
{
    if (gpu_queue_case>=54 && (gpu_queue_case<=68||gpu_queue_case==73||gpu_queue_case==75)) fast_model(unused);
    (void)unused;
    for (;;) {
        while (__atomic_load_n(&kicked, __ATOMIC_ACQUIRE) == run) {}
        uint32_t sequence = __atomic_load_n(&kicked, __ATOMIC_ACQUIRE);
        uint64_t started = kick_us;
        const unsigned char *words = compute->job_cpu + PVR_COMPUTE_QUEUE_ARGUMENTS;
        uint64_t w_gpu = word(words, 8), x_gpu = word(words + 8, 8);
        uint32_t k = (uint32_t)word(words + 16, 2), n = (uint32_t)word(words + 18, 2);
        uint32_t rows = (uint32_t)word(words + 20, 4), ws = (uint32_t)word(words + 24, 2);
        uint32_t xs = (uint32_t)word(words + 26, 2);
        configASSERT((uint32_t)word(words + 28, 4) == sequence);
        configASSERT(x_gpu >= FIXTURE_INPUT && w_gpu >= FIXTURE_WEIGHTS);
        unsigned job = (sequence - 1)%FIXTURE_JOBS, c = gpu_queue_case;
        configASSERT(job < FIXTURE_JOBS && fixture_jobs[job].rows == rows && fixture_jobs[job].k == k &&
                     fixture_jobs[job].n_gpu == n);
        const int8_t *wt = weights_cpu(w_gpu);
        const int16_t *x = (const int16_t *)(uintptr_t)x_gpu;
        unsigned char *sums = (unsigned char *)(uintptr_t)(FIXTURE_OUTPUT + FIXTURE_TAG_BYTES);
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
        uint32_t *tag = (uint32_t *)(uintptr_t)(FIXTURE_OUTPUT + FIXTURE_TAG_BYTES) - tags;
        for (uint32_t i = 0; i < tags; ++i) tag[i] = (c == 7 && i == 0) ? sequence - 1 : sequence;
        uint64_t duration = c>=41 && sequence==1 ? 5000 : c==3 || c==4 ? 0 : fixture_jobs[job].duration_us;
        while (now_us() < started + duration) {}
        while (atomic_load(&hold_completion)) {}
        __asm__ volatile("dsb sy" ::: "memory");
        if (c == 4 || c == 9 || c == 30 || (c==42 && sequence==1)) {
            unsigned write = get((uint32_t *)resource(PVR_FW_EVENT_CONTROL));
            unsigned char *e = resource(PVR_FW_EVENTS) + (write & 31) * 88;
            memset(e, 0, 88);
            if (c == 4) { put(e, 4, PVR_CCB_MAGIC | 107); put(e + 8, 4, 1); put(e + 16, 4, 1); }
            else { put(e, 4, PVR_CCB_MAGIC | 105); put(e + 8, 4, compute->firmware_address[0]); }
            __atomic_store_n((uint32_t *)resource(PVR_FW_EVENT_CONTROL), (write + 1) & 31, __ATOMIC_RELEASE);
        }
        if (c != 8 && c != 9 && c != 30 && !(c==42 && sequence==1)) put(compute->firmware[5].storage.cpu, 4, sequence);
        __asm__ volatile("dsb sy" ::: "memory");
        set(&gpu_status, 1);
        /* Level interrupt: pending until the handler masks it. */
        *(volatile uint32_t *)(uintptr_t)(GICD + 0x200 + 4 * (95 / 32)) = 1u << (95 % 32);
        __asm__ volatile("dsb sy" ::: "memory");
        run = sequence;
    }
}

/* Client: the client's side, as apps/os/gpu.c and apps/os/gpu_hook.cc. */
static uint64_t seed = 0x9e3779b97f4a7c15u;
static uint64_t next_random(void) { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; }
static struct gpu_queue_job make_job(unsigned index)
{
    const struct fixture_job *j = &fixture_jobs[index];
    const struct fixture_tensor *t = &tensors[j->ordinal];
    uint32_t tags=lane_tags(j->rows,j->n_gpu), bytes=j->rows*j->n_gpu*(j->k>512?8u:4u);
    struct gpu_queue_job job = {.sequence=index+1,.bindings=3,.argument_bytes=16,
        .groups={(j->n_gpu+15u)/16u,(j->rows+31u)/32u,1}};
    job.binding[0]=(struct gpu_queue_binding){.lease=0,.sync_offset=FIXTURE_TAG_BYTES-4*tags,.sync_bytes=4*tags+bytes};
    job.binding[1]=(struct gpu_queue_binding){.lease=(uint16_t)(t->region+1),.address=t->offset};
    job.binding[2]=(struct gpu_queue_binding){.lease=3,.sync_bytes=(j->rows*2u*j->k+63u)&~63u};
    put(job.arguments,4,j->k|((uint32_t)j->n_gpu<<16)); put(job.arguments+4,4,j->rows);
    put(job.arguments+8,4,t->stride|((2u*j->k)<<16)); put(job.arguments+12,4,job.sequence);
    return job;
}
static int submit(const struct gpu_queue_job *j)
{
    int result = gpu_queue_submit(context,j);
    return result==GPU_QUEUE_OK ? 1 : result==GPU_QUEUE_FAULT ? -1 : 0;
}
static int receive_now(struct gpu_queue_record *r, TickType_t wait)
{
    unsigned owed=atomic_load(&compute->client_owed);
    int result=gpu_queue_wait(context,r,wait);
    return owed && !atomic_load(&compute->client_owed) &&
        (result==GPU_QUEUE_OK || result==GPU_QUEUE_REFUSED || result==GPU_QUEUE_FAULT);
}
static int tags_complete(uint32_t rows,uint32_t n,uint32_t sequence)
{
    uint32_t tags=lane_tags(rows,n);
    const volatile uint32_t *tag=(const volatile uint32_t *)(uintptr_t)(FIXTURE_OUTPUT+FIXTURE_TAG_BYTES)-tags;
    for(uint32_t i=0;i<tags;++i)if(tag[i]!=sequence)return 0;
    return 1;
}
static struct gpu_queue_record receive(void)
{
    struct gpu_queue_record r;
    TickType_t start = xTaskGetTickCount();
    while (!receive_now(&r, 10))
        configASSERT(xTaskGetTickCount() - start < (gpu_queue_slow ? 60000u : 3000u));
    return r;
}
static uint64_t retained_failure(void)
{
    TickType_t start=xTaskGetTickCount();
    while (!atomic_load(&compute->lost)) { configASSERT(xTaskGetTickCount()-start<3000);vTaskDelay(1); }
    struct gpu_queue_record record={0xfeed,0xabcd,0xdcba,0x1234,0x5678},before=record;
    configASSERT(gpu_queue_wait(context,&record,0)==GPU_QUEUE_TIMEOUT &&
        !memcmp(&record,&before,sizeof(record)) && atomic_load(&compute->client_owed) &&
        !uxQueueMessagesWaiting(compute->completions) && !isolated);
    return now_us();
}
static uint32_t sums_checked;
static int sums_equal(unsigned job, uint32_t sequence)
{
    const struct fixture_tensor *t = &tensors[fixture_jobs[job].ordinal];
    uint32_t rows = fixture_jobs[job].rows, n = t->n_gpu, k = t->k, width = k > 512 ? 8 : 4;
    const unsigned char *base = (const unsigned char *)(uintptr_t)(FIXTURE_OUTPUT + FIXTURE_TAG_BYTES);
    const int8_t *wt = weights_cpu(FIXTURE_WEIGHTS + (uint64_t)t->region * (8u << 20) + t->offset);
    for (uint32_t r = 0; r < rows; ++r)
        for (uint32_t c = 0; c < n; ++c) {
            int64_t s = 0;
            if (gpu_queue_case == 1) {
                const int16_t *x = (const int16_t *)(uintptr_t)(FIXTURE_INPUT + (size_t)r * 2 * k);
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
static void shared_peer(void *unused)
{
    if (gpu_queue_case==59) fast_peer();
    (void)unused;
    while (!atomic_load(&peer_release)) vTaskDelay(1);
    struct gpu_queue_context handle;
    struct gpu_queue_lease_info leases[GPU_QUEUE_LEASES];
    configASSERT(gpu_queue_context_join(&handle,leases)==GPU_QUEUE_OK && handle.client==1);
    struct gpu_queue_job job=make_job(1);
    job.sequence=1;
    if (gpu_queue_case==43) job.not_after_us=now_us()+10000;
    configASSERT(gpu_queue_submit(handle,&job)==GPU_QUEUE_OK);
    atomic_store(&peer_submitted,1);
    struct gpu_queue_record record;
    for (;;) {
        int r=gpu_queue_wait(handle,&record,10);
        if (r!=GPU_QUEUE_TIMEOUT) {
            configASSERT(record.sequence==1);
            if (gpu_queue_case==43) configASSERT(r==GPU_QUEUE_REFUSED && record.detail==GPU_QUEUE_DEADLINE);
            else configASSERT(r==GPU_QUEUE_OK && sums_equal(1,2) && tags_complete(fixture_jobs[1].rows,fixture_jobs[1].n_gpu,2));
            break;
        }
        if (atomic_load(&compute->lost)) break;
    }
    atomic_store(&peer_finished,1);
    vTaskSuspend(NULL);
}
static void lifecycle_cases(unsigned c)
{
    if (c==52) {
        const struct gpu_queue_job job=make_job(0);
        configASSERT(gpu_queue_submit(context,&job)==GPU_QUEUE_OK);
        (void)retained_failure();
        configASSERT(!service.recovery_detected && !fault_records && !reset_readbacks && !isolated &&
            service.epoch==1 && kick_step_count==1 && !atomic_load(&compute->peer.client_owed));
    } else if (c==51) {
        compute->timeline_serial=UINT32_MAX;
        const struct gpu_queue_job job=make_job(0);
        configASSERT(gpu_queue_submit(context,&job)==GPU_QUEUE_OK);
        struct gpu_queue_record record=receive();
        configASSERT(record.result==GPU_QUEUE_REFUSED && record.detail==GPU_QUEUE_SEQUENCE && record.sequence==1 &&
            !kick_step_count && !atomic_load(&compute->lost) && !atomic_load(&compute->application_sequence) &&
            compute->timeline_serial==UINT32_MAX);
    } else if (c==39) {
        for (unsigned i=0;i<10;++i) {
            struct gpu_queue_job job=make_job(i%FIXTURE_JOBS); job.sequence=i+1;
            put(job.arguments+12,4,i+1);
            configASSERT(gpu_queue_submit(context,&job)==GPU_QUEUE_OK);
            struct gpu_queue_record r=receive();
            configASSERT(r.result==GPU_QUEUE_OK && r.sequence==i+1);
            configASSERT(gpu_queue_close(context,3000)==GPU_QUEUE_OK && pvr_compute_queue_clients_idle(compute));
            if (i!=9) configASSERT(gpu_queue_open(context,3000)==GPU_QUEUE_OK);
        }
        configASSERT(service.epoch==10 && compute->completed_jobs==10 && kick_step_count==10);
    } else {
        if (c==40) atomic_store(&hold_slot,1);
        struct gpu_queue_job job=make_job(0);
        configASSERT(gpu_queue_submit(context,&job)==GPU_QUEUE_OK);
        atomic_store(&peer_release,1);
        TickType_t start=xTaskGetTickCount();
        while (!atomic_load(&peer_submitted)) { configASSERT(xTaskGetTickCount()-start<1000); vTaskDelay(1); }
        if (c==40) {
            configASSERT(atomic_load(&compute->peer.slot_state)==PVR_COMPUTE_QUEUE_SUBMITTED);
            atomic_store(&hold_slot,0);
            const struct pvr_gpu_service_message wake={.client=PVR_GPU_SERVICE_CLIENTS,.ticket=PVR_GPU_SERVICE_QUEUE_WAKE};
            configASSERT(xQueueSend(service.requests,&wake,0)==pdTRUE);
        }
        if ((c>=44 && c<=46) || c==49) {
            while (!atomic_load(&compute->lost)) { configASSERT(xTaskGetTickCount()-start<2000); vTaskDelay(1); }
            struct gpu_queue_record record;
            configASSERT(gpu_queue_wait(context,&record,0)==GPU_QUEUE_TIMEOUT && atomic_load(&compute->client_owed) &&
                atomic_load(&compute->peer.client_owed) && !fault_records && !isolated &&
                atomic_load(&compute->slot_state)==PVR_COMPUTE_QUEUE_TAKEN &&
                atomic_load(&compute->peer.slot_state)==PVR_COMPUTE_QUEUE_SUBMITTED && kick_step_count==1 &&
                memory.phase==A733_GPU_MEMORY_PUBLISHED && service.phase==PVR_GPU_SERVICE_FAILED);
            configASSERT(service.recovery_stop_deadline<=service.recovery_detected+PVR_GPU_SERVICE_RECOVERY_STOP_US &&
                service.recovery_ready_deadline<=service.recovery_detected+PVR_GPU_SERVICE_RECOVERY_READY_US);
            if (c==49) configASSERT(service.recovery_stop_deadline==service.recovery_ready_deadline);
        } else {
            struct gpu_queue_record record=receive();
            configASSERT(record.sequence==1 && record.result==(c==40 ? GPU_QUEUE_OK : GPU_QUEUE_FAULT));
            if (c!=40) {
                configASSERT(record.detail==(c==42 ? GPU_QUEUE_RESET : GPU_QUEUE_HANG));
                int recovered=gpu_queue_recover(context,3000);
                if (c==47 || c==48) {
                    configASSERT(recovered==GPU_QUEUE_FAULT && fault_records==1 && isolated &&
                        atomic_load(&compute->peer.client_owed) && kick_step_count==1 &&
                        atomic_load(&compute->peer.slot_state)==PVR_COMPUTE_QUEUE_SUBMITTED);
                    goto done;
                }
                configASSERT(recovered==GPU_QUEUE_OK && service.recovery_count==1 && service.epoch==2 &&
                    service.flush.phase==PVR_GPU_FLUSH_COMPLETE && get(&mmu_flushes)==2 && fault_records==1);
            }
            while (!atomic_load(&peer_finished)) { configASSERT(xTaskGetTickCount()-start<3000); vTaskDelay(1); }
            unsigned serial=c==43 ? 2 : 3;
            job=make_job(serial-1); job.sequence=2;
            configASSERT(gpu_queue_submit(context,&job)==GPU_QUEUE_OK);
            record=receive();
            configASSERT(record.result==GPU_QUEUE_OK && record.sequence==2 && compute->timeline_serial==serial &&
                sums_equal(serial-1,serial) && tags_complete(fixture_jobs[serial-1].rows,fixture_jobs[serial-1].n_gpu,serial));
            configASSERT(gpu_queue_close(context,3000)==GPU_QUEUE_OK && pvr_compute_queue_clients_idle(compute));
            configASSERT(compute->completed_jobs==(c==40 ? 3 : c==43 ? 1 : 2));
        }
    }
done:
    metric("successful-jobs",compute->completed_jobs); metric("fault-records",fault_records);
    metric("reset-readbacks",reset_readbacks); metric("service-epoch",service.epoch);
    metric("timeline-serial",compute->timeline_serial); metric("kicks",kick_step_count);
    platform_puts("PASS GPU queue case ");platform_hex(c);platform_puts("; lifecycle ownership and bounded recovery\n");
    platform_exit(0);
}
static uint32_t completed;
/* One grid job: 1 when it completed with every tag and sum. */
static int grid_job(unsigned job)
{
    const unsigned c = gpu_queue_case;
    uint32_t k = fixture_jobs[job].k, rows = fixture_jobs[job].rows;
    if (c == 1) {
        int16_t *x = (int16_t *)(uintptr_t)FIXTURE_INPUT;
        for (size_t i = 0; i < (size_t)rows * k; ++i)
            x[i] = i < k ? 32767 : (int16_t)(next_random() % 65535 - 32767);
    }
    const struct gpu_queue_job request = make_job(job);
    uint64_t submitted = now_us();
    configASSERT(submit(&request) == 1);
    __atomic_fetch_add(&hook_wakes, 1, __ATOMIC_RELAXED);
    struct gpu_queue_record r = receive();
    if (r.result != GPU_QUEUE_OK || r.sequence != completed + 1) {
        metric("failed-job", job); metric("failed-result", r.result); metric("failed-sequence", r.sequence);
        metric("component-error", compute->error); metric("service-error", service.error);
        metric("control-error", service.control.error); metric("queue-phase", service.queue.phase);
        metric("ring-error", compute->client.error);
        metric("kicks-seen", kick_step_count); metric("since-kick-us", now_us() - kick_us);
        metric("model-run", run); metric("irqs", irq_count); metric("since-submit-us", now_us() - submitted);
        metric("kick-after-submit-us", kick_us - submitted);
    }
    configASSERT(r.result == GPU_QUEUE_OK && r.sequence == completed + 1);
    completed = r.sequence;
    if (!tags_complete(rows, fixture_jobs[job].n_gpu, r.sequence)) return 0;
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
/* --wrap counters of the two architecture helpers and the queue.
 * Every maintenance call on the output region must clean and invalidate:
 * DC IVAC is checked as a write and faults on the image's read-only view.
 * Case 12 makes the first such call of a job fail once. */
uint64_t cache_calls, cache_lines, translations, output_cache_calls, payload_cache_calls, dirty_input_cache_calls;
uint32_t ring_polls, fail_output_cache;
int __real_aarch64_device_cache(enum aarch64_cache_op, uintptr_t, size_t);
int __wrap_aarch64_device_cache(enum aarch64_cache_op op, uintptr_t address, size_t bytes)
{
    ++cache_calls; cache_lines += (bytes + 63) / 64;
    if (compute && compute->job_cpu && address<(uintptr_t)compute->job_cpu+PVR_COMPUTE_FAST_DEPTH*PVR_COMPUTE_FAST_STRIDE &&
        address+bytes>(uintptr_t)compute->job_cpu) ++payload_cache_calls;
    if (address>=FIXTURE_INPUT && address<FIXTURE_INPUT+(12u<<20)) ++dirty_input_cache_calls;
    if (fail_reopen_cache && service.epoch == 2) { fail_reopen_cache = 0; return 0; }
    if (address < FIXTURE_OUTPUT + FIXTURE_OUTPUT_BYTES && address + bytes > FIXTURE_OUTPUT) {
        configASSERT(op == AARCH64_CACHE_CLEAN_INVALIDATE && address >= FIXTURE_OUTPUT &&
                     address + bytes <= FIXTURE_OUTPUT + FIXTURE_OUTPUT_BYTES);
        if(compute && compute->phase==PVR_COMPUTE_QUEUE_IN_FLIGHT) {
            ++complete_calls;
            if(gpu_queue_case>=14 && gpu_queue_case<=17) {
                configASSERT(completion_clock==1 && completion_clock_samples==1);
                if(gpu_queue_case==14)completion_clock=2;
                if(gpu_queue_case==16)completion_clock=3;
                if(gpu_queue_case==17)completion_clock=4;
            }
        }
        ++output_cache_calls;
        if (__atomic_exchange_n(&fail_output_cache, 0, __ATOMIC_ACQ_REL)) return 0;
    }
    return __real_aarch64_device_cache(op, address, bytes);
}
enum pvr_cccb_result __real_pvr_cccb_poll(struct pvr_cccb *);
enum pvr_cccb_result __wrap_pvr_cccb_poll(struct pvr_cccb *c)
{ ++ring_polls;in_ring_poll=1;enum pvr_cccb_result r=__real_pvr_cccb_poll(c);in_ring_poll=0;return r; }
int __real_aarch64_device_translate(uintptr_t, int, struct aarch64_translation *);
int __wrap_aarch64_device_translate(uintptr_t a, int w, struct aarch64_translation *t)
{
    ++translations;
    /* The shader model writes output at EL1. Model the native client's
     * read-only output translation while keeping this backing writable for
     * the model. This is not CPU page-table permission qualification. */
    if(w && a>=FIXTURE_OUTPUT && a<FIXTURE_OUTPUT+FIXTURE_OUTPUT_BYTES)return 0;
    return __real_aarch64_device_translate(a,w,t);
}
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
    while (reply.result==PVR_GPU_SERVICE_OK && reply.phase!=PVR_GPU_SERVICE_READY) {
        const struct pvr_gpu_service_request status={PVR_GPU_SERVICE_STATUS,0};
        configASSERT(xTaskGetTickCount()-start<3000 && reply.phase!=PVR_GPU_SERVICE_FAILED);
        vTaskDelay(1);
        configASSERT(pvr_gpu_service_send_epoch(&service,reply.epoch,&status,10,&ticket));
        while (!pvr_gpu_service_collect(&service,ticket,&reply,10))
            configASSERT(xTaskGetTickCount()-start<3000);
    }
    if (reply.result==PVR_GPU_SERVICE_OK)
        configASSERT(get(&mmu_flushes)==reply.epoch && service.flush.phase==PVR_GPU_FLUSH_COMPLETE &&
            service.flush.executed_after!=service.flush.executed_before);
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

#ifdef GPU_QUEUE_RETURNING
#include "gpu-return-service.inc"
#endif
#ifdef PVR_GPU_PERFORMANCE
#include "gpu-performance-service.inc"
#endif
/* CPU maintenance does not evict GPU SLC lines. Shared mutable mappings
 * bypass SLC; immutable model and program mappings may remain cached. */
static void shared_mapping_flags(void)
{
    unsigned leases=0,job=0,programs=0,state=0;
    for(unsigned i=0;i<A733_GPU_QUEUE_MAPS && session.maps[i].bytes;++i){
        const struct pvr_gpu_mapping *m=&session.maps[i];
        if(m->address<(UINT64_C(1)<<36)){
            unsigned found=0;
            for(unsigned j=0;j<compute->lease_count;++j)if(m->address==compute->lease[j].address){
                unsigned access=compute->lease[j].access;
                uint32_t expected=(access&SS_ACCESS_GPU_WRITE?0:PVR_GPU_VM_READ_ONLY) |
                    (access&(SS_ACCESS_WRITE|SS_ACCESS_GPU_WRITE)?PVR_GPU_VM_SLC_BYPASS:0);
                configASSERT(m->flags==expected);++found;++leases;
            }
            configASSERT(found==1);
        }else if(m->address>=A733_GPU_QUEUE_JOB_GPU && m->address<A733_GPU_QUEUE_JOB_GPU+A733_GPU_QUEUE_JOB_PAGES*4096u){
            configASSERT(m->flags==(PVR_GPU_VM_READ_ONLY|PVR_GPU_VM_SLC_BYPASS));++job;
        }else if(m->address>=A733_GPU_QUEUE_PROGRAM_GPU && m->address<A733_GPU_QUEUE_PROGRAM_GPU+8192){
            configASSERT(m->flags==PVR_GPU_VM_READ_ONLY);++programs;
        }else{
            unsigned found=0;
            for(unsigned j=0;j<PVR_PROGRAM_REGIONS;++j){
                uint64_t first=session.programs.buffers[j].gpu;
                if(m->address>=first && m->address-first<compute->publish[j].page_count*4096u){
                    configASSERT(m->flags==(j<PVR_PROGRAM_SHARED_A?PVR_GPU_VM_READ_ONLY:PVR_GPU_VM_SLC_BYPASS));
                    ++found;if(j<PVR_PROGRAM_SHARED_A)++programs;else ++state;
                }
            }
            configASSERT(found==1);
        }
    }
    configASSERT(leases==compute->lease_count && job==A733_GPU_QUEUE_JOB_PAGES && programs && state);
    metric("shared-mapping-leases",leases);metric("shared-mapping-state-pages",state);
}
/* No accepted CREATE has run here. These calls execute the worker's actual
 * admission function with invalid source extents; no shader bytes are read. */
static void worker_program_bounds(struct gpu_queue_context_config *creation,
                                  struct gpu_queue_program *boundary)
{
    static struct a733_gpu_queue owner_before;
    static unsigned char payload_before[A733_GPU_QUEUE_PAYLOAD_PAGES*4096u];
    static struct device_memory_pool wb_before,nc_before;
    const uintptr_t begin=(uintptr_t)__start_sunstone_gpu_program,end=(uintptr_t)__stop_sunstone_gpu_program;
    configASSERT(end>=begin+4 && !(begin&3) && !(end&3));
    const struct {uintptr_t code;uint32_t bytes;} invalid[]={
        {UINT32_C(0x02000000),4},       /* Unreadable device register space. */
        {(uintptr_t)&program,4},       /* Other immutable image data. */
        {begin-4,4},{end,4},{end-4,8},{UINTPTR_MAX-3,8}
    };
    struct device_memory_pool *wb=a733_memory_reservations_init(),*nc=a733_memory_uncached_init();
    configASSERT(session.payload_extent.bytes==sizeof(payload_before) && !session.table_lease.generation);
    memcpy(&owner_before,&session,sizeof(session));
    memcpy(payload_before,(const void *)session.payload_extent.cpu,sizeof(payload_before));
    memcpy(&wb_before,wb,sizeof(wb_before));memcpy(&nc_before,nc,sizeof(nc_before));
    uint64_t cache_before=cache_calls,translations_before=translations;
    unsigned callbacks_before=calls;
    struct gpu_queue_program candidate=program;
    struct gpu_queue_context_config bad=*creation;bad.program=&candidate;
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i){
        candidate.code=(const uint32_t *)invalid[i].code;candidate.code_bytes=invalid[i].bytes;
        configASSERT(!pvr_compute_queue_create(compute,&bad));
        configASSERT(!memcmp(&owner_before,&session,sizeof(session)) &&
            !memcmp(payload_before,(const void *)session.payload_extent.cpu,sizeof(payload_before)) &&
            !memcmp(&wb_before,wb,sizeof(wb_before)) && !memcmp(&nc_before,nc,sizeof(nc_before)));
        configASSERT(cache_calls==cache_before && translations==translations_before && calls==callbacks_before &&
            service.phase==PVR_GPU_SERVICE_COLD && !service.epoch && !enabled && !running && !kick_step_count);
    }
    *boundary=program;boundary->code=(const uint32_t *)(end-4);boundary->code_bytes=4;
    creation->program=boundary;
    metric("invalid-program-source-extents",sizeof(invalid)/sizeof(invalid[0]));
}
static void client(void *unused)
{
    (void)unused;
    const unsigned c = gpu_queue_case;
    uint64_t lines0 = cache_lines, translations0 = translations;
#ifdef GPU_QUEUE_RETURNING
    if(c==72)failure_return_client(c);
#endif
    struct gpu_queue_context_config creation={.job_us=(c>=41&&c<=49)||c==52?200000:1000000,.program=&program,.lease_count=4};
    if (c==55) creation.priority=GPU_QUEUE_PRIORITY_HIGH;
    if (c==56) creation.job_us=2000;
    if (c==62) creation.payload_cache=GPU_QUEUE_PAYLOAD_UNCACHED;
    for(unsigned i=0;i<4;++i)creation.lease[i].region=i;
    struct gpu_queue_lease_info leases[GPU_QUEUE_LEASES];
    struct gpu_queue_program boundary;
    if(c==38)worker_program_bounds(&creation,&boundary);
    if(c==24)fail_reopen_post=1;
    int created=gpu_queue_context_create(&creation,gpu_queue_slow?60000:3000,&context,leases);
    if(c>=34 && c<=36){
        configASSERT(created==GPU_QUEUE_REFUSED && service.phase==PVR_GPU_SERVICE_COLD && !service.epoch &&
            !enabled && !running && !kick_step_count && !session.table_lease.generation && !session.requirements.table_pages);
        configASSERT(session.payload_lease.generation && session.firmware_lease.generation);
        platform_puts("PASS GPU queue case ");platform_hex(c);
        platform_puts("; rounded lease refuses an unleased manifest neighbour before GPU access\n");platform_exit(0);
    }
    if(c==24) {
        configASSERT(created==GPU_QUEUE_FAULT && !fail_reopen_post && service.phase==PVR_GPU_SERVICE_FAILED && atomic_load(&compute->lost));
        configASSERT(session.payload_lease.generation && session.table_lease.generation && session.firmware_lease.generation);
        platform_puts("PASS GPU queue case ");platform_hex(c);
        platform_puts("; failed creation record publication retains every resource\n");platform_exit(0);
    }
    if(created!=GPU_QUEUE_OK){metric("create-result",created);metric("prepare-stage",session.stage);
        metric("component-phase",compute->phase);metric("component-error",compute->error);
        metric("service-phase",service.phase);metric("service-error",service.error);
        metric("vm-phase",session.vm.phase);metric("table-pages",session.requirements.table_pages);}
    configASSERT(level()==0 && created==GPU_QUEUE_OK);
    configASSERT(leases[0].address==FIXTURE_OUTPUT && leases[1].address==FIXTURE_WEIGHTS &&
        leases[2].address==FIXTURE_WEIGHTS+(8u<<20) && leases[3].address==FIXTURE_INPUT);
    struct gpu_queue_record r;
    struct pvr_gpu_service_reply reply;
    configASSERT(compute->phase==PVR_COMPUTE_QUEUE_READY && service.phase==PVR_GPU_SERVICE_READY &&
        session.vm.phase==A733_GPU_VM_MEMORY_PUBLISHED);
#ifdef GPU_QUEUE_RETURNING
    if(c>=69)failure_return_client(c);
#endif
    if (c>=54) fast_cases(c);
    if (c==53) {
        while (service.irq_masked) vTaskDelay(1);
        vTaskSuspend(service.worker);
        service.config.job_irq_ack=job_irq_ack;
        service.config.job_now=job_now;
        pvr_compute_fast_ready(compute);
        /* A coalesced completion published before the waiter blocks must
         * leave a direct notification pending without running the worker. */
        atomic_store(&compute->fast.submitted,4);
        atomic_store(&compute->fast.waiter[0],3);
        put(compute->firmware[5].storage.cpu,4,4);
        set(&gpu_status,1);
        *(volatile uint32_t *)(uintptr_t)(GICD+0x200+4*(95/32))=1u<<(95%32);
        __asm__ volatile("dsb sy" ::: "memory");
        configASSERT(ulTaskNotifyTakeIndexed(PVR_COMPUTE_FAST_NOTIFY,pdTRUE,portMAX_DELAY)==1);
        configASSERT(atomic_load(&compute->fast.completed)==4 && !atomic_load(&compute->fast.waiter[0]) &&
            !service.irq_masked && !uxQueueMessagesWaiting(service.requests));
        /* Clear stale notifications before registration; a fence already
         * acquired by the ISR is then observed without blocking. */
        xTaskNotifyGiveIndexed(xTaskGetCurrentTaskHandle(),PVR_COMPUTE_FAST_NOTIFY);
        configASSERT(ulTaskNotifyTakeIndexed(PVR_COMPUTE_FAST_NOTIFY,pdTRUE,0)==1);
        atomic_store(&compute->fast.waiter[0],4);
        configASSERT(atomic_load_explicit(&compute->fast.completed,memory_order_acquire)>=4);
        atomic_store(&compute->fast.waiter[0],0);
        /* A timeline outside published work retains its actual inputs. */
        put(compute->firmware[5].storage.cpu,4,5);
        set(&gpu_status,1);
        *(volatile uint32_t *)(uintptr_t)(GICD+0x200+4*(95/32))=1u<<(95%32);
        __asm__ volatile("dsb sy" ::: "memory");
        while (!atomic_load(&compute->fast.irq_error)) taskYIELD();
        configASSERT(compute->fast.fault_observed==5 && compute->fast.fault_submitted==4 &&
            atomic_load(&compute->fast.completed)==4);
        platform_puts("PASS GPU queue case ");platform_hex(c);
        platform_puts("; direct ISR fences, coalesced completion, stale notification and fault origin\n");
        platform_exit(0);
    }
    configASSERT(get(&mmu_flushes)==1 && service.flush.phase==PVR_GPU_FLUSH_COMPLETE &&
        service.flush.executed_after!=service.flush.executed_before);
    shared_mapping_flags();
    metric("open-cache-lines", cache_lines - lines0);
    metric("open-translations", translations - translations0);
    unsigned wb, nc; pool_pages(&wb, &nc);
    metric("pool-wb-pages", wb); metric("pool-nc-pages", nc);
    metric("table-pages", session.requirements.table_pages);
    if(c>=39) lifecycle_cases(c);
    configASSERT(wb <= 87 && nc <= 52 && session.requirements.table_pages == 24);
#ifdef PVR_GPU_PERFORMANCE
    if(c>=25)performance_client();
#endif
#ifdef GPU_QUEUE_RETURNING
    if(c>=31)return_client();
#endif
    if(c==38){
        configASSERT(*(const uint32_t *)(session.payload_extent.cpu+4096)==*boundary.code && grid_job(0));
        platform_puts("PASS GPU queue case ");platform_hex(c);
        platform_puts("; invalid worker code extents preserve memory, final immutable word creates and runs\n");platform_exit(0);
    }
    if(c==37){
        configASSERT(grid_job(0));
        platform_puts("PASS GPU queue case ");platform_hex(c);
        platform_puts("; unleased neighbour at the rounded boundary remains disjoint\n");platform_exit(0);
    }
    if (c >= 21 && c <= 24) {
        configASSERT(grid_job(0));
        closed();
        const uint64_t old_epoch = service.epoch;
        const struct device_memory_lease payload = session.payload_lease, tables = session.table_lease;
        unsigned wb_before, nc_before; pool_pages(&wb_before, &nc_before);
        uint64_t cache_before = cache_lines;
        if (c == 22) {
            refuse_stopped = 1;
            configASSERT(reopen_reply(old_epoch).result == PVR_GPU_SERVICE_BUSY);
            refuse_stopped = 0;
            struct gpu_queue_record stale = {.sequence=1,.result=GPU_QUEUE_OK};
            configASSERT(xQueueSend(compute->completions, &stale, 0) == pdTRUE);
            configASSERT(reopen_reply(old_epoch).result == PVR_GPU_SERVICE_BUSY);
            configASSERT(xQueueReceive(compute->completions,&stale,0)==pdTRUE);
            ++session.payload_lease.generation;
            configASSERT(reopen_reply(old_epoch).result == PVR_GPU_SERVICE_BUSY);
            session.payload_lease = payload;
            configASSERT(cache_lines == cache_before && service.epoch == old_epoch &&
                         atomic_load(&compute->slot_state) == PVR_COMPUTE_QUEUE_CLOSED);
        }
        if (c == 23) fail_reopen_cache = 1;

        reply = reopen_reply(old_epoch);
        if (c == 23) {
            configASSERT(reply.result == (c == 23 ? PVR_GPU_SERVICE_FAULT : PVR_GPU_SERVICE_OK));
            configASSERT(!fail_reopen_cache &&
                         service.phase == PVR_GPU_SERVICE_FAILED && atomic_load(&compute->lost) &&
                         !receive_now(&r, 0));
            configASSERT(reopen_reply(service.epoch).result == PVR_GPU_SERVICE_FAULT);
        } else {
            configASSERT(reply.result == PVR_GPU_SERVICE_OK && reply.epoch == old_epoch + 1);
            configASSERT(compute->sequence == 1 &&
                         atomic_load(&compute->slot_state) == PVR_COMPUTE_QUEUE_IDLE);
            metric("first-reopen-cache-lines", cache_lines - cache_before);
            configASSERT(reopen_reply(old_epoch).result == PVR_GPU_SERVICE_INVALID);
            configASSERT(grid_job(1) && compute->sequence == 2);
            if (c == 21) {
                closed();
                configASSERT(reopen_reply(service.epoch).result == PVR_GPU_SERVICE_OK);
                configASSERT(compute->phase == PVR_COMPUTE_QUEUE_READY);
                configASSERT(grid_job(2) && compute->sequence == 3);
            }
        }
        unsigned wb_after, nc_after; pool_pages(&wb_after, &nc_after);
        configASSERT(wb_after == wb_before && nc_after == nc_before &&
                     !memcmp(&payload, &session.payload_lease, sizeof(payload)) &&
                     !memcmp(&tables, &session.table_lease, sizeof(tables)) &&
                     session.vm.phase == A733_GPU_VM_MEMORY_PUBLISHED);
        metric("stopped-proofs", stopped_proofs);
        metric("restart-cache-lines", cache_lines - cache_before);
        metric("service-epoch", service.epoch);
        metric("job-sequence", compute->sequence);
        platform_puts("PASS GPU queue case "); platform_hex(c);
        platform_puts("; retained healthy reopen with monotonic job sequence\n"); platform_exit(0);
    }
    if (c >= 18 && c <= 20) {
        const struct gpu_queue_job job = make_job(0);
        if (c == 18) {
            atomic_store(&hold_slot, 1);
            atomic_store(&hold_completion, 1);
            configASSERT(submit(&job) == 1);
            configASSERT(close_reply().result == PVR_GPU_SERVICE_BUSY);
            configASSERT(service.phase == PVR_GPU_SERVICE_READY && !stopping && !kick_step_count &&
                         atomic_load(&compute->slot_state) == PVR_COMPUTE_QUEUE_SUBMITTED);
            atomic_store(&hold_slot, 0);
            const struct pvr_gpu_service_message wake = {.client = PVR_GPU_SERVICE_CLIENTS,
                                                         .ticket = PVR_GPU_SERVICE_QUEUE_WAKE};
            configASSERT(xQueueSend(service.requests, &wake, 0) == pdTRUE);
            TickType_t wait_start = xTaskGetTickCount();
            while (compute->phase != PVR_COMPUTE_QUEUE_IN_FLIGHT) {
                configASSERT(xTaskGetTickCount() - wait_start < 3000);
                vTaskDelay(1);
            }
            configASSERT(close_reply().result == PVR_GPU_SERVICE_BUSY);
            configASSERT(service.phase == PVR_GPU_SERVICE_READY && !stopping && kick_step_count == 1 &&
                         compute->phase == PVR_COMPUTE_QUEUE_IN_FLIGHT &&
                         atomic_load(&compute->slot_state) == PVR_COMPUTE_QUEUE_TAKEN);
            atomic_store(&hold_completion, 0);
            r = receive();
            configASSERT(r.result == GPU_QUEUE_OK && r.sequence == 1 && complete_calls == 1);
            configASSERT(close_reply().result == PVR_GPU_SERVICE_OK);
            configASSERT(atomic_load(&compute->slot_state) == PVR_COMPUTE_QUEUE_CLOSED &&
                         submit(&job) == 0);
            TickType_t start = xTaskGetTickCount();
            while (service.phase != PVR_GPU_SERVICE_STOPPED) {
                configASSERT(service.phase != PVR_GPU_SERVICE_FAILED && xTaskGetTickCount() - start < 3000);
                vTaskDelay(1);
            }
            configASSERT(submit(&job) == 0 && !compute->owed &&
                         !receive_now(&r, 0) && kick_step_count == 1);
        } else if (c == 19) {
            /* State injection models a producer paused after reservation.
             * It does not resume an interrupted submit stack. */
            atomic_store(&compute->slot_state, PVR_COMPUTE_QUEUE_RESERVED);
            configASSERT(close_reply().result == PVR_GPU_SERVICE_BUSY);
            configASSERT(service.phase == PVR_GPU_SERVICE_READY && !stopping && !kick_step_count &&
                         atomic_load(&compute->slot_state) == PVR_COMPUTE_QUEUE_RESERVED);
            pvr_compute_queue_abort(compute);
            configASSERT(atomic_load(&compute->lost) && !compute->owed &&
                         atomic_load(&compute->slot_state) == PVR_COMPUTE_QUEUE_RESERVED &&
                         submit(&job) == -1 &&
                         !receive_now(&r, 0));
        } else {
            configASSERT(submit(&job) == 1);
            (void)retained_failure();
            configASSERT(service.phase==PVR_GPU_SERVICE_FAILED);
            configASSERT(close_reply().result == PVR_GPU_SERVICE_FAULT);
            configASSERT(atomic_load(&compute->slot_state) == PVR_COMPUTE_QUEUE_TAKEN &&
                         submit(&job) == -1 && !stopping &&
                         !receive_now(&r, 0));
        }
        unsigned wb_after, nc_after; pool_pages(&wb_after, &nc_after);
        configASSERT(wb_after == wb && nc_after == nc);
        platform_puts("PASS GPU queue case "); platform_hex(c);
        platform_puts("; checked CLOSE admission and retained slot ownership\n"); platform_exit(0);
    }
    if (c == 10) { fixture_mark(3); platform_puts("PASS GPU queue case "); platform_hex(c); platform_puts("; OPEN only\n"); platform_exit(0); }
    uint32_t waits0 = service.ready_timed_waits, expiries0 = service.ready_wait_expiries;
    uint64_t jobs_lines = cache_lines, jobs_translations = translations, jobs_cache = cache_calls;
    uint64_t output_cache0=output_cache_calls;
    if (c >= 13 && c <= 17) {
        if (c == 15) {
            configASSERT(grid_job(0));
            configASSERT(completion_clock_samples == 2 && complete_calls == 1 &&
                         compute->phase == PVR_COMPUTE_QUEUE_READY && !atomic_load(&compute->lost));
        } else {
            const struct gpu_queue_job job = make_job(0);
            configASSERT(submit(&job) == 1);
            (void)retained_failure();
            configASSERT(service.phase == PVR_GPU_SERVICE_FAILED &&
                         compute->phase == PVR_COMPUTE_QUEUE_FAILED && atomic_load(&compute->lost));
            metric("boundary-queue-error",compute->error);metric("boundary-service-error",service.error);
            metric("boundary-clock-samples",completion_clock_samples);metric("boundary-completes",complete_calls);
            configASSERT(compute->error == (c >= 16 ? GPU_QUEUE_SERVICE : GPU_QUEUE_HANG));
            configASSERT(completion_clock_samples == (c == 13 ? 1u : 2u) &&
                         complete_calls == (c == 13 ? 0u : 1u));
            configASSERT(atomic_load(&compute->slot_state) == PVR_COMPUTE_QUEUE_TAKEN &&
                         !receive_now(&r, 0) &&
                         submit(&job) == -1);
            unsigned wb_after, nc_after; pool_pages(&wb_after, &nc_after);
            configASSERT(wb_after == wb && nc_after == nc);
        }
        configASSERT(kick_step_count == 1 && submit_calls == 1);
        metric("completion-clock-samples", completion_clock_samples);
        metric("completion-cache-acquires", complete_calls);
        platform_puts("PASS GPU queue case "); platform_hex(c);
        platform_puts("; one job with checked completion clock boundary\n"); platform_exit(0);
    }
    if (c <= 4) {
        fixture_mark(1);
        for (unsigned j = 0; j < FIXTURE_JOBS; ++j) { fixture_mark(2); configASSERT(grid_job(j)); }
#ifdef SUNSTONE_LATENCY
        configASSERT(ss_latency_results[0].metric[SS_LAT_GPU].count > 1);
        configASSERT(ss_latency_results[0].metric[SS_LAT_GPU].maximum > 0);
        uint64_t latency_frequency;
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(latency_frequency));
        metric("latency-gpu-maximum-ticks", ss_latency_results[0].metric[SS_LAT_GPU].maximum);
        metric("latency-counter-hz", latency_frequency);
        platform_puts("PASS GPU IRQ timestamp reaches client receipt\n");
#endif
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
                     complete_calls == FIXTURE_JOBS && output_cache_calls-output_cache0 == FIXTURE_JOBS);
        platform_puts("PASS GPU queue case "); platform_hex(c);
        platform_puts("; grid jobs with every tag and sum\n"); platform_exit(0);
    }
    if (c == 5) {
        struct gpu_queue_job bad = make_job(0);
        bad.groups[0] = 0;
        configASSERT(gpu_queue_submit(context, &bad) == GPU_QUEUE_REFUSED);
        bad = make_job(0); bad.binding[1].lease = GPU_QUEUE_LEASES;
        configASSERT(gpu_queue_submit(context, &bad) == GPU_QUEUE_REFUSED);
        bad = make_job(0); bad.binding[2].sync_bytes = UINT32_MAX;
        configASSERT(gpu_queue_submit(context, &bad) == GPU_QUEUE_REFUSED);
        bad = make_job(0); bad.sequence = 0;
        configASSERT(gpu_queue_submit(context, &bad) == GPU_QUEUE_REFUSED);
        /* Worker-side deadline refusal leaves sequence and context usable. */
        bad = make_job(0); bad.not_after_us = 1;
        configASSERT(submit(&bad) == 1);
        r = receive();
        configASSERT(r.result == GPU_QUEUE_REFUSED && r.detail == GPU_QUEUE_DEADLINE && r.sequence == 1);
        configASSERT(atomic_load(&compute->slot_state) == PVR_COMPUTE_QUEUE_IDLE && !kick_step_count);
        configASSERT(grid_job(0) && grid_job(1));
        platform_puts("PASS GPU queue case "); platform_hex(c);
        platform_puts("; malformed bindings/grid/sequence and expired kick deadline refuse without a kick\n"); platform_exit(0);
    }
    if (c == 6) {
        /* A repeated wake is coalesced; reserved request capacity accepts the
         * one legitimate queue wake while all client tickets are pending. */
        vTaskSuspend(service.worker);
        const struct gpu_queue_job job = make_job(0);
        configASSERT(submit(&job) == 1);
        unsigned messages = uxQueueMessagesWaiting(service.requests);
        configASSERT(pvr_compute_queue_wake(compute) >= 0);
        configASSERT(uxQueueMessagesWaiting(service.requests) == messages);
        configASSERT(gpu_queue_submit(context, &job) == GPU_QUEUE_BUSY);
        vTaskResume(service.worker);
        r = receive();
        configASSERT(r.result == GPU_QUEUE_OK && r.sequence == 1 && kick_step_count == 1);
        completed = 1;
        configASSERT(grid_job(1));
        platform_puts("PASS GPU queue case "); platform_hex(c);
        platform_puts("; one coalesced wake accepts and completes one job\n"); platform_exit(0);
    }
    if (c == 7) {
        configASSERT(!grid_job(0));
        configASSERT(compute->phase == PVR_COMPUTE_QUEUE_READY && service.phase == PVR_GPU_SERVICE_READY);
        platform_puts("PASS GPU queue case "); platform_hex(c);
        platform_puts("; a missing tag reaches the client with the session idle\n"); platform_exit(0);
    }
    /* 8, 9, 11, 12: the job is FAULT; afterwards submit reports the loss. */
    unsigned lost_job = 0;
    if (c == 11) {
        /* A timeline the session did not expect, written while it is idle:
         * the next job is refused at its timeline check as FAULT, before any
         * ring write. */
        configASSERT(grid_job(0));
        __atomic_store_n((uint32_t *)compute->firmware[5].storage.cpu, completed + 5, __ATOMIC_RELEASE);
        lost_job = 1;
    }
    if (c == 12) __atomic_store_n(&fail_output_cache, 1, __ATOMIC_RELEASE);
    const struct gpu_queue_job job = make_job(lost_job);
    uint32_t kicks_before = kick_step_count;
    configASSERT(submit(&job) == 1);
    uint64_t at=retained_failure();
    configASSERT(service.phase==PVR_GPU_SERVICE_FAILED);
    configASSERT(compute->error == (c == 8 ? GPU_QUEUE_HANG : c == 9 ? GPU_QUEUE_RESET :
                                  c == 11 ? GPU_QUEUE_TIMELINE : GPU_QUEUE_MAINTENANCE));
    if (c == 11) configASSERT(kick_step_count == kicks_before && submit_calls == 1);
    if (c == 12) configASSERT(complete_calls == 1 && !__atomic_load_n(&fail_output_cache, __ATOMIC_ACQUIRE));
    metric("lost-after-kick-us", at - kick_us);
    if (c==8) configASSERT(at>=compute->client.deadline && at<=compute->deadline+3000);
    configASSERT(submit(&job) == -1);
    platform_puts("PASS GPU queue case "); platform_hex(c);
    platform_puts(c == 8 ? "; a hang retains DMA ownership after max_job_us and submit reports the loss\n" :
                  c == 9 ? "; a context reset retains DMA ownership and submit reports the loss\n" :
                  c == 11 ? "; an unexpected timeline retains DMA ownership and submit reports the loss\n" :
                  "; a failed completion clean-and-invalidate ends in FAULT and submit then reports the loss\n");
    platform_exit(0);
}
/* Unused parts of the included model. */
void fixture_unused(void)
{ (void)firmware_fixture_owner; (void)firmware_fixture_event; (void)firmware_fixture_irq_read;
  (void)firmware_fixture_irq_write; (void)peer; (void)supervisor; }

#include "gpu-fast.inc"

void kernel_main(void)
{
    platform_init(); first_publish = 0;
#ifdef SUNSTONE_LATENCY
    uint64_t frequency;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    ss_latency_init(frequency);
#endif
    configASSERT(gpu_queue_case >= 1 && gpu_queue_case <= 75);
    /* Synthetic weights over both 8 MiB model regions. */
    uint64_t *weights = (uint64_t *)(uintptr_t)0x44000000u;
    if (gpu_queue_case == 1) for (size_t i = 0; i < (16u << 20) / 8; ++i) weights[i] = next_random();
    memset((void *)(uintptr_t)FIXTURE_OUTPUT, 0, FIXTURE_TAG_BYTES);
    /* SPI 95: group 1, an API-safe priority, routed to core 0, disabled. */
    volatile uint32_t *d = (volatile uint32_t *)(uintptr_t)GICD;
    d[(0x80 + 4 * (95 / 32)) / 4] |= 1u << (95 % 32);
    *(volatile uint8_t *)(uintptr_t)(GICD + 0x400 + 95) = portLOWEST_USABLE_INTERRUPT_PRIORITY << portPRIORITY_SHIFT;
    *(volatile uint64_t *)(uintptr_t)(GICD + 0x6000 + 8 * 95) = 0;
    d[(0x180 + 4 * (95 / 32)) / 4] = 1u << (95 % 32);
    __asm__ volatile("dsb sy" ::: "memory");
    for(unsigned i=0;i<FIXTURE_GPU_TENSORS;++i)tensors[fixture_tensors[i].ordinal]=fixture_tensors[i];
    struct pvr_gpu_service_config *rc = a733_gpu_reserved_prepare(&firmware, 9, gpu_firmware,
        (size_t)(gpu_firmware_end - gpu_firmware), 400000000);
    configASSERT(rc);
    unsigned region_count=4;
    if(gpu_queue_case>=34 && gpu_queue_case<=37){
        unsigned target=gpu_queue_case==35?1:gpu_queue_case==36?3:0;
        regions[target].bytes=target==1?17:4096;
        regions[4]=(struct ss_region){.address=regions[target].address+(gpu_queue_case==37?FIXTURE_OUTPUT_BYTES:4096),
            .bytes=4096,.kind=SS_REGION_WORK,.access=SS_ACCESS_READ|SS_ACCESS_WRITE};
        region_count=5;
    }
    compute = a733_gpu_queue_prepare(&session, &firmware, &service, 10, regions, region_count);
    configASSERT(compute && compute->phase == PVR_COMPUTE_QUEUE_BOUND && firmware.config.compute_queue == compute);
    configASSERT(pvr_compute_queue_client_bind(compute));
    config = firmware.config;
    config.queue = (struct pvr_ccb_ops){read_mem, write_mem, barrier, kick};
    config.start = (struct pvr_gpu_start_ops){boundary_now, read_reg, write_reg, started, publish};
    config.control = (struct pvr_gpu_control_ops){boundary_now, irq_read, irq_write, compat, event};
    config.executed = executed; config.flush_mmu = 1;
    config.stop = (struct pvr_gpu_stop_ops){boundary_now, read_reg, write_reg, sync_write, sync_read, isolate, NULL};
    config.enable = enable; config.context = &config; config.compute_mode = mode;
    config.irq = (struct pvr_gpu_service_irq_ops){mask, rearm};
    if (gpu_queue_case>=54 && (gpu_queue_case<=68||gpu_queue_case==73||gpu_queue_case==75)) {config.job_irq_ack=job_irq_ack;config.job_now=job_now;}
    if (gpu_queue_case>=63 && (gpu_queue_case<=68||gpu_queue_case==73||gpu_queue_case==75)) config.job_kick=job_kick;
    if(gpu_queue_case>=41) {
        config.recovery_begin=recovery_admit;
        config.stop.read=recovery_read;config.stop.isolate=recovery_isolate;
        config.enable=recovery_enable;
    }
    configASSERT(a733_gpu_memory_init(&memory, config.firmware_buffers, config.resources));
    /* The component was bound to the reserved configuration; the service
     * copies this one, which carries the same pointer. */
#ifdef PVR_GPU_PERFORMANCE
    config.performance_clock=(struct pvr_gpu_clock_ops){boundary_now,sync_write,sync_read,perf_state,perf_publish};
    config.performance_begin=perf_begin;config.performance_poll=perf_poll;config.performance_rate=perf_current;config.performance_client=1;
#endif
#ifdef GPU_QUEUE_RETURNING
    config.park=final_park;
#endif
    configASSERT(pvr_gpu_service_init(&service, &config, 4, 1));
    TaskHandle_t a = xTaskCreateStaticAffinitySet(client, "client", 4096, NULL, 3, queue_stacks[0], &queue_tasks[0], 1);
    TaskHandle_t m = xTaskCreateStaticAffinitySet(model, "model", 4096, NULL, 6, queue_stacks[1], &queue_tasks[1], 2);
    configASSERT(a && m && pvr_gpu_service_bind(&service, 0, a));
    if(gpu_queue_case>=40 && gpu_queue_case<68) {
        TaskHandle_t peer=xTaskCreateStaticAffinitySet(shared_peer,"peer",4096,NULL,3,queue_stacks[2],&queue_tasks[2],1);
        configASSERT(peer && pvr_compute_queue_client_register(compute,peer));
    }
#ifdef PVR_GPU_PERFORMANCE
    TaskHandle_t thermal=xTaskCreateStaticAffinitySet(performance_owner,"thermal",4096,NULL,5,perf_stack,&perf_task,1);
    configASSERT(thermal && pvr_gpu_service_bind(&service,1,thermal));
#endif
    vTaskStartScheduler(); platform_panic("scheduler returned", 0);
}
