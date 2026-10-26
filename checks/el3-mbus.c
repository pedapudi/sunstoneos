/* Host model of include/a733_el3_mbus.h: the MBUS restore at each EL3 entry.
 *
 * Models MBUS_CLK_REG (update bit 27 clears after a chosen number of reads,
 * or never; an optional fault drops the source write) and MSI_LITE0_BGR, and
 * a 24 MHz counter that advances on every read. Prints one JSON line per case
 * with the writes, the return value and the tick spans that the runner
 * checks (checks/el3-mbus.py). Exit status 0 when every case passes. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define A733_EL3_MBUS_ACCESS 1
static uint32_t mbus, msi;
static unsigned clear_after, reads_since_update, never_clear_step, ignore_source;
static unsigned update_writes;
static uint64_t ticks, tick_step;
static struct { uint32_t address, value; uint64_t at; } trace[64];
static unsigned writes;

static uint32_t a733_el3_mbus_read(uintptr_t address)
{
    ticks += tick_step;
    if (address == 0x02002588u) {
        if ((mbus & (1u << 27)) && never_clear_step != update_writes && ++reads_since_update >= clear_after)
            mbus &= ~(1u << 27);
        return mbus;
    }
    assert(address == 0x02002594u);
    return msi;
}
static void a733_el3_mbus_write(uintptr_t address, uint32_t value)
{
    assert(writes < 64);
    trace[writes].address = (uint32_t)address; trace[writes].value = value; trace[writes].at = ticks; ++writes;
    if (address == 0x02002588u) {
        if (ignore_source && (value & 0x07000000u) == 0x01000000u) value = (value & ~0x07000000u) | (mbus & 0x07000000u);
        mbus = value; reads_since_update = 0;
        if (value & (1u << 27)) ++update_writes;
        return;
    }
    assert(address == 0x02002594u);
    msi = value;
}
static uint64_t a733_el3_mbus_ticks(void) { return ticks += tick_step; }
#include "a733_el3_mbus.h"

static int failures;
static void run(const char *name, uint32_t m, uint32_t s, unsigned after, unsigned stuck, unsigned drop,
                unsigned expected, uint32_t final_mbus)
{
    mbus = m; msi = s; clear_after = after; never_clear_step = stuck; ignore_source = drop;
    reads_since_update = 0; update_writes = 0; ticks = 1000; tick_step = 1; writes = 0;
    unsigned result = a733_el3_mbus_restore();
    int ok = result == expected && (mbus & ~(1u << 27)) == final_mbus && msi == s;
    printf("{\"case\":\"%s\",\"result\":%u,\"mbus\":\"0x%08x\",\"msi\":\"0x%08x\",\"ticks\":%llu,\"writes\":[",
           name, result, mbus, msi, (unsigned long long)(ticks - 1000));
    for (unsigned i = 0; i < writes; ++i)
        printf("%s[\"0x%08x\",\"0x%08x\",%llu]", i ? "," : "", trace[i].address, trace[i].value,
               (unsigned long long)(trace[i].at - 1000));
    printf("],\"pass\":%s}\n", ok ? "true" : "false");
    if (!ok) ++failures;
}

int main(void)
{
    /* The ROM's state, with the update bit clearing on the first, the tenth
     * and the thousandth read. */
    run("rom-state", 0x84000000u, 0x00030001u, 1, 0, 0, 0, 0x81000000u);
    run("rom-state-late-update", 0x84000000u, 0x00030001u, 10, 0, 0, 0, 0x81000000u);
    run("rom-state-slow-update", 0x84000000u, 0x00030001u, 1000, 0, 0, 0, 0x81000000u);
    /* Nothing to do, or a state the restore does not own. */
    run("boot0-state", 0x81000000u, 0x00030001u, 1, 0, 0, 0, 0x81000000u);
    run("reset-default", 0x80000000u, 0x00030001u, 1, 0, 0, 0, 0x80000000u);
    run("other-divider", 0x84000001u, 0x00030001u, 1, 0, 0, 0, 0x84000001u);
    run("dfs-enabled", 0x94000000u, 0x00030001u, 1, 0, 0, 0, 0x94000000u);
    run("update-pending", 0x8c000000u, 0x00030001u, 1, 0, 0, 0, 0x84000000u);
    run("gate-off", 0x04000000u, 0x00030001u, 1, 0, 0, 0, 0x04000000u);
    run("bridge-reset", 0x84000000u, 0x00010001u, 1, 0, 0, 0, 0x84000000u);
    /* An update bit that never clears at each of the four waits. The source
     * and gate stay where the hardware left them; the gate is turned on. */
    run("stuck-gate-off", 0x84000000u, 0x00030001u, 1, 1, 0, 1, 0x84000000u);
    run("stuck-divider", 0x84000000u, 0x00030001u, 1, 2, 0, 3, 0x84000000u);
    run("stuck-source", 0x84000000u, 0x00030001u, 1, 3, 0, 4, 0x81000000u);
    run("stuck-gate-on", 0x84000000u, 0x00030001u, 1, 4, 0, 6, 0x81000000u);
    /* The source write does not take effect: the read-back refuses it. */
    run("source-ignored", 0x84000000u, 0x00030001u, 1, 0, 1, 7, 0x84000000u);
    return failures != 0;
}
