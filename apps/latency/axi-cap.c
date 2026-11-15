/* SPDX-License-Identifier: GPL-2.0-only */
/* The bridge accesses of the stage 3a AXI2HIF cap, at EL1 on CPU0 (axi-cap.h). Every write follows the vendor order
 * (sunxi-nsi.c lines 401-488) and is preceded by a read of the three gates that the runner and the facts probe check:
 * the DRAM0 clock (0x02002c00 bit 31), its bus gate and reset (0x02002c0c bits 0 and 16) and the CPU path select
 * (0x08000200 bit 0). A closed gate stops the operation before any bridge access, because an access behind a closed
 * gate can hang the interconnect. Only +0x30, +0x48 and +0x4c are written in normal operation; the entry check also
 * zeroes +0x50 to +0x5c if a run finds them set. */
#include "axi-cap.h"
#include <stdatomic.h>
#include <stddef.h>

#if defined(SUNSTONE_LATENCY_MODEL)
/* The QEMU latency models have no bridges: the words live in memory, the gates read open, and the sub-phase
 * control around them runs as on the board. */
#include "FreeRTOS.h"
#include "task.h"
#include "sunstone_app.h"
#include "sunstone_latency.h"
static uint32_t model_words[2][0x60 / 4];
static uint32_t model_read(uintptr_t address)
{
    if (address == UINT32_C(0x02002c00)) return UINT32_C(0x80000000);
    if (address == UINT32_C(0x02002c0c)) return UINT32_C(0x00010001);
    if (address == UINT32_C(0x08000200)) return 1u;
    return model_words[(address >> 16) & 1u ? 0 : 1][(address & 0xffffu) / 4];
}
#define AXI_READ(address) model_read(address)
#define AXI_WRITE(address, value) (model_words[((address) >> 16) & 1u ? 0 : 1][((address) & 0xffffu) / 4] = (value))
#define AXI_NOW() ss_latency_now()
#elif !defined(LATENCY_AXI_HOST_TEST)
#include "FreeRTOS.h"
#include "task.h"
#include "sunstone_app.h"
#include "sunstone_latency.h"
#include "a733_mmio.h"
#define AXI_READ(address) a733_mmio_read32_dsb_sy_both(address)
#define AXI_WRITE(address, value) a733_mmio_write32_dsb_sy_both((address), (value))
#define AXI_NOW() ss_latency_now()
#else
uint32_t axi_host_read(uintptr_t address);
void axi_host_write(uintptr_t address, uint32_t value);
uint64_t axi_host_now(void);
#define AXI_READ(address) axi_host_read(address)
#define AXI_WRITE(address, value) axi_host_write((address), (value))
#define AXI_NOW() axi_host_now()
#endif

#define GATE_DRAM0_CLOCK UINT32_C(0x02002c00)
#define GATE_DRAM0_BUS UINT32_C(0x02002c0c)
#define GATE_PATH_SELECT UINT32_C(0x08000200)
#define IAG_MODE 0x30u
#define LIMIT_ENABLE (UINT32_C(1) << 0)
#define LIMIT_TOTAL (UINT32_C(1) << 16)

static const uintptr_t bridges[LATENCY_AXI_BRIDGES] = {UINT32_C(0x0a030000), UINT32_C(0x0a040000)};
static const uint16_t words[LATENCY_AXI_WORDS] = {0x30, 0x48, 0x4c, 0x50, 0x54, 0x58, 0x5c};

struct latency_axi_report latency_axi;
static struct latency_axi_switch *current;

