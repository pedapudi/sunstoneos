/* The OS side of the NPU graph application (apps/npu-graphs/main.c) on the host: the SDK calls it makes, with the rules the OS
 * applies to them, so that the application's own code runs its order of calls and its receipt is decoded as on the board.
 *
 * The rules come from the OS code they model:
 * - registration (runtime/npu_app.c, drivers/npu/a733_npu_runtime.c npu_runtime_register): answered by the OS's NPU task, so
 *   each request is pending once; refused beyond the declared graph count, above graph_bytes, or when the graph's read or
 *   write region (tools/npu_graph_layout.py: inputs; memory pool and outputs) exceeds tensor_bytes in pages plus 64 bytes;
 * - the ring (apps/os/npu-queue.c open_queue): only in a session whose declaration selects it (in_flight above 1), at most
 *   the declared depth and links; the OS's NPU task answers it, so the first request is pending; it loads catalogue graph i
 *   into slot i for every declared slot and is refused (0x703) when a slot has no registered graph; links join an output and
 *   an input of the same size;
 * - submit refuses (0) while the declared depth of jobs is in flight; a handle's tensors are unavailable while the device
 *   owns its group (include/sunstone_npu_queue.h);
 * - ss_finish (apps/os/application.c) waits for the NPU client's report, which only the close publishes: a finish before the
 *   close stops at detail 0x75f.
 * Jobs compute the graphs this model knows (graph-add: float16 inputs summed to int16 at fixed position 12; graph-relu:
 * int16 at fixed position 13), so a linked pair gives the output the device would. Any other graph gives its reference only
 * for its own reference input. */
#include "sunstone_app.h"
#include "sunstone_npu_queue.h"
#include "vectors.h"
#include "npu_graphs-app.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct model_graph { int kind; unsigned inputs, outputs; size_t in_bytes[4], out_bytes[4]; size_t read, write; };
extern const struct model_graph model_graphs[];
enum { OTHER, ADD, RELU };

static const char *destination;
/* Time is the model's own: a microsecond for every counter read and a millisecond for every task delay, so the application's
 * timeouts end the same way on every run. */
static uint64_t now = 24000000u;
uint64_t ss_counter_ticks(void) { return now += 24; }
int ss_live(void) { return 1; }
void ss_progress(void) {}
int ss_clock_ready(void) { return 1; }
int ss_throttled(void) { return 0; }
int ss_clock_info(char *line, size_t n)
{ return snprintf(line, n, "CLOCK cpu_l_mhz=1794 cpu_b_mhz=2002 cpu_l_mv=1000 cpu_b_mv=1050 transitions=4 dsu_mhz=1274 core=0\n"); }
void vTaskDelay(unsigned ticks) { now += 24000u * ticks; }
_Noreturn void ss_fail_value(uint32_t detail, uint32_t value)
{ printf("FIRST_FAULT detail=%08x value=%08x\n", detail, value); fflush(stdout); _Exit(2); }
_Noreturn void ss_fail(uint32_t detail) { ss_fail_value(detail, detail); }

static unsigned registered, register_pending, sync_pending, sync_closed, queue_closed;
static size_t pages(size_t b) { return (b + 4095) & ~(size_t)4095; }

_Noreturn void ss_finish(const void *data, size_t bytes)
{
    if (!sync_closed && !queue_closed) ss_fail_value(0x75f, 0);
    if (bytes > 1728) ss_fail(0x756);
    FILE *f = fopen(destination, "wb");
    if (!f || fwrite(data, 1, bytes, f) != bytes || fclose(f)) abort();
    _Exit(0);
}

