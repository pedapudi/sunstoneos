#include "npu_queue.h"
#include <limits.h>

static int healthy(const struct npu_queue *q)
{ return q && q->initialized && !atomic_load_explicit(&q->failed, memory_order_acquire); }
static int retain(struct npu_queue *q, unsigned site)
{
    q->failure_site = site; q->failure_fence = q->head ? q->head->fence.value : 0;
    atomic_store_explicit(&q->failed, 1, memory_order_release); return -1;
}
int npu_queue_init(struct npu_queue *q, const struct npu_queue_ops *ops, void *context)
{
    if (!q || q->initialized || !ops || !ops->validate || !ops->start ||
        !ops->complete || !ops->notify_from_isr || !context) return 0;
    q->ops = *ops; q->context = context;
    atomic_init(&q->completed, 0); atomic_init(&q->failed, 0);
    q->initialized = 1;
    return 1;
}
int npu_queue_register(struct npu_queue *q, struct npu_queue_command *c,
                       const struct a733_npu_command *hardware)
{
    if (!healthy(q) || !c || c->owner || !hardware || q->head ||
        !q->ops.validate(q->context, hardware)) return 0;
    c->hardware = *hardware; c->owner = q;
    return 1;
}
int npu_queue_submit(struct npu_queue *q, struct npu_queue_job *job,
                     struct npu_queue_command *command, struct npu_queue_fence after,
                     uint64_t timeout, struct npu_queue_fence *out)
{
    if (!q || !q->initialized) return 0;
    if (!healthy(q)) return -1;
    if (!job || job->held || !command || command->owner != q || !out ||
        !timeout || timeout > INT64_MAX || q->submitted == UINT64_MAX ||
        q->held == SIZE_MAX || command->users == SIZE_MAX ||
        (after.value ? after.queue != q || after.value > q->submitted : after.queue != NULL)) return 0;
    const struct npu_queue_fence fence = {q, q->submitted + 1};
    job->next = NULL; job->command = command; job->fence = fence;
    job->timeout_us = timeout; job->held = 1;
    ++command->users; ++q->held; q->submitted = fence.value;
    if (q->tail) q->tail->next = job;
    else q->head = job;
    q->tail = job;
    *out = fence;
    if (q->head == job && q->ops.start(q->context, &command->hardware, timeout) != 1)
        return retain(q, 1);
    return 1;
}
void npu_queue_irq(struct npu_queue *q)
{
    if (!q || !q->initialized) return;
    if (!healthy(q)) { q->ops.notify_from_isr(q->context); return; }
    struct npu_queue_job *job = q->head;
    if (!job || q->ops.complete(q->context) != 1) {
        (void)retain(q, 2); q->ops.notify_from_isr(q->context); return;
    }
    q->head = job->next;
    if (!q->head) q->tail = NULL;
    /* complete proves full idle and orders device writes. CPU consumers must
     * still invalidate their output lines before reading them. */
    atomic_store_explicit(&q->completed, job->fence.value, memory_order_release);
    if (q->head && q->ops.start(q->context, &q->head->command->hardware,
                              q->head->timeout_us) != 1) (void)retain(q, 3);
    q->ops.notify_from_isr(q->context);
}
int npu_queue_fence_status(const struct npu_queue *q, struct npu_queue_fence fence)
{
    if (!q || !q->initialized || fence.queue != q || !fence.value || fence.value > q->submitted ||
        !healthy(q)) return -1;
    if (atomic_load_explicit(&q->completed, memory_order_acquire) >= fence.value) return 1;
    return 0;
}
int npu_queue_release(struct npu_queue *q, struct npu_queue_job *job)
{
    if (!healthy(q) || !job || !job->held || job->fence.queue != q ||
        npu_queue_fence_status(q, job->fence) != 1) return 0;
    --job->command->users; --q->held; job->held = 0;
    return 1;
}
int npu_queue_idle(const struct npu_queue *q)
{ return healthy(q) && !q->head && !q->held; }