static int gates_open(void)
{
    current->gates[0] = AXI_READ(GATE_DRAM0_CLOCK);
    current->gates[1] = AXI_READ(GATE_DRAM0_BUS);
    current->gates[2] = AXI_READ(GATE_PATH_SELECT);
    return (current->gates[0] >> 31 & 1u) && (current->gates[1] & UINT32_C(0x10001)) == UINT32_C(0x10001) &&
           (current->gates[2] & 1u);
}
static uint32_t bridge_read(unsigned bridge, unsigned offset)
{
    const uint64_t start = AXI_NOW();
    const uint32_t value = AXI_READ(bridges[bridge] + offset);
    current->access_ticks += AXI_NOW() - start;
    ++current->accesses;
    return value;
}
/* Returns 0 without writing when a gate reads closed. */
static int bridge_write(unsigned bridge, unsigned offset, uint32_t value)
{
    if (!gates_open()) return 0;
    const uint64_t start = AXI_NOW();
    AXI_WRITE(bridges[bridge] + offset, value);
    current->access_ticks += AXI_NOW() - start;
    ++current->accesses;
    return 1;
}
static void read_all(uint32_t out[LATENCY_AXI_BRIDGES][LATENCY_AXI_WORDS])
{
    for (unsigned b = 0; b < LATENCY_AXI_BRIDGES; ++b)
        for (unsigned w = 0; w < LATENCY_AXI_WORDS; ++w) out[b][w] = bridge_read(b, words[w]);
}
/* The vendor's enable of limit 0 on one bridge: read +0x30, write it back with the limit bit clear, write the
 * bandwidth and saturation words, then write the first value with the limit bit and the limiter bit set. */
static uint32_t set_limit(uint32_t bw, uint32_t sat)
{
    for (unsigned b = 0; b < LATENCY_AXI_BRIDGES; ++b) {
        if (!gates_open()) return LATENCY_AXI_GATE_CLOSED;
        const uint32_t mode = bridge_read(b, IAG_MODE);
        if (!bridge_write(b, IAG_MODE, mode & ~LIMIT_TOTAL) || !bridge_write(b, 0x48, bw & 0xfffu) ||
            !bridge_write(b, 0x4c, sat & 0x3ffu) || !bridge_write(b, IAG_MODE, mode | LIMIT_TOTAL | LIMIT_ENABLE))
            return LATENCY_AXI_GATE_CLOSED;
    }
    read_all(current->readback);
    for (unsigned b = 0; b < LATENCY_AXI_BRIDGES; ++b)
        if ((current->readback[b][0] & (LIMIT_TOTAL | LIMIT_ENABLE)) != (LIMIT_TOTAL | LIMIT_ENABLE) ||
            (current->readback[b][1] & 0xfffu) != (bw & 0xfffu) || (current->readback[b][2] & 0x3ffu) != (sat & 0x3ffu))
            return LATENCY_AXI_READBACK;
    return LATENCY_AXI_OK;
}
/* The vendor's disable: clear the limit and limiter bits of +0x30 and leave the limit words. */
static uint32_t clear_limit(void)
{
    for (unsigned b = 0; b < LATENCY_AXI_BRIDGES; ++b) {
        if (!gates_open()) return LATENCY_AXI_GATE_CLOSED;
        const uint32_t mode = bridge_read(b, IAG_MODE);
        if (!bridge_write(b, IAG_MODE, mode & ~(LIMIT_TOTAL | LIMIT_ENABLE))) return LATENCY_AXI_GATE_CLOSED;
    }
    read_all(current->readback);
    for (unsigned b = 0; b < LATENCY_AXI_BRIDGES; ++b)
        if (current->readback[b][0] & (LIMIT_TOTAL | LIMIT_ENABLE)) return LATENCY_AXI_READBACK;
    return LATENCY_AXI_OK;
}
/* The reset values the facts probe read at EL1 (entry 1794744873056408964-85d3ad7d): every word here is 0. */
static uint32_t restore(void)
{
    for (unsigned b = 0; b < LATENCY_AXI_BRIDGES; ++b)
        if (!bridge_write(b, IAG_MODE, 0) || !bridge_write(b, 0x48, 0) || !bridge_write(b, 0x4c, 0))
            return LATENCY_AXI_GATE_CLOSED;
    read_all(current->readback);
    for (unsigned b = 0; b < LATENCY_AXI_BRIDGES; ++b)
        for (unsigned w = 0; w < LATENCY_AXI_WORDS; ++w)
            if (current->readback[b][w]) return LATENCY_AXI_READBACK;
    return LATENCY_AXI_OK;
}
/* Every run, baselines included, reads the seven words first. A word set at entry means a cap survived the reset:
 * zero it, record what was found and report it, and the run stops. */
