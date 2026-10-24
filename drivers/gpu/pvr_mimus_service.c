/* The mimus session as a component of the GPU service
 * (docs/gpu-mimus-service.md). The GPU service worker calls every function
 * here except pvr_mimus_service_submit and pvr_mimus_service_receive, which
 * the client task calls. Configuration is checked once at bind; the per-job
 * path makes no translation, hash or allocation and waits for no tick. */
#include "pvr_mimus_service.h"
#include "pvr_gpu_service.h"
#include "pvr_gpu_events.h"
#include "a733_range.h"
#include "a733_mmio.h"
#include "a733_gpu_mapping.h"
#include "aarch64_device_memory.h"
#include <string.h>

#define PAGE 4096u
enum { CONTEXT, STATE, CONTROL, RING, MEMORY, TIMELINE };
/* The firmware's 16 MiB MIPS heap; each object needs its own address there. */
#define FIRMWARE_HEAP UINT32_C(0xc0000000)
#define FIRMWARE_HEAP_END UINT32_C(0xc1000000)

static void put32(unsigned char *p, uint32_t v)
{ for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (8 * i)); }

/* Client-ring operations. Bind proved the control and ring pages uncached and
 * mapped, so no access translates. A command write copies without a barrier:
 * the ring's release barrier precedes its publication. Control reads complete
 * before the next access. */
static int ring_now(void *ctx, uint64_t *now)
{ return pvr_gpu_service_now(((struct pvr_mimus_service *)ctx)->service, now); }
static int ring_read(void *ctx, enum pvr_cccb_region r, uint32_t off, void *out, size_t n)
{
    struct pvr_mimus_service *m = ctx;
    if (r != PVR_CCCB_CONTROL || n != 4 || (off & 3) || off >= PVR_CCCB_CONTROL_BYTES) return 0;
    uint32_t v = a733_mmio_read32_dsb_sy_after((uintptr_t)m->config->firmware[CONTROL].storage.cpu + off);
    memcpy(out, &v, 4);
    return 1;
}
static int ring_write(void *ctx, enum pvr_cccb_region r, uint32_t off, const void *in, size_t n)
{
    struct pvr_mimus_service *m = ctx;
    unsigned id = r == PVR_CCCB_CONTROL ? CONTROL : RING;
    size_t limit = r == PVR_CCCB_CONTROL ? PVR_CCCB_CONTROL_BYTES : PVR_MIMUS_SERVICE_RING_BYTES;
    if ((unsigned)r > PVR_CCCB_COMMANDS || !n || off > limit || n > limit - off ||
        (r == PVR_CCCB_CONTROL && m->phase != PVR_MIMUS_SERVICE_FORMAT)) return 0;
    volatile unsigned char *d = (volatile unsigned char *)m->config->firmware[id].storage.cpu + off;
    const unsigned char *s = in;
    for (size_t i = 0; i < n; ++i) d[i] = s[i];
    return 1;
}
static int ring_barrier(void *ctx, enum pvr_ccb_barrier kind)
{
    (void)ctx;
    if ((unsigned)kind > PVR_CCB_DEVICE_COMPLETE) return 0;
    a733_mmio_dsb_sy();
    return 1;
}
/* The timeline word is uncached, so a plain load observes the firmware's
 * store; the completion's DC CIVAC begins with DSB SY, which orders the tag
 * and sum reads after this load. */
static uint32_t timeline(const struct pvr_mimus_service *m)
{ return *(const volatile uint32_t *)m->config->firmware[TIMELINE].storage.cpu; }

static void post(struct pvr_mimus_service *m, uint32_t sequence, uint32_t result)
{
    const struct pvr_mimus_record record = {sequence, result};
    m->owed = 0;
    /* One record is owed at a time, so the one-entry queue is empty here. */
    (void)xQueueSend(m->completions, &record, 0);
}

