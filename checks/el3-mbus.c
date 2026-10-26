/* Host model of a733_el3_mbus_restore() in include/a733_el3_mbus.h: its
 * admission, the bus-master checks and the read-back around the SRAM window.
 *
 * The window itself (boot/a733/mbus-window.S) runs from SRAM on the board;
 * checks/el3-mbus.py executes its machine code under Unicorn. Here a stub
 * stands in for it: it records the call and applies boot0's setting, an
 * ignored source write or a timed-out step. Each case prints one JSON line
 * with the result, the window calls and the registers read; the runner
 * checks them. Exit status 0 when every case passes. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define A733_EL3_MBUS_ACCESS 1
static uint32_t mbus, msi, gpu_clk, gpu_bus, npu_clk, npu_bus, dma_bgr, dma_sta, ce_bgr, ce_tlr_ns, ce_tlr_s;
static unsigned window_calls, window_result, window_ignores_source, reads;
static uint32_t window_msi;

static uint32_t a733_el3_mbus_read(uintptr_t address)
{
    ++reads;
    switch (address) {
    case 0x02002588u: return mbus;
    case 0x02002594u: return msi;
    case 0x02002b20u: return gpu_clk;
    case 0x02002b24u: return gpu_bus;
    case 0x02002b00u: return npu_clk;
    case 0x02002b04u: return npu_bus;
    case 0x02002704u: return dma_bgr;
    case 0x04601030u: assert((dma_bgr & 0x10001u) == 0x10001u); return dma_sta;
    case 0x02002ac4u: return ce_bgr;
    case 0x04603010u: assert((ce_bgr & 0x10001u) == 0x10001u); return ce_tlr_ns;
    case 0x04603810u: assert((ce_bgr & 0x10001u) == 0x10001u); return ce_tlr_s;
    }
    assert(0 && "unmodeled read");
    return 0;
}
static unsigned a733_el3_mbus_window(uint32_t m)
{
    ++window_calls; window_msi = m;
    if (window_result) { mbus = (mbus & ~0x08000000u) | 0x80000000u; return window_result; }
    mbus = window_ignores_source ? 0x84000000u : 0x81000000u;
    return 0;
}
#include "a733_el3_mbus.h"

static int failures;
static void run(const char *name, uint32_t m, unsigned expected, unsigned calls, uint32_t final)
{
    mbus = m; window_calls = 0; window_msi = 0; reads = 0;
    unsigned result = a733_el3_mbus_restore();
    int ok = result == expected && window_calls == calls && mbus == final && (!calls || window_msi == msi);
    printf("{\"case\":\"%s\",\"result\":%u,\"window_calls\":%u,\"mbus\":\"0x%08x\",\"reads\":%u,\"pass\":%s}\n",
           name, result, window_calls, mbus, reads, ok ? "true" : "false");
    if (!ok) ++failures;
}
static void idle(void)
{
    msi = 0x00030001u; gpu_clk = gpu_bus = npu_clk = 0; npu_bus = 0x00080000u;
    dma_bgr = dma_sta = 0; ce_bgr = 0x00030003u; ce_tlr_ns = ce_tlr_s = 0;
    window_result = 0; window_ignores_source = 0;
}

int main(void)
{
    idle(); run("rom-state", 0x84000000u, 0, 1, 0x81000000u);
    idle(); dma_bgr = 0x10001u; run("rom-state-dma-open-idle", 0x84000000u, 0, 1, 0x81000000u);
    /* Nothing to do, or a state the restore does not own: no window. */
    idle(); run("boot0-state", 0x81000000u, 0, 0, 0x81000000u);
    idle(); run("reset-default", 0x80000000u, 0, 0, 0x80000000u);
    idle(); run("other-divider", 0x84000001u, 0, 0, 0x84000001u);
    idle(); run("dfs-enabled", 0x94000000u, 0, 0, 0x94000000u);
    idle(); run("update-pending", 0x8c000000u, 0, 0, 0x8c000000u);
    idle(); run("gate-off", 0x04000000u, 0, 0, 0x04000000u);
    idle(); msi = 0x00010001u; run("bridge-reset", 0x84000000u, 0, 0, 0x84000000u);
    /* A bus master that could still be running: refused, no window. */
    idle(); gpu_clk = 0x80000000u; run("gpu-clocked", 0x84000000u, 8, 0, 0x84000000u);
    idle(); gpu_bus = 1; run("gpu-bus-open", 0x84000000u, 8, 0, 0x84000000u);
    idle(); npu_clk = 0x80000000u; run("npu-clocked", 0x84000000u, 8, 0, 0x84000000u);
    idle(); npu_bus = 0x00080001u; run("npu-bus-open", 0x84000000u, 8, 0, 0x84000000u);
    idle(); dma_bgr = 0x10001u; dma_sta = 1; run("dma-channel-busy", 0x84000000u, 9, 0, 0x84000000u);
    idle(); dma_bgr = 0x10001u; dma_sta = 0x80000000u; run("dma-fifo-not-empty", 0x84000000u, 9, 0, 0x84000000u);
    idle(); dma_bgr = 0x1u; dma_sta = 1; run("dma-in-reset", 0x84000000u, 0, 1, 0x81000000u);
    idle(); ce_tlr_ns = 0x400; run("ce-hash-loaded", 0x84000000u, 10, 0, 0x84000000u);
    idle(); ce_tlr_s = 1; run("ce-secure-loaded", 0x84000000u, 10, 0, 0x84000000u);
    idle(); ce_bgr = 0; ce_tlr_ns = 0x400; run("ce-in-reset", 0x84000000u, 0, 1, 0x81000000u);
    /* The window's own failures and the read-back. */
    idle(); window_result = 4; run("window-step-4", 0x84000000u, 4, 1, 0x84000000u | 0x80000000u);
    idle(); window_ignores_source = 1; run("source-ignored", 0x84000000u, 7, 1, 0x84000000u);
    /* Codes 1 to 7 stop the image; 8 to 10 skip the restore and continue. */
    for (unsigned code = 0; code <= 10; ++code)
        if (A733_MBUS_FAILED(code) != (code >= 1 && code <= 7)) ++failures;
    printf("{\"case\":\"failure-codes\",\"result\":0,\"window_calls\":0,\"mbus\":\"0x00000000\",\"reads\":0,\"pass\":%s}\n",
           failures ? "false" : "true");
    return failures != 0;
}