static uint32_t entry_check(void)
{
    if (!gates_open()) return LATENCY_AXI_GATE_CLOSED;
    read_all(latency_axi.entry);
    uint32_t found = 0;
    for (unsigned b = 0; b < LATENCY_AXI_BRIDGES; ++b)
        for (unsigned w = 0; w < LATENCY_AXI_WORDS; ++w) found |= latency_axi.entry[b][w];
    if (!found) return LATENCY_AXI_OK;
    for (unsigned b = 0; b < LATENCY_AXI_BRIDGES; ++b)
        for (unsigned w = 0; w < LATENCY_AXI_WORDS; ++w)
            if (latency_axi.entry[b][w] && !bridge_write(b, words[w], 0)) return LATENCY_AXI_GATE_CLOSED;
    read_all(current->readback);
    return LATENCY_AXI_ENTRY_SET;
}
uint32_t latency_axi_run(enum latency_axi_op op)
{
    if (latency_axi.switch_count >= LATENCY_AXI_SWITCHES) return LATENCY_AXI_BAD_OP;
    current = &latency_axi.switches[latency_axi.switch_count++];
    current->op = op;
    current->begin = AXI_NOW();
    uint32_t status;
    switch (op) {
    case LATENCY_AXI_ENTRY: status = entry_check(); break;
    case LATENCY_AXI_SET_NONBINDING: status = set_limit(LATENCY_AXI_BW_NONBINDING, LATENCY_AXI_SAT_NONBINDING); break;
    case LATENCY_AXI_SET_B: status = set_limit(LATENCY_AXI_BW_B, LATENCY_AXI_SAT_B); break;
    case LATENCY_AXI_CLEAR: status = clear_limit(); break;
    case LATENCY_AXI_RESTORE: status = restore(); break;
    default: status = LATENCY_AXI_BAD_OP; break;
    }
    current->end = AXI_NOW();
    current->status = status;
    return status;
}

#ifndef LATENCY_AXI_HOST_TEST
static StaticTask_t worker_state;
static StackType_t worker_stack[1024];
static TaskHandle_t worker, requester;
static _Atomic unsigned posted, answered;
static _Atomic uint32_t answer;

void latency_axi_worker(void *argument)
{
    (void)argument;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const unsigned request = atomic_load_explicit(&posted, memory_order_acquire);
        if (request == atomic_load_explicit(&answered, memory_order_relaxed)) continue;
        atomic_store_explicit(&answer, latency_axi_run((enum latency_axi_op)(request & 0xffu)), memory_order_relaxed);
        atomic_store_explicit(&answered, request, memory_order_release);
        xTaskNotifyGive(requester);
    }
}
void latency_axi_init(void)
{
    latency_axi = (struct latency_axi_report){
        .magic = LATENCY_AXI_MAGIC, .version = LATENCY_AXI_VERSION, .bytes = sizeof(struct latency_axi_report),
        .bw_b = LATENCY_AXI_BW_B, .sat_b = LATENCY_AXI_SAT_B, .bw_nonbinding = LATENCY_AXI_BW_NONBINDING,
        .sat_nonbinding = LATENCY_AXI_SAT_NONBINDING, .subphase_wakes = LATENCY_AXI_SUBPHASE_WAKES,
        .capped_mask = LATENCY_AXI_CAPPED_MASK};
    /* Above the idle memory task, below the wake probe that shares CPU0. */
    worker = ss_task_create_static_affinity_set(latency_axi_worker, "latency-axi-cap", 1024, NULL, 2,
                                                worker_stack, &worker_state, 1U << 0);
    configASSERT(worker);
}
int latency_axi_request(enum latency_axi_op op)
{
    requester = xTaskGetCurrentTaskHandle();
    /* A sequence number above the operation keeps a late answer from passing for the next request. */
    const unsigned request = ((atomic_load_explicit(&posted, memory_order_relaxed) >> 8) + 1u) << 8 | (unsigned)op;
    (void)ulTaskNotifyTake(pdTRUE, 0);
    atomic_store_explicit(&posted, request, memory_order_release);
    xTaskNotifyGive(worker);
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(LATENCY_AXI_OP_MS);
    while (atomic_load_explicit(&answered, memory_order_acquire) != request) {
        const TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) return -1;
        (void)ulTaskNotifyTake(pdTRUE, deadline - now);
    }
    return (int)atomic_load_explicit(&answer, memory_order_relaxed);
}
#endif
