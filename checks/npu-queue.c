#include "npu_queue.h"
#include "a733_npu_sequence.h"
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

/* Model a level event and posted output. The device may signal before full
 * idle; only both conditions permit a fence. The ISR callback has no worker. */
struct hardware { unsigned starts, notices, event, idle, error, fail_start, registration_irq;
    uint32_t address; struct npu_queue *queue; };
static int validate(void *v, const struct a733_npu_command *c)
{
    struct hardware *h = v;
    if (h->registration_irq) npu_queue_irq(h->queue);
    return c->device == 0x10080000 && c->readable_bytes == 524288 && c->token;
}
static int start(void *v, const struct a733_npu_command *c, uint64_t timeout)
{
    struct hardware *h = v;
    assert(!h->event && timeout == 1000);
    ++h->starts; h->address = c->device; h->idle = 0;
    return !h->fail_start;
}
static int complete(void *v)
{
    struct hardware *h = v;
    unsigned event = h->event; h->event = 0;
    return event == 1 && h->idle && !h->error;
}
static void notify(void *v) { ++((struct hardware *)v)->notices; }
static const struct npu_queue_ops ops = {validate, start, complete, notify};
static void done(struct npu_queue *q, struct hardware *h)
{ h->event = 1; h->idle = 1; npu_queue_irq(q); }
static void scenario(unsigned failure)
{
    struct npu_queue q = {0}, foreign = {0};
    struct hardware h = {0};
    struct npu_queue_command c = {0};
    struct a733_npu_command command = {0x10080000, 524288, &h};
    enum { N = 10000 };
    static struct npu_queue_job jobs[N];
    assert(npu_queue_init(&q, &ops, &h));
    assert(!npu_queue_init(&q, &ops, &h));
    if (failure == 4) {
        h.queue = &q; h.registration_irq = 1;
        assert(!npu_queue_register(&q, &c, &command));
        assert(!c.owner && atomic_load(&q.failed) && h.notices == 1 && !h.starts);
        return;
    }
    assert(npu_queue_register(&q, &c, &command));
    assert(!npu_queue_register(&q, &c, &command));
    struct npu_queue_fence previous = {0}, out = {&foreign, 123};
    assert(!npu_queue_submit(&q, &jobs[0], &c, (struct npu_queue_fence){&foreign, 1}, 1000, &out));
    assert(out.queue == &foreign && out.value == 123 && !h.starts && !jobs[0].held);
    assert(!npu_queue_submit(&q, &jobs[0], &c, (struct npu_queue_fence){&q, 1}, 1000, &out));
    for (unsigned i = 0; i < N; ++i) {
        assert(npu_queue_submit(&q, &jobs[i], &c, previous, 1000, &out) == 1);
        assert(out.value == i + 1 && out.queue == &q);
        previous = out;
    }
    assert(h.starts == 1 && c.users == N); /* Caller starts the first job. */
    assert(!npu_queue_submit(&q, &jobs[0], &c, previous, 1000, &out));
    assert(!npu_queue_release(&q, &jobs[0]));
    assert(npu_queue_fence_status(&q, (struct npu_queue_fence){&q, N+1}) == -1);
    if (failure) {
        if (failure == 1) h.error = 1;
        if (failure == 2) { h.event = 1; h.idle = 0; npu_queue_irq(&q); }
        else { if (failure == 3) h.fail_start = 1; done(&q, &h); }
        assert(atomic_load(&q.failed) && h.notices == 1);
        assert(c.users == N && q.held == N && !npu_queue_idle(&q));
        for (unsigned i = 0; i < N; ++i) {
            assert(npu_queue_fence_status(&q, jobs[i].fence) == -1);
            assert(!npu_queue_release(&q, &jobs[i]));
        }
        assert(npu_queue_submit(&q, &jobs[0], &c, previous, 1000, &out) == -1);
        return;
    }
    for (unsigned i = 0; i < N; ++i) {
        done(&q, &h);
        assert(h.notices == i + 1 && h.starts == (i + 2 < N ? i + 2 : N));
        assert(npu_queue_fence_status(&q, jobs[i].fence) == 1);
        assert(npu_queue_release(&q, &jobs[i]));
        assert(!npu_queue_release(&q, &jobs[i]));
    }
    assert(npu_queue_idle(&q) && !c.users && q.submitted == N);
    assert(npu_queue_submit(&q, &jobs[0], &c, previous, 1000, &out) == 1);
    assert(out.value == N + 1); /* Released fence identity remains stable. */
    done(&q, &h); assert(npu_queue_release(&q, &jobs[0]));
    assert(npu_queue_idle(&q));
    /* A stray IRQ is a fault, and cannot be attributed to a later job. */
    npu_queue_irq(&q); assert(atomic_load(&q.failed));
}
static void sequence(void)
{
    static _Alignas(64) unsigned char destination[524288], before[524288];
    const uint32_t first[] = {0x08010a00, 1,
        0x08010e02, 0x30000701, 0x48000000, 0x30000701,
        0x08010e03, 0xc23, 0x08010e01, 0x40, 0x10000000, 0, 0, 0, 0, 0};
    uint32_t second[16]; memcpy(second, first, sizeof second); second[1] = 2;
    struct a733_npu_sequence_part parts[] = {{first, sizeof first}, {second, sizeof second}};
    size_t bytes = 77;
    memset(destination, 0x71, sizeof destination); memcpy(before, destination, sizeof before);
    second[9] = 0; /* Wrong completion event: validation must precede all writes. */
    assert(!a733_npu_sequence_prepare(destination, sizeof destination, parts, 2, &bytes));
    assert(bytes == 77 && !memcmp(before, destination, sizeof before));
    second[9] = 0x40;
    assert(a733_npu_sequence_prepare(destination, sizeof destination, parts, 2, &bytes));
    assert(bytes == 96);
    assert(!memcmp(destination, first, 32));
    assert(!memcmp(destination + 32, second, 32));
    assert(!memcmp(destination + 64, first + 8, 32));
    /* Decode the actual prepared packets: both dependency synchronizations
     * survive, and only the final packet raises an event and ends execution. */
    unsigned events = 0, stalls = 0, marks = 0, ended = 0;
    for (size_t at = 0; at < bytes; at += 8) {
        uint32_t packet[2]; memcpy(packet, destination + at, 8);
        if (packet[0] == 0x08010a00) assert(packet[1] == ++marks);
        if (packet[0] == 0x48000000) { assert(packet[1] == 0x30000701); ++stalls; }
        if (packet[0] == 0x08010e01) { assert(marks == 2 && stalls == 2); ++events; }
        if (packet[0] == 0x10000000) { assert(events == 1); ended = 1; break; }
    }
    assert(events == 1 && stalls == 2 && marks == 2 && ended);
    for (size_t at = bytes; at < sizeof destination; at += 8) {
        uint64_t packet; memcpy(&packet, destination + at, 8); assert(packet == 0x10000000);
    }
}
int main(int argc, char **argv)
{
    sequence();
    scenario(argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 0);
    puts("PASS ordered NPU fences, direct ISR starts, ownership and retention");
    return 0;
}
