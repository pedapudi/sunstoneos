/* Runs every graph of the OS NPU catalogue once through the NPU service and compares the output with the simulator's. */
#include "sunstone_app.h"
#include "FreeRTOS.h"
#include "task.h"
#include "receipt.h"
#include "vectors.h"
#ifdef NPU_GRAPHS_QUEUE
#include "sunstone_npu_queue.h"
#endif
#include <string.h>

static struct npu_graph_receipt receipt;
static uint8_t output[1024 * 1024 / 16];
#define STAGE_CLOCK 0xa0u
#define STAGE_CATALOGUE 0xa1u
#define STAGE_CLOSE 0xa2u
#define GRAPH_TIMEOUT_US 2000000u

static _Noreturn void fault(unsigned stage, unsigned graph, uint32_t value)
{ ss_fail_value((stage << 8) | graph, value); }

void npu_graphs_init(void) {}

#ifdef NPU_GRAPHS_QUEUE
/* Backpressure depth: one resident graph is submitted again and again through the EL0 ring while the application keeps at
 * most k jobs in flight, for k = 1, 2, 4 ... up to the ring. The submit call answers 0 when the ring is full; each such
 * answer is counted. Record i holds k (input_bytes), the jobs (output_bytes), the counter ticks for all of them
 * (first_difference), the submit refusals (observed) and a check of the output at the end (status). */
#define QUEUE_JOBS 1000u
#define QUEUE_TIMEOUT_US 1000000u
static void queue_fault(unsigned stage, uint32_t value) { ss_fail_value((0xb0u << 8) | stage, value); }
static _Noreturn void npu_graphs_queue_main(void)
{
    const struct npu_graph_vector *v = &npu_graph_vectors[0];
    struct ss_npu_queue *q = NULL;
    uint64_t begin = ss_counter_ticks();
    int opened;
    while ((opened = ss_npu_queue_open(SS_NPU_QUEUE_MAX_DEPTH, &q)) != 1) {
        if (opened < 0 || (ss_counter_ticks() - begin) / 24 > 3000000) queue_fault(1, (uint32_t)opened);
        ss_progress(); vTaskDelay(1);
    }
    ss_npu_handle h;
    if (!ss_npu_queue_register(q, 0, &h)) queue_fault(2, 0);
    size_t offset = 0;
    for (unsigned i = 0;; ++i) {
        struct ss_npu_tensor t;
        if (!ss_npu_queue_tensor(q, h, SS_NPU_INPUT, i, &t)) break;
        if (offset + t.bytes > v->input_bytes) queue_fault(3, i);
        memcpy(t.data, v->input + offset, t.bytes);
        if (!ss_npu_queue_dirty(q, h, SS_NPU_INPUT, i, 0, t.bytes)) queue_fault(4, i);
        offset += t.bytes;
    }
    if (offset != v->input_bytes) queue_fault(5, (uint32_t)offset);
    static struct ss_npu_fence fences[SS_NPU_QUEUE_MAX_DEPTH];
    unsigned records = 0;
    receipt.start = ss_counter_ticks();
    for (unsigned k = 1; k <= SS_NPU_QUEUE_MAX_DEPTH; k *= 2, ++records) {
        struct npu_graph_record *r = &receipt.graph[records];
        r->index = records; r->input_bytes = k; r->output_bytes = QUEUE_JOBS; r->status = NPU_GRAPH_NOT_RUN;
        unsigned submitted = 0, completed = 0, refused = 0;
        const uint64_t t0 = ss_counter_ticks();
        unsigned spins = 0;
        while (completed < QUEUE_JOBS) {
            while (submitted < QUEUE_JOBS && submitted - completed < k) {
                const int r1 = ss_npu_queue_submit(q, h, (struct ss_npu_fence){0}, QUEUE_TIMEOUT_US, &fences[submitted % SS_NPU_QUEUE_MAX_DEPTH]);
                if (r1 == 1) ++submitted; else if (r1 == 0) { ++refused; break; } else queue_fault(6, submitted);
            }
            const int done = ss_npu_queue_status(q, fences[completed % SS_NPU_QUEUE_MAX_DEPTH]);
            if (done == 1) {
                if (ss_npu_queue_release(q, fences[completed % SS_NPU_QUEUE_MAX_DEPTH]) != 1) queue_fault(7, completed);
                ++completed;
            } else if (done < 0) queue_fault(8, completed);
            if ((++spins & 0x3ff) == 0) { ss_progress(); if (ss_live() < 0) queue_fault(9, completed); }
        }
        r->first_difference = ss_counter_ticks() - t0; r->observed = refused;
        r->ticks = r->first_difference / QUEUE_JOBS;
        r->status = NPU_GRAPH_MATCH;
        ++receipt.completed;
    }
    /* The output after the last job equals the reference. */
    begin = ss_counter_ticks();
    int acquired;
    while ((acquired = ss_npu_queue_acquire_handle(q, h)) != 1) {
        if (acquired < 0 || (ss_counter_ticks() - begin) / 24 > 2000000) queue_fault(10, (uint32_t)acquired);
        ss_progress(); vTaskDelay(1);
    }
    offset = 0;
    unsigned wrong = 0;
    for (unsigned i = 0;; ++i) {
        struct ss_npu_tensor t;
        if (!ss_npu_queue_tensor(q, h, SS_NPU_OUTPUT, i, &t)) break;
        for (size_t b = 0; b < t.bytes && offset + b < v->reference_bytes; ++b) wrong += ((const uint8_t *)t.data)[b] != v->reference[offset + b];
        offset += t.bytes;
    }
    receipt.matched = (wrong == 0 && offset == v->reference_bytes) ? receipt.completed : 0;
    for (unsigned i = 0; i < records; ++i) receipt.differing_bytes[i] = wrong;
    begin = ss_counter_ticks();
    int closed;
    while ((closed = ss_npu_queue_close(q)) != 1) {
        if (closed < 0 || (ss_counter_ticks() - begin) / 24 > 2000000) queue_fault(11, (uint32_t)closed);
        ss_progress(); vTaskDelay(1);
    }
    receipt.count = records; receipt.sdk_closed = 1;
    receipt.stop = ss_counter_ticks();
    receipt.magic = NPU_GRAPHS_MAGIC; receipt.version = 1; receipt.bytes = sizeof(receipt);
    ss_finish(&receipt, sizeof(receipt));
}
#endif

