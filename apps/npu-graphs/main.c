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
#include <stdlib.h>

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
 * most k jobs in flight, for k = 1, 2, 4 ... up to twice the ring. The submit call answers 0 when the ring is full; each such
 * answer is counted. Record i holds k (input_bytes), the jobs (output_bytes), the counter ticks for all of them
 * (first_difference), the submit refusals (observed) and a check of the output at the end (status). */
#ifndef QUEUE_JOBS
#define QUEUE_JOBS 1000u
#endif
/* The largest in-flight limit of the sweep (default: twice the ring, so that the ring refuses). */
#ifndef QUEUE_KMAX
#define QUEUE_KMAX (2 * SS_NPU_QUEUE_MAX_DEPTH)
#endif
#define QUEUE_TIMEOUT_US 1000000u
static void queue_fault(unsigned stage, uint32_t value) { ss_fail_value((0xb0u << 8) | stage, value); }
/* The CPU clock record of the OS (ss_clock_info) as it is when read: the applied points of the little (Cortex-A55) and big
 * clusters in MHz, their rails in mV, the DSU point, the transitions made and the core of the caller, in three words. */
static void clock_words(uint64_t *a, uint64_t *b, uint64_t *c)
{
    static const char *const keys[] = {"cpu_l_mhz=", "cpu_b_mhz=", "cpu_l_mv=", "cpu_b_mv=", "transitions=", "dsu_mhz=", "core="};
    char line[192];
    unsigned long v[7] = {0};
    if (ss_clock_info(line, sizeof(line)) > 0)
        for (unsigned i = 0; i < 7; ++i) { const char *at = strstr(line, keys[i]); if (at) v[i] = strtoul(at + strlen(keys[i]), NULL, 10); }
    *a = v[0] | v[1] << 16 | v[6] << 32 | v[4] << 40; *b = v[2] | v[3] << 16 | v[5] << 32; *c = 1;
}
static __attribute__((unused)) _Noreturn void npu_graphs_queue_main(void)
{
    const struct npu_graph_vector *v = &npu_graph_vectors[0];
    struct ss_npu_queue *q = NULL;
    uint64_t begin = ss_counter_ticks();
    int opened;
    /* One tensor link chosen now, after the scheduler started: the output of the first resident graph feeds the input of the
     * second (the graph is the same one under two names, and it maps its input to an output of the same representation). */
    static const struct ss_npu_queue_edge link = {0, 0, 1, 0};
#ifdef NPU_GRAPHS_QUEUE_NO_LINK
    const size_t links = 0;   /* an image with one resident graph has nothing to link */
#else
    const size_t links = 1;
#endif
    while ((opened = ss_npu_queue_open_linked(SS_NPU_QUEUE_MAX_DEPTH, &link, links, &q)) != 1) {
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
    clock_words(&receipt.reserved[0], &receipt.reserved[1], &receipt.reserved[2]);   /* before the sweep */
    receipt.start = ss_counter_ticks();
    /* The last k is twice the ring: the ring, not k, then limits the jobs in flight and refuses the rest. */
    for (unsigned k = 1; k <= QUEUE_KMAX; k *= 2, ++records) {
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
    const unsigned sweeps = records;
    clock_words(&receipt.reserved[3], &receipt.reserved[4], &receipt.reserved[5]);   /* after the sweep */
#ifndef NPU_GRAPHS_QUEUE_NO_LINK
    /* The link: only handle 1 has its input written. Handle 2 runs after it and reads handle 1's output as its input, so its
     * output equals the reference only if the link was made (its own input was never written and is zero). Record i holds
     * 1 link (input_bytes), 1 pair (output_bytes), the ticks of the pair (first_difference) and the wrong bytes of handle
     * 2's output (differing_bytes). */
    {
        ss_npu_handle h2;
        if (!ss_npu_queue_register(q, 1, &h2)) queue_fault(12, 0);
        struct npu_graph_record *r = &receipt.graph[records];
        r->index = records; r->input_bytes = 1; r->output_bytes = 1; r->status = NPU_GRAPH_NOT_RUN;
        struct ss_npu_fence f1, f2;
        const uint64_t t0 = ss_counter_ticks();
        int s1;
        while ((s1 = ss_npu_queue_submit(q, h, (struct ss_npu_fence){0}, QUEUE_TIMEOUT_US, &f1)) == 0) { ss_progress(); }
        if (s1 < 0) queue_fault(13, 0);
        while ((s1 = ss_npu_queue_submit(q, h2, f1, QUEUE_TIMEOUT_US, &f2)) == 0) { ss_progress(); }
        if (s1 < 0) queue_fault(14, 0);
        unsigned spins = 0;
        int done;
        while ((done = ss_npu_queue_status(q, f2)) != 1) {
            if (done < 0) queue_fault(15, 0);
            if ((++spins & 0x3ff) == 0) { ss_progress(); if (ss_live() < 0) queue_fault(16, 0); }
        }
        r->first_difference = ss_counter_ticks() - t0;
        if (ss_npu_queue_release(q, f1) != 1 || ss_npu_queue_release(q, f2) != 1) queue_fault(17, 0);
        uint64_t waited = ss_counter_ticks();
        int got;
        while ((got = ss_npu_queue_acquire_handle(q, h2)) != 1) {
            if (got < 0 || (ss_counter_ticks() - waited) / 24 > 2000000) queue_fault(18, (uint32_t)got);
            ss_progress(); vTaskDelay(1);
        }
        size_t at = 0;
        unsigned bad = 0;
        for (unsigned i = 0;; ++i) {
            struct ss_npu_tensor t;
            if (!ss_npu_queue_tensor(q, h2, SS_NPU_OUTPUT, i, &t)) break;
            for (size_t b = 0; b < t.bytes && at + b < v->reference_bytes; ++b) bad += ((const uint8_t *)t.data)[b] != v->reference[at + b];
            at += t.bytes;
        }
        r->ticks = r->first_difference;
        r->status = (bad == 0 && at == v->reference_bytes) ? NPU_GRAPH_MATCH : NPU_GRAPH_MISMATCH;
        receipt.differing_bytes[records] = bad;
        if (r->status == NPU_GRAPH_MATCH) ++receipt.completed;
        ++records;
    }
#endif
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
    for (unsigned i = 0; i < sweeps; ++i) receipt.differing_bytes[i] = wrong;
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

#ifdef NPU_GRAPHS_SEQUENCE
/* One chunk through a sequence of resident graphs: every graph of the model list is resident in its own slot, and each round
 * submits one job for each of them in list order (they are independent jobs with their own inputs), as the layers of a
 * network follow one another. Rounds repeat; the time of every round is taken, and at the end every graph's output is
 * compared with the simulator's. Receipt: record 0 holds the graphs per round (input_bytes), the rounds (output_bytes), the
 * ticks of all rounds (first_difference), the submit refusals (observed), the longest round (expected) and the shortest
 * (ticks); record 1 holds the bit mask of the graphs whose output matched (input_bytes), the graph count (output_bytes)
 * and the sum of wrong bytes (first_difference); differing_bytes[i] and then max_error[i] hold the wrong bytes of graph i. */
#ifndef SEQUENCE_ROUNDS
#define SEQUENCE_ROUNDS 20u
#endif
static _Noreturn void npu_graphs_sequence_main(void)
{
    const unsigned n = npu_graph_count;
    if (n == 0 || n > SS_NPU_QUEUE_MAX_HANDLES || n > SS_NPU_QUEUE_MAX_DEPTH || n > 32) queue_fault(0, n);
    struct ss_npu_queue *q = NULL;
    uint64_t begin = ss_counter_ticks();
    int opened;
    while ((opened = ss_npu_queue_open(SS_NPU_QUEUE_MAX_DEPTH, &q)) != 1) {
        if (opened < 0 || (ss_counter_ticks() - begin) / 24 > 6000000) queue_fault(1, (uint32_t)opened);
        ss_progress(); vTaskDelay(1);
    }
    ss_npu_handle h[32];
    for (unsigned g = 0; g < n; ++g) {
        const struct npu_graph_vector *v = &npu_graph_vectors[g];
        if (!ss_npu_queue_register(q, g, &h[g])) queue_fault(2, g);
        size_t offset = 0;
        for (unsigned i = 0;; ++i) {
            struct ss_npu_tensor t;
            if (!ss_npu_queue_tensor(q, h[g], SS_NPU_INPUT, i, &t)) break;
            if (offset + t.bytes > v->input_bytes) queue_fault(3, g);
            memcpy(t.data, v->input + offset, t.bytes);
            if (!ss_npu_queue_dirty(q, h[g], SS_NPU_INPUT, i, 0, t.bytes)) queue_fault(4, g);
            offset += t.bytes;
        }
        if (offset != v->input_bytes) queue_fault(5, g);
    }
    static struct ss_npu_fence fences[32];
    clock_words(&receipt.reserved[0], &receipt.reserved[1], &receipt.reserved[2]);
    receipt.start = ss_counter_ticks();
    uint64_t total = 0, longest = 0, shortest = UINT64_MAX, refused = 0;
    for (unsigned round = 0; round < SEQUENCE_ROUNDS; ++round) {
        const uint64_t t0 = ss_counter_ticks();
        for (unsigned g = 0; g < n; ++g) {
            int r1;
            while ((r1 = ss_npu_queue_submit(q, h[g], (struct ss_npu_fence){0}, QUEUE_TIMEOUT_US, &fences[g])) == 0) { ++refused; ss_progress(); }
            if (r1 < 0) queue_fault(6, g);
        }
        unsigned spins = 0;
        int done;
        while ((done = ss_npu_queue_status(q, fences[n - 1])) != 1) {
            if (done < 0) queue_fault(8, round);
            if ((++spins & 0x3ff) == 0) { ss_progress(); if (ss_live() < 0) queue_fault(9, round); }
        }
        const uint64_t dt = ss_counter_ticks() - t0;
        total += dt; if (dt > longest) longest = dt; if (dt < shortest) shortest = dt;
        for (unsigned g = 0; g < n; ++g) if (ss_npu_queue_release(q, fences[g]) != 1) queue_fault(7, g);
    }
    clock_words(&receipt.reserved[3], &receipt.reserved[4], &receipt.reserved[5]);
    uint32_t wrong[32] = {0};
    uint64_t mask = 0, wrong_total = 0;
    for (unsigned g = 0; g < n; ++g) {
        const struct npu_graph_vector *v = &npu_graph_vectors[g];
        begin = ss_counter_ticks();
        int acquired;
        while ((acquired = ss_npu_queue_acquire_handle(q, h[g])) != 1) {
            if (acquired < 0 || (ss_counter_ticks() - begin) / 24 > 2000000) queue_fault(10, g);
            ss_progress(); vTaskDelay(1);
        }
        size_t offset = 0;
        for (unsigned i = 0;; ++i) {
            struct ss_npu_tensor t;
            if (!ss_npu_queue_tensor(q, h[g], SS_NPU_OUTPUT, i, &t)) break;
            for (size_t b = 0; b < t.bytes && offset + b < v->reference_bytes; ++b) wrong[g] += ((const uint8_t *)t.data)[b] != v->reference[offset + b];
            offset += t.bytes;
        }
        if (offset != v->reference_bytes) wrong[g] += 1u << 20;
        if (!wrong[g]) mask |= UINT64_C(1) << g;
        wrong_total += wrong[g];
    }
    begin = ss_counter_ticks();
    int closed;
    while ((closed = ss_npu_queue_close(q)) != 1) {
        if (closed < 0 || (ss_counter_ticks() - begin) / 24 > 2000000) queue_fault(11, (uint32_t)closed);
        ss_progress(); vTaskDelay(1);
    }
    struct npu_graph_record *r0 = &receipt.graph[0], *r1 = &receipt.graph[1];
    const int ok = mask == ((UINT64_C(1) << n) - 1);
    *r0 = (struct npu_graph_record){0, n, SEQUENCE_ROUNDS, ok ? NPU_GRAPH_MATCH : NPU_GRAPH_MISMATCH, total, refused, longest, shortest};
    *r1 = (struct npu_graph_record){1, mask, n, ok ? NPU_GRAPH_MATCH : NPU_GRAPH_MISMATCH, wrong_total, 0, 0, total / SEQUENCE_ROUNDS};
    for (unsigned g = 0; g < n && g < 16; ++g) receipt.differing_bytes[g] = wrong[g];
    for (unsigned g = 16; g < n; ++g) receipt.max_error[g - 16] = wrong[g];
    receipt.count = 2; receipt.completed = 2; receipt.matched = ok ? 2 : 0; receipt.sdk_closed = 1;
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
#ifdef NPU_GRAPHS_SEQUENCE
    npu_graphs_sequence_main();
#else
    npu_graphs_queue_main();
#endif
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