int ss_npu_register_graph(const void *graph, size_t bytes, unsigned *index)
{
    if (sync_closed || queue_closed || !graph || !bytes || !index) return -1;
    if (!register_pending) { register_pending = 1; return 0; }
    register_pending = 0;
    const unsigned i = registered;
    if (i >= NPU_GRAPHS_DECL_GRAPHS || i >= npu_graph_runtime_count || graph != npu_graph_runtime[i].model ||
        bytes != npu_graph_runtime[i].model_bytes) return -2;
    const struct model_graph *g = &model_graphs[i];
    if (bytes > NPU_GRAPHS_DECL_GRAPH_BYTES || g->read > pages(NPU_GRAPHS_DECL_TENSOR_BYTES + 64) ||
        g->write > pages(NPU_GRAPHS_DECL_TENSOR_BYTES + 64)) return -2;
    *index = npu_graph_count + registered++;
    return 1;
}

/* One job of graph g: from its inputs to its outputs. */
static void compute(const struct model_graph *g, const struct npu_graph_vector *v, uint8_t *const in[], uint8_t *const out[])
{
    if (g->kind == ADD) {
        const _Float16 *a = (const _Float16 *)in[0], *b = (const _Float16 *)in[1];
        int16_t *o = (int16_t *)out[0];
        for (size_t i = 0; i < g->out_bytes[0] / 2; ++i) {
            double s = rint(((double)a[i] + (double)b[i]) * 4096.0);
            if (s != s) s = 0;   /* a NaN input (int16 bits read as float16 by a wrong link): any value fails the reference */
            o[i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
        }
        return;
    }
    if (g->kind == RELU) {
        const int16_t *a = (const int16_t *)in[0];
        int16_t *o = (int16_t *)out[0];
        for (size_t i = 0; i < g->out_bytes[0] / 2; ++i) o[i] = a[i] > 0 ? a[i] : 0;
        return;
    }
    size_t at = 0;
    int same = 1;
    for (unsigned k = 0; k < g->inputs; ++k) { same &= !memcmp(in[k], v->input + at, g->in_bytes[k]); at += g->in_bytes[k]; }
    at = 0;
    for (unsigned k = 0; k < g->outputs; ++k) {
        if (same) memcpy(out[k], v->reference + at, g->out_bytes[k]); else memset(out[k], 0xa5, g->out_bytes[k]);
        at += g->out_bytes[k];
    }
}
static const struct npu_graph_vector *vector(unsigned i) { return &npu_graph_runtime[i - npu_graph_count].vector; }

/* The synchronous mailbox (runtime/npu_app.c with the OS backend). */
int ss_npu_model_find(const char *name, unsigned *index) { (void)name; (void)index; return 0; }
int ss_npu_model_info(unsigned index, struct ss_npu_model *m)
{
    if (!m || index >= npu_graph_count + registered) return -1;
    if (!sync_pending) { sync_pending = 1; return 0; }
    sync_pending = 0;
    const struct model_graph *g = &model_graphs[index - npu_graph_count];
    memset(m, 0, sizeof(*m));
    m->inputs = g->inputs; m->outputs = g->outputs;
    for (unsigned k = 0; k < g->inputs; ++k) m->input_bytes += g->in_bytes[k];
    for (unsigned k = 0; k < g->outputs; ++k) m->output_bytes += g->out_bytes[k];
    return 1;
}
int ss_npu_run(unsigned model, const void *input, size_t input_bytes, void *output, size_t output_bytes, uint64_t timeout)
{
    if (sync_closed || !timeout || timeout > 1000000) return -1;
    if (!sync_pending) { sync_pending = 1; return 0; }
    sync_pending = 0;
    if (model < npu_graph_count || model >= npu_graph_count + registered) return -2;
    const struct model_graph *g = &model_graphs[model - npu_graph_count];
    size_t ni = 0, no = 0;
    for (unsigned k = 0; k < g->inputs; ++k) ni += g->in_bytes[k];
    for (unsigned k = 0; k < g->outputs; ++k) no += g->out_bytes[k];
    if (ni != input_bytes || no != output_bytes) return -2;
    uint8_t *in[4], *out[4];
    size_t at = 0;
    for (unsigned k = 0; k < g->inputs; ++k) { in[k] = (uint8_t *)input + at; at += g->in_bytes[k]; }
    at = 0;
    for (unsigned k = 0; k < g->outputs; ++k) { out[k] = (uint8_t *)output + at; at += g->out_bytes[k]; }
    compute(g, vector(model), in, out);
    return 1;
}
int ss_npu_close(void) { if (!sync_pending) { sync_pending = 1; return 0; } sync_pending = 0; sync_closed = 1; return 1; }
int ss_npu_last_fault(struct ss_npu_fault_info *f) { (void)f; return 0; }

/* The EL0 ring. */
struct job { unsigned slot; uint64_t sequence, after; int done, released; };
struct ss_npu_queue {
    unsigned depth, slots, links;
    struct ss_npu_queue_edge edge[8];
    uint8_t *in[64][4], *out[64][4];
    int owned[64];
    struct job *jobs; size_t job_count, job_capacity;
    uint64_t sequence, released;
};
static struct ss_npu_queue ring;
static int ring_open, open_asked;

static unsigned group_of(unsigned slot)
{
    for (unsigned e = 0; e < ring.links; ++e)
        if (ring.edge[e].consumer == slot || ring.edge[e].producer == slot) return ring.edge[e].producer < ring.edge[e].consumer ? ring.edge[e].producer : ring.edge[e].consumer;
    return slot;
}
int ss_npu_queue_open_linked(unsigned depth, const struct ss_npu_queue_edge *links, size_t count, struct ss_npu_queue **out)
{
    if (!out || ring_open || NPU_GRAPHS_DECL_IN_FLIGHT <= 1) return -1;
    if (!depth || depth > NPU_GRAPHS_DECL_IN_FLIGHT || count > NPU_GRAPHS_DECL_LINKS || count > 8 || (count && !links)) return -1;
    if (!open_asked) { open_asked = 1; memcpy(ring.edge, links, count * sizeof(*links)); ring.links = (unsigned)count; return 0; }
    if (registered < NPU_GRAPHS_DECL_SLOTS) return -1;   /* the open loads slot i with graph i for every declared slot */
    ring.depth = depth; ring.slots = NPU_GRAPHS_DECL_SLOTS;
    if (ring.slots > 64) return -1;
    for (unsigned s = 0; s < ring.slots; ++s) {
        const struct model_graph *g = &model_graphs[s];
        for (unsigned k = 0; k < g->inputs; ++k) ring.in[s][k] = calloc(1, g->in_bytes[k]);
        for (unsigned k = 0; k < g->outputs; ++k) ring.out[s][k] = calloc(1, g->out_bytes[k]);
    }
    for (unsigned e = 0; e < ring.links; ++e) {
        const struct ss_npu_queue_edge *x = &ring.edge[e];
        if (x->producer >= ring.slots || x->consumer >= ring.slots || x->producer == x->consumer ||
            x->output >= model_graphs[x->producer].outputs || x->input >= model_graphs[x->consumer].inputs ||
            model_graphs[x->producer].out_bytes[x->output] != model_graphs[x->consumer].in_bytes[x->input]) return -1;
        free(ring.in[x->consumer][x->input]);
        ring.in[x->consumer][x->input] = ring.out[x->producer][x->output];
    }
    ring_open = 1; *out = &ring;
    return 1;
}
int ss_npu_queue_open(unsigned depth, struct ss_npu_queue **out) { return ss_npu_queue_open_linked(depth, NULL, 0, out); }
int ss_npu_queue_register(struct ss_npu_queue *q, unsigned entry, ss_npu_handle *out)
{ if (q != &ring || !ring_open || !out || entry >= ring.slots) return 0; *out = entry + 1; return 1; }
int ss_npu_queue_tensor(struct ss_npu_queue *q, ss_npu_handle h, enum ss_npu_tensor_kind kind, unsigned index, struct ss_npu_tensor *t)
{
    if (q != &ring || !ring_open || !t || !h || h > ring.slots || ring.owned[group_of(h - 1)]) return 0;
    const struct model_graph *g = &model_graphs[h - 1];
    if (index >= (kind == SS_NPU_INPUT ? g->inputs : g->outputs)) return 0;
    *t = kind == SS_NPU_INPUT ? (struct ss_npu_tensor){ring.in[h - 1][index], g->in_bytes[index]}
                              : (struct ss_npu_tensor){ring.out[h - 1][index], g->out_bytes[index]};
    return 1;
}
int ss_npu_queue_dirty(struct ss_npu_queue *q, ss_npu_handle h, enum ss_npu_tensor_kind kind, unsigned index, size_t offset, size_t bytes)
{ struct ss_npu_tensor t; return ss_npu_queue_tensor(q, h, kind, index, &t) && bytes && offset <= t.bytes && bytes <= t.bytes - offset; }
int ss_npu_queue_submit(struct ss_npu_queue *q, ss_npu_handle h, struct ss_npu_fence after, uint64_t timeout, struct ss_npu_fence *fence)
{
    if (q != &ring || !ring_open || queue_closed || !h || h > ring.slots || !fence || !timeout) return -1;
    if (after.value && (after.queue != &ring || after.value > ring.sequence)) return 0;
    if (ring.sequence - ring.released >= ring.depth) return 0;   /* backpressure: the declared depth is in flight */
    if (ring.job_count == ring.job_capacity) {
        ring.job_capacity = ring.job_capacity ? 2 * ring.job_capacity : 256;
        ring.jobs = realloc(ring.jobs, ring.job_capacity * sizeof(*ring.jobs));
    }
    ring.jobs[ring.job_count++] = (struct job){h - 1, ++ring.sequence, after.value, 0, 0};
    ring.owned[group_of(h - 1)] = 1;
    *fence = (struct ss_npu_fence){&ring, ring.sequence};
    return 1;
}
static struct job *job_of(uint64_t sequence) { return sequence && sequence <= ring.job_count ? &ring.jobs[sequence - 1] : NULL; }
int ss_npu_queue_status(struct ss_npu_queue *q, struct ss_npu_fence f)
{
    struct job *j = q == &ring ? job_of(f.value) : NULL;
    if (!j) return -1;
    /* Jobs complete in FIFO order: complete every job up to this one. */
    for (uint64_t s = 1; s <= f.value; ++s) {
        struct job *k = job_of(s);
        if (k->done) continue;
        const struct model_graph *g = &model_graphs[k->slot];
        compute(g, vector(npu_graph_count + k->slot), ring.in[k->slot], ring.out[k->slot]);
        k->done = 1;
    }
    return 1;
}
int ss_npu_queue_release(struct ss_npu_queue *q, struct ss_npu_fence f)
{
    struct job *j = q == &ring ? job_of(f.value) : NULL;
    if (!j || !j->done || j->released || f.value != ring.released + 1) return 0;   /* FIFO */
    j->released = 1; ++ring.released;
    return 1;
}
int ss_npu_queue_acquire_handle(struct ss_npu_queue *q, ss_npu_handle h)
{
    if (q != &ring || !h || h > ring.slots) return -1;
    const unsigned group = group_of(h - 1);
    for (size_t i = 0; i < ring.job_count; ++i) if (group_of(ring.jobs[i].slot) == group && !ring.jobs[i].done) return 0;
    ring.owned[group] = 0;
    return 1;
}
int ss_npu_queue_close(struct ss_npu_queue *q)
{
    if (q != &ring || !ring_open) return -1;
    for (size_t i = 0; i < ring.job_count; ++i) if (!ring.jobs[i].done) return 0;
    queue_closed = 1;
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 2) return 4;
    destination = argv[1];
    static struct ss_grant grant;
    npu_graphs_main(&grant);
    puts("npu_graphs_main returned");
    return 3;
}