static int object_valid(const struct pvr_mips_buffer *b, uint32_t address)
{
    return b->page_count == 1 && b->storage.bytes == PAGE && !((uintptr_t)b->storage.cpu & (PAGE - 1)) &&
        a733_span_excl_nonempty(b->pages, sizeof(*b->pages)) && !(address & (PAGE - 1)) &&
        address >= FIRMWARE_HEAP && address < FIRMWARE_HEAP_END &&
        a733_gpu_page_mapped((uintptr_t)b->storage.cpu, b->pages[0], 0x44, 1);
}
static int pages_valid(const struct pvr_mips_buffer *b)
{
    if (!b->page_count || b->page_count > 8 || b->storage.bytes != (size_t)b->page_count * PAGE ||
        ((uintptr_t)b->storage.cpu & (PAGE - 1)) ||
        !a733_span_excl_nonempty(b->pages, b->page_count * sizeof(*b->pages))) return 0;
    for (unsigned i = 0; i < b->page_count; ++i)
        if (!a733_gpu_page_mapped((uintptr_t)b->storage.cpu + i * PAGE, b->pages[i], 0xff, 1)) return 0;
    return 1;
}
/* Disjoint from the service, its configuration and its firmware memory. */
static int outside_service(const void *p, size_t n, const struct pvr_gpu_service *g,
                           const struct pvr_gpu_service_config *gc)
{
    if (!a733_ranges_disjoint_nonempty(p, n, g, sizeof(*g)) ||
        !a733_ranges_disjoint_nonempty(p, n, gc, sizeof(*gc)) ||
        !a733_ranges_disjoint_nonempty(p, n, gc->firmware, gc->firmware_bytes)) return 0;
    for (unsigned i = 0; i < 3 + PVR_FW_RESOURCE_COUNT; ++i) {
        const struct pvr_mips_buffer *b = i < 3 ? &gc->firmware_buffers[i] : &gc->resources[i - 3];
        if (!a733_ranges_disjoint_nonempty(p, n, b->storage.cpu, b->storage.bytes)) return 0;
    }
    return 1;
}
static int outside(const void *p, size_t n, const struct pvr_mimus_service *m,
                   const struct pvr_gpu_service *g, const struct pvr_gpu_service_config *gc)
{ return a733_ranges_disjoint_nonempty(p, n, m, sizeof(*m)) && outside_service(p, n, g, gc); }

int pvr_mimus_service_bind(struct pvr_mimus_service *m, struct pvr_gpu_service *g,
                           struct pvr_gpu_service_config *gc, const struct pvr_mimus_service_config *c)
{
    if (!a733_span_excl_nonempty(m, sizeof(*m)) || !a733_span_excl_nonempty(c, sizeof(*c)) ||
        !a733_span_excl_nonempty(g, sizeof(*g)) || !a733_span_excl_nonempty(gc, sizeof(*gc)) ||
        m->phase != PVR_MIMUS_SERVICE_NEW || g->initialized || gc->mimus || gc->compute ||
        !a733_ranges_disjoint_nonempty(m, sizeof(*m), c, sizeof(*c)) ||
        xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED ||
        c->session.last_sequence || !c->resume || !c->max_job_us || c->max_job_us > PVR_MIMUS_SERVICE_MAX_JOB_US ||
        !c->tensor_count || c->tensor_count > 0xffffu ||
        !a733_span_excl_nonempty(c->tensors, c->tensor_count * sizeof(*c->tensors)) ||
        !a733_span_excl_nonempty(c->vm, sizeof(*c->vm)) || c->vm->phase != A733_GPU_VM_MEMORY_PREPARED ||
        c->vm->context_cpu != c->firmware[MEMORY].storage.cpu ||
        c->vm->context_physical != c->firmware[MEMORY].pages[0] ||
        !outside_service(m, sizeof(*m), g, gc) || !outside(c->tensors, c->tensor_count * sizeof(*c->tensors), m, g, gc))
        return 0;
    for (unsigned i = 0; i < PVR_MIMUS_SERVICE_OBJECTS; ++i) {
        const struct pvr_mips_buffer *b = &c->firmware[i];
        if (!object_valid(b, c->firmware_address[i]) || !outside(b->storage.cpu, PAGE, m, g, gc)) return 0;
        for (unsigned j = 0; j < i; ++j)
            if (c->firmware_address[j] == c->firmware_address[i] || c->firmware[j].pages[0] == b->pages[0] ||
                !a733_ranges_disjoint_nonempty(c->firmware[j].storage.cpu, PAGE, b->storage.cpu, PAGE)) return 0;
    }
    for (unsigned i = 0; i < PVR_MIMUS_SERVICE_PUBLISH; ++i)
        if (!pages_valid(&c->publish[i]) || !outside(c->publish[i].storage.cpu, c->publish[i].storage.bytes, m, g, gc))
            return 0;
    struct pvr_mimus_service value = {.config = c, .service = g, .phase = PVR_MIMUS_SERVICE_BOUND};
    for (unsigned i = 0; i < PVR_MIMUS_SERVICE_OBJECTS; ++i)
        value.objects[i] = (struct pvr_mips_object){c->firmware[i].pages, 1, c->firmware_address[i], PVR_MIPS_UNCACHED};
    *m = value;
    m->completions = xQueueCreateStatic(1, sizeof(struct pvr_mimus_record), m->completion_storage,
                                        &m->completion_state);
    if (!m->completions) { m->phase = PVR_MIMUS_SERVICE_FAILED; return 0; }
    gc->mimus = m;
    return 1;
}