void npu_graphs_main(const struct ss_grant *grant)
{
    uint64_t begin = ss_counter_ticks();
    while (!ss_clock_ready() || ss_throttled()) {
        if (ss_live() < 0) fault(STAGE_CLOCK, 0, 0);
        if (ss_counter_ticks() - begin > 240000000) fault(STAGE_CLOCK, 0, 1);
        vTaskDelay(1);
    }
#ifdef NPU_GRAPHS_QUEUE
    /* One graph is aliased under many names, one resident slot each, so the list is longer than the receipt. */
    if (npu_graph_count == 0) fault(STAGE_CATALOGUE, 0, npu_graph_count);
    npu_graphs_queue_main();
#endif
    if (npu_graph_count == 0 || npu_graph_count > NPU_GRAPHS_MAX) fault(STAGE_CATALOGUE, 0, npu_graph_count);
    const unsigned total = npu_graph_count + npu_graph_runtime_count;
    if (total > NPU_GRAPHS_MAX) fault(STAGE_CATALOGUE, 0, total);
    receipt.count = total;
    receipt.start = ss_counter_ticks();
    unsigned failed = 0;
    for (unsigned i = 0; i < total; ++i) {
        const int at_run_time = i >= npu_graph_count;
        const struct npu_graph_vector *v = at_run_time ? &npu_graph_runtime[i - npu_graph_count].vector : &npu_graph_vectors[i];
        struct npu_graph_record *r = &receipt.graph[i];
        unsigned model;
        struct ss_npu_model info;
        r->index = i; r->input_bytes = v->input_bytes; r->output_bytes = v->reference_bytes; r->status = NPU_GRAPH_NOT_RUN;
        if (failed) continue;
        if (at_run_time) {
            /* A graph that no list of the OS image names: hand its bytes to the OS, which admits them and gives the index. */
            const struct npu_graph_runtime *g = &npu_graph_runtime[i - npu_graph_count];
            int registered;
            const uint64_t t = ss_counter_ticks();
            while ((registered = ss_npu_register_graph(g->model, g->model_bytes, &model)) == 0) {
                if (ss_live() < 0 || (ss_counter_ticks() - t) / 24 > GRAPH_TIMEOUT_US) { registered = -1; break; }
                ss_progress(); vTaskDelay(1);
            }
            if (registered != 1) { r->status = registered == -2 ? NPU_GRAPH_REFUSED : NPU_GRAPH_FAILED; failed = registered != -2; continue; }
            if (v->reference_bytes > sizeof(output)) fault(STAGE_CATALOGUE, i, model);
        } else if (ss_npu_model_find(v->name, &model) != 1 || ss_npu_model_info(model, &info) != 1 ||
            info.input_bytes != v->input_bytes || info.output_bytes != v->reference_bytes ||
            v->reference_bytes > sizeof(output)) fault(STAGE_CATALOGUE, i, model);
        memset(output, 0xa5, v->reference_bytes);
        const uint64_t t0 = ss_counter_ticks();
        for (;;) {
            if (ss_live() < 0) fault(STAGE_CATALOGUE, i, 0xdead);
            const int ran = ss_npu_run(model, v->input, v->input_bytes, output, v->reference_bytes, 1000000);
            if (ran == 1) {
                r->ticks = ss_counter_ticks() - t0;
                r->status = NPU_GRAPH_MATCH;
                for (size_t b = 0; b < v->reference_bytes; ++b)
                    if (output[b] != v->reference[b]) {
                        if (r->status == NPU_GRAPH_MATCH) {
                            r->status = NPU_GRAPH_MISMATCH; r->first_difference = b;
                            r->observed = output[b]; r->expected = v->reference[b];
                        }
                        ++receipt.differing_bytes[i];
                    }
                for (size_t b = 0; b + 1 < v->reference_bytes; b += 2) {
                    const int a = (int16_t)(output[b] | output[b + 1] << 8), e = (int16_t)(v->reference[b] | v->reference[b + 1] << 8);
                    const uint32_t d = (uint32_t)(a > e ? a - e : e - a);
                    if (d > receipt.max_error[i]) receipt.max_error[i] = d;
                }
                ++receipt.completed;
                if (r->status == NPU_GRAPH_MATCH) ++receipt.matched;
                break;
            }
            if (ran == -3) {
                /* An NPU MMU exception failed this job; the NPU was reset and the session goes on. */
                struct ss_npu_fault_info f;
                r->status = NPU_GRAPH_FAULT; r->ticks = ss_counter_ticks() - t0;
                if (ss_npu_last_fault(&f) == 1) {
                    r->first_difference = f.mmu_address; r->observed = f.mmu_status; r->expected = f.canary_crc;
                    receipt.differing_bytes[i] = f.marks; receipt.max_error[i] = f.window_ms;
                }
                break;
            }
            if (ran == -2) { r->status = NPU_GRAPH_REFUSED; break; }
            if (ran < 0) { r->status = NPU_GRAPH_FAILED; failed = 1; break; }
            if ((ss_counter_ticks() - t0) / 24 > GRAPH_TIMEOUT_US) { r->status = NPU_GRAPH_TIMEOUT; failed = 1; break; }
            ss_progress();
            vTaskDelay(1);
        }
    }
    begin = ss_counter_ticks();
    for (;;) {
        if (ss_live() < 0 || (ss_counter_ticks() - begin) / 24 > 2000000) break;
        const int closed = ss_npu_close();
        if (closed == 1) { receipt.sdk_closed = 1; break; }
        if (closed < 0) break;
        vTaskDelay(1);
    }
    receipt.stop = ss_counter_ticks();
    receipt.magic = NPU_GRAPHS_MAGIC; receipt.version = 1; receipt.bytes = sizeof(receipt);
    (void)grant;
    ss_finish(&receipt, sizeof(receipt));
}
void platform_puts(const char *text) { (void)text; ss_fail(0x8d0); }
