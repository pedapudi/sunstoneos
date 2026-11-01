/* One NPU job from an application task on the NPU runtime (npu_job.h). */
#include "npu_job.h"
#include "npu_crumb.h"
#include <string.h>

static struct {
    struct npu_runtime *rt;
    TickType_t wait;
    /* Written only by each slot's client task. */
    unsigned failed[NPU_RUNTIME_SLOTS];
} job;

int npu_job_attach(struct npu_runtime *rt, TickType_t wait)
{
    if (job.rt || !rt || !rt->bound || !wait || wait == portMAX_DELAY ||
        xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) return 0;
    job.rt = rt;
    job.wait = wait;
    return 1;
}
/* The slot whose client is the calling task, or NPU_RUNTIME_SLOTS. */
static unsigned own_slot(void)
{
    if (!job.rt || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) return NPU_RUNTIME_SLOTS;
    const TaskHandle_t self = xTaskGetCurrentTaskHandle();
    for (unsigned i = 0; i < job.rt->slots; ++i)
        if (job.rt->slot[i].client == self) return i;
    return NPU_RUNTIME_SLOTS;
}
static int failed(unsigned slot, uint32_t reason)
{
    job.failed[slot] = 1;
    sunstone_crumb(NPU_JOB_CRUMB_FAILED, reason);
    return -1;
}
static int refused(uint32_t reason)
{
    sunstone_crumb(NPU_JOB_CRUMB_REFUSED, reason);
    return 0;
}
/* One request and its reply. 0 means the reply did not arrive or does not
 * answer this request; the ticket then stays outstanding. */
static int call(unsigned slot, enum npu_service_operation op, unsigned model, uint64_t timeout,
                struct npu_service_reply *reply)
{
    uint64_t ticket;
    return npu_runtime_send(job.rt, slot, op, model, timeout, job.wait, &ticket) &&
        npu_runtime_collect(job.rt, slot, ticket, reply, job.wait) && reply->ticket == ticket;
}
/* The controller state of a reply with no job of the slot running: the
 * bootstrap may still run after OPEN and LOAD. */
static int idle(enum a733_npu_state state)
{ return state == A733_NPU_BOOTING || state == A733_NPU_READY; }
/* Sum of the logical sizes of the loaded model's tensors in one section. */
static size_t extent(unsigned slot, enum nbg_section_id section)
{
    size_t total = 0;
    struct npu_runtime_tensor t;
    for (unsigned i = 0; i < NBG_ASSEMBLY_MAX_TENSORS &&
         npu_runtime_tensor(job.rt, slot, section, i, &t) == 1; ++i) total += t.bytes;
    return total;
}
/* Copy between the caller's bytes and the tensor windows, in model order. */
static void copy(unsigned slot, enum nbg_section_id section, unsigned char *bytes)
{
    struct npu_runtime_tensor t;
    for (unsigned i = 0; i < NBG_ASSEMBLY_MAX_TENSORS &&
         npu_runtime_tensor(job.rt, slot, section, i, &t) == 1; ++i) {
        if (section == NBG_INPUT) memcpy(t.data, bytes, t.bytes);
        else memcpy(bytes, t.data, t.bytes);
        bytes += t.bytes;
    }
}
int npu_job_run(unsigned model, const void *in, size_t in_bytes, void *out, size_t out_bytes,
                uint64_t deadline_us)
{
    const unsigned slot = own_slot();
    if (slot == NPU_RUNTIME_SLOTS) return refused(1);
    if (job.failed[slot]) return -1;
    struct npu_runtime *rt = job.rt;
    struct npu_runtime_slot *s = &rt->slot[slot];
    if (model >= rt->count || !in || !in_bytes || !out || !out_bytes || !deadline_us ||
        deadline_us > INT64_MAX || s->closed) return refused(2);
    struct npu_service_reply r;
    if (!s->opened) {
        sunstone_crumb(NPU_JOB_CRUMB_OPEN, slot);
        if (!call(slot, NPU_SERVICE_OPEN, 0, deadline_us, &r) || r.result != NPU_SERVICE_OK ||
            r.fault != A733_NPU_NO_FAULT || !r.owns_session || !idle(r.state))
            return failed(slot, 1);
    }
    /* The worker publishes the slot's model before READY, with release. */
    if (atomic_load_explicit(&s->state, memory_order_acquire) != NPU_RUNTIME_READY || s->model != model) {
        sunstone_crumb(NPU_JOB_CRUMB_LOAD, model);
        if (!call(slot, NPU_SERVICE_LOAD, model, 0, &r)) return failed(slot, 2);
        if (r.result == NPU_SERVICE_INVALID) return refused(3);
        if (r.result != NPU_SERVICE_OK || r.fault != A733_NPU_NO_FAULT || !idle(r.state) ||
            atomic_load_explicit(&s->state, memory_order_acquire) != NPU_RUNTIME_READY || s->model != model)
            return failed(slot, 3);
    }
    if (extent(slot, NBG_INPUT) != in_bytes || extent(slot, NBG_OUTPUT) != out_bytes) return refused(4);
    copy(slot, NBG_INPUT, (unsigned char *)(uintptr_t)in);
    sunstone_crumb(NPU_JOB_CRUMB_SUBMIT, model);
    if (!call(slot, NPU_SERVICE_SUBMIT, 0, deadline_us, &r) || r.result != NPU_SERVICE_OK ||
        r.fault != A733_NPU_NO_FAULT || r.state != A733_NPU_DONE || r.job_ticket != r.ticket)
        return failed(slot, 4);
    sunstone_crumb(NPU_JOB_CRUMB_DONE, (uint32_t)r.job_ticket);
    copy(slot, NBG_OUTPUT, out);
    sunstone_crumb(NPU_JOB_CRUMB_COPIED, (uint32_t)out_bytes);
    return 1;
}
int npu_job_close(void)
{
    const unsigned slot = own_slot();
    if (slot == NPU_RUNTIME_SLOTS) return refused(5);
    if (job.failed[slot]) return -1;
    struct npu_runtime_slot *s = &job.rt->slot[slot];
    if (!s->opened || s->closed) return refused(6);
    struct npu_service_reply r;
    sunstone_crumb(NPU_JOB_CRUMB_CLOSE, slot);
    if (!call(slot, NPU_SERVICE_CLOSE, 0, 0, &r) || r.result != NPU_SERVICE_OK ||
        r.fault != A733_NPU_NO_FAULT || r.state != A733_NPU_CLOSED || r.owns_session)
        return failed(slot, 5);
    return 1;
}