int pvr_mimus_service_prepare(struct pvr_mimus_service *m, uint64_t deadline)
{
    if (!m || m->phase != PVR_MIMUS_SERVICE_BOUND || !m->service->epoch) return 0;
    const struct pvr_mimus_service_config *c = m->config;
    /* The record of OPEN is owed from here on, whatever happens next. */
    m->owed = 1;
    m->epoch = m->service->epoch;
    m->deadline = deadline;
    if (!pvr_mimus_session_open(&m->session, &c->session)) return 0;
    memset(m->command, 0, sizeof(m->command));
    /* The program samples nothing, so the command names no border table. */
    pvr_mimus_session_command(&m->session, m->command, 0, c->resume);
    put32(m->update, c->firmware_address[TIMELINE]);
    put32(m->update + 4, 0);
    /* Timeline 0: the first job's sequence is 1. */
    *(volatile uint32_t *)c->firmware[TIMELINE].storage.cpu = 0;
    a733_mmio_dsb_sy();
    m->sequence = 0;
    m->publish_index = m->publish_page = 0;
    m->phase = PVR_MIMUS_SERVICE_PUBLISHING;
    return 1;
}

int pvr_mimus_service_publish_poll(struct pvr_mimus_service *m)
{
    if (!m || m->epoch != m->service->epoch) return -1;
    const struct pvr_mimus_service_config *c = m->config;
    if (m->phase == PVR_MIMUS_SERVICE_PUBLISHING) {
        const struct pvr_mips_buffer *b = &c->publish[m->publish_index];
        if (!aarch64_device_cache(AARCH64_CACHE_CLEAN, (uintptr_t)b->storage.cpu + m->publish_page * PAGE, PAGE)) {
            m->error = PVR_MIMUS_SERVICE_PUBLICATION; return -1;
        }
        if (++m->publish_page == b->page_count) { m->publish_page = 0; ++m->publish_index; }
        if (m->publish_index == PVR_MIMUS_SERVICE_PUBLISH) {
            if (a733_gpu_vm_memory_publish_begin(c->vm) != A733_GPU_VM_MEMORY_AGAIN) {
                m->error = PVR_MIMUS_SERVICE_PUBLICATION; return -1;
            }
            m->phase = PVR_MIMUS_SERVICE_TABLES;
        }
        return 0;
    }
    if (m->phase != PVR_MIMUS_SERVICE_TABLES) return -1;
    enum a733_gpu_vm_memory_result r = a733_gpu_vm_memory_poll(c->vm);
    if (r == A733_GPU_VM_MEMORY_AGAIN) return 0;
    if (r != A733_GPU_VM_MEMORY_OK) { m->error = PVR_MIMUS_SERVICE_PUBLICATION; return -1; }
    m->phase = PVR_MIMUS_SERVICE_PUBLISHED;
    return 1;
}

