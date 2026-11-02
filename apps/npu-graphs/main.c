/* Runs every graph of the OS NPU catalogue once through the NPU service and compares the output with the simulator's. */
#include "sunstone_app.h"
#include "FreeRTOS.h"
#include "task.h"
#include "receipt.h"
#include "vectors.h"
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

void npu_graphs_main(const struct ss_grant *grant)
{
    uint64_t begin = ss_counter_ticks();
    while (!ss_clock_ready() || ss_throttled()) {
        if (ss_live() < 0) fault(STAGE_CLOCK, 0, 0);
        if (ss_counter_ticks() - begin > 240000000) fault(STAGE_CLOCK, 0, 1);
        vTaskDelay(1);
    }
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