static int fault(struct pvr_mimus_service *m, unsigned error)
{ m->error = error; return -1; }
/* Steps the client ring until its kick is accepted or it must wait for ring
 * space or a kernel-ring slot. */
static int drive_to_kick(struct pvr_mimus_service *m)
{
    while (m->client.phase >= PVR_CCCB_SPACE && m->client.phase <= PVR_CCCB_SUBMIT) {
        uint32_t before = m->client.phase, cursor = m->client.cursor, index = m->client.index;
        enum pvr_cccb_result r = pvr_cccb_poll(&m->client);
        if (r == PVR_CCCB_FAULT || r == PVR_CCCB_INVALID) return fault(m, PVR_MIMUS_SERVICE_RING);
        if (m->client.phase == before && m->client.cursor == cursor && m->client.index == index) break;
    }
    if (m->client.phase == PVR_CCCB_WAIT) {
        uint64_t now;
        /* The hang bound runs from the accepted kick. */
        if (pvr_gpu_service_now(m->service, &now) != 1 ||
            !a733_deadline_after(now, m->config->max_job_us, &m->deadline)) return fault(m, PVR_MIMUS_SERVICE_CLOCK);
    }
    return 1;
}
static int take(struct pvr_mimus_service *m)
{
    unsigned submitted = PVR_MIMUS_SLOT_SUBMITTED;
    if (!atomic_compare_exchange_strong_explicit(&m->slot_state, &submitted, PVR_MIMUS_SLOT_TAKEN,
            memory_order_acquire, memory_order_relaxed)) return 0;
    /* One copy; every later check and use reads this copy only. */
    m->job = m->slot;
    m->owed = 1;
    const struct pvr_mimus_service_config *c = m->config;
    uint32_t sequence = m->sequence + 1;
    /* At most 13 compares: the ordinal bound and the session's admission. */
    const struct pvr_mimus_tensor *t = m->job.ordinal < c->tensor_count ? &c->tensors[m->job.ordinal] : NULL;
    if (!t || !t->job_rows || sequence == UINT32_MAX || m->client.phase != PVR_CCCB_IDLE) {
        atomic_store_explicit(&m->slot_state, PVR_MIMUS_SLOT_IDLE, memory_order_release);
        post(m, sequence, PVR_MIMUS_REFUSED);
        return 1;
    }
    /* The previous job's update is the only timeline write since OPEN. */
    if (timeline(m) != m->sequence) return fault(m, PVR_MIMUS_SERVICE_TIMELINE);
    const struct pvr_mimus_job job = {m->job.x_offset, m->job.rows, m->job.x_stride, sequence};
    if (!pvr_mimus_submit(&m->session, t, &job)) {
        atomic_store_explicit(&m->slot_state, PVR_MIMUS_SLOT_IDLE, memory_order_release);
        post(m, sequence, PVR_MIMUS_REFUSED);
        return 1;
    }
    put32(m->update + 4, sequence);
    const struct pvr_cccb_command commands[2] = {
        {PVR_CCB_MAGIC | PVR_CCCB_TASK | 205u, 96, 1, 1, m->command},
        {PVR_CCB_MAGIC | 213u, 8, 0, 0, m->update}};
    uint64_t now, deadline;
    if (pvr_gpu_service_now(m->service, &now) != 1 || !a733_deadline_after(now, c->max_job_us, &deadline))
        return fault(m, PVR_MIMUS_SERVICE_CLOCK);
    /* From here the job counts as on the GPU: any failure is LOST. */
    m->sequence = sequence;
    m->deadline = deadline;
    m->phase = PVR_MIMUS_SERVICE_IN_FLIGHT;
    if (!pvr_cccb_begin(&m->client, commands, 2, NULL, 0, deadline)) return fault(m, PVR_MIMUS_SERVICE_RING);
    return drive_to_kick(m);
}
static int complete(struct pvr_mimus_service *m)
{
    if (m->client.phase != PVR_CCCB_WAIT && m->client.phase != PVR_CCCB_IDLE) {
        if (drive_to_kick(m) < 0) return -1;
        return 0;
    }
    if (m->client.phase == PVR_CCCB_WAIT) {
        enum pvr_cccb_result r = pvr_cccb_poll(&m->client);
        if (r == PVR_CCCB_FAULT || r == PVR_CCCB_INVALID) return fault(m, PVR_MIMUS_SERVICE_RING);
    }
    uint64_t now;
    if (pvr_gpu_service_now(m->service, &now) != 1) return fault(m, PVR_MIMUS_SERVICE_CLOCK);
    if (now >= m->deadline) return fault(m, PVR_MIMUS_SERVICE_DEADLINE);
    if (m->client.phase == PVR_CCCB_IDLE) {
        uint32_t value = timeline(m);
        if (value == m->sequence) {
            if (!pvr_mimus_complete(&m->session)) return fault(m, PVR_MIMUS_SERVICE_MAINTENANCE);
            /* Acquiring the output can consume the remaining job budget.
             * Keep the slot owned until that work is timely and visible. */
            if (pvr_gpu_service_now(m->service, &now) != 1) return fault(m, PVR_MIMUS_SERVICE_CLOCK);
            if (now >= m->deadline) return fault(m, PVR_MIMUS_SERVICE_DEADLINE);
            m->phase = PVR_MIMUS_SERVICE_IDLE;
            atomic_store_explicit(&m->slot_state, PVR_MIMUS_SLOT_IDLE, memory_order_release);
            post(m, m->sequence, PVR_MIMUS_OK);
            return 1;
        }
        if (value != m->sequence - 1) return fault(m, PVR_MIMUS_SERVICE_TIMELINE);
    }
    return 0;
}

int pvr_mimus_service_poll(struct pvr_mimus_service *m)
{
    if (!m || m->phase == PVR_MIMUS_SERVICE_FAILED || m->epoch != m->service->epoch) return -1;
    switch (m->phase) {
    case PVR_MIMUS_SERVICE_PUBLISHED: {
        static const struct pvr_cccb_ops ops = {ring_now, ring_read, ring_write, ring_barrier};
        if (!pvr_cccb_init(&m->client, &ops, m, &m->service->queue, PVR_MIMUS_SERVICE_RING_BYTES,
                           m->config->firmware_address[CONTEXT], m->deadline)) return fault(m, PVR_MIMUS_SERVICE_RING);
        m->phase = PVR_MIMUS_SERVICE_FORMAT;
    }
    /* fall through */
    case PVR_MIMUS_SERVICE_FORMAT: {
        enum pvr_cccb_result r = pvr_cccb_poll(&m->client);
        if (r == PVR_CCCB_FAULT || r == PVR_CCCB_INVALID) return fault(m, PVR_MIMUS_SERVICE_RING);
        if (m->client.phase != PVR_CCCB_IDLE) return 0;
        m->phase = PVR_MIMUS_SERVICE_IDLE;
        post(m, 0, PVR_MIMUS_OK);
        return 1;
    }
    case PVR_MIMUS_SERVICE_IDLE: return take(m);
    case PVR_MIMUS_SERVICE_IN_FLIGHT: return complete(m);
    default: return fault(m, PVR_MIMUS_SERVICE_LIFECYCLE);
    }
}

int pvr_mimus_service_event(struct pvr_mimus_service *m, const unsigned char command[88])
{
    if (!m || m->phase == PVR_MIMUS_SERVICE_FAILED) return 0;
    struct pvr_gpu_event event;
    if (pvr_gpu_event_decode(command, 88, &event) != PVR_GPU_EVENT_DECODE_OK || event.kind == PVR_GPU_EVENT_RESET) {
        m->error = PVR_MIMUS_SERVICE_RESET;
        return 0;
    }
    return 1;
}

void pvr_mimus_service_abort(struct pvr_mimus_service *m)
{
    if (!m || m->phase == PVR_MIMUS_SERVICE_FAILED || m->phase == PVR_MIMUS_SERVICE_NEW) return;
    m->phase = PVR_MIMUS_SERVICE_FAILED;
    if (m->client.phase != PVR_CCCB_NEW) (void)pvr_cccb_abort(&m->client);
    pvr_mimus_stop(&m->session);
    /* The flag precedes the slot check; the client stores the slot before it
     * reads the flag. One of the two sees the other. */
    atomic_store(&m->lost, 1);
    unsigned submitted = PVR_MIMUS_SLOT_SUBMITTED;
    if (atomic_compare_exchange_strong(&m->slot_state, &submitted, PVR_MIMUS_SLOT_TAKEN)) m->owed = 1;
    if (m->owed) post(m, m->sequence, PVR_MIMUS_LOST);
}

int pvr_mimus_service_deadline(const struct pvr_mimus_service *m, uint64_t *deadline)
{
    if (!m || m->phase != PVR_MIMUS_SERVICE_IN_FLIGHT) return 0;
    *deadline = m->deadline;
    return 1;
}
int pvr_mimus_service_close(struct pvr_mimus_service *m)
{
    if (!m || m->phase != PVR_MIMUS_SERVICE_IDLE || m->owed || atomic_load(&m->lost)) return 0;
    unsigned idle = PVR_MIMUS_SLOT_IDLE;
    return atomic_compare_exchange_strong(&m->slot_state, &idle, PVR_MIMUS_SLOT_CLOSED);
}
int pvr_mimus_service_submitting(const struct pvr_mimus_service *m)
{
    return m && (m->phase == PVR_MIMUS_SERVICE_PUBLISHED || m->phase == PVR_MIMUS_SERVICE_FORMAT ||
        (m->phase == PVR_MIMUS_SERVICE_IDLE &&
         atomic_load_explicit(&m->slot_state, memory_order_relaxed) == PVR_MIMUS_SLOT_SUBMITTED));
}
uint64_t pvr_mimus_service_progress(const struct pvr_mimus_service *m)
{
    return m ? (uint64_t)m->phase | (uint64_t)m->client.phase << 8 | (uint64_t)m->client.cursor << 16 |
        (uint64_t)m->sequence << 32 : 0;
}

int pvr_mimus_service_submit(struct pvr_mimus_service *m, const struct pvr_mimus_request *job)
{
    if (atomic_load(&m->lost)) return -1;
    /* Reserve before copying: CLOSE can seal only an unowned slot. The
     * sequentially consistent publication and lost handshake also covers
     * an abort while this producer holds RESERVED. */
    unsigned idle = PVR_MIMUS_SLOT_IDLE;
    if (!atomic_compare_exchange_strong(&m->slot_state, &idle, PVR_MIMUS_SLOT_RESERVED)) return 0;
    m->slot = *job;
    atomic_store(&m->slot_state, PVR_MIMUS_SLOT_SUBMITTED);
    const struct pvr_gpu_service_message wake = {.client = PVR_GPU_SERVICE_CLIENTS,
                                                 .ticket = PVR_GPU_SERVICE_MIMUS_WAKE};
    int sent = xQueueSend(m->service->requests, &wake, 0) == pdTRUE;
    if (!sent || atomic_load(&m->lost)) {
        unsigned submitted = PVR_MIMUS_SLOT_SUBMITTED;
        /* Taken back: nothing reached the worker. Otherwise it owes a record. */
        if (atomic_compare_exchange_strong(&m->slot_state, &submitted, PVR_MIMUS_SLOT_IDLE))
            return atomic_load(&m->lost) ? -1 : 0;
    }
    return 1;
}
int pvr_mimus_service_receive(struct pvr_mimus_service *m, struct pvr_mimus_record *record, TickType_t wait)
{ return xQueueReceive(m->completions, record, wait) == pdTRUE; }
