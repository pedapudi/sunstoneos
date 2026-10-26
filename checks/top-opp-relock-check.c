/* SPDX-License-Identifier: Apache-2.0 */
/* Host model of boot/a733/top-opp-relock.c, the CPU0 relock of the
 * workload's top-operating-point profile. The CPU PLL, mux and pattern words
 * are modeled with port 3-3's recorded boot values; time runs in
 * nanoseconds and the counter returns 24 MHz ticks. The model requires the
 * CPU clock driver's sequence: a PLL write only while its mux is parked and
 * at least 20 us after the park; the PLL selected again only after three
 * lock reads at least 3 us apart and 20 us after the third; P written before
 * the select; and the domains in the order CPU-B, CPU-L, DSU. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t ns;
static uint32_t regs[4][8]; /* Per domain index 1..3: PLL, PAT0, PAT1, LFM, mux. */
static unsigned stuck_update, stuck_lock, flicker, writes, order[8], orders;
static uint64_t park_ns[4], third_ns[4], last_lock_ns[4];
static unsigned run[4], update_reads[4], lock_reads_after_update[4];

static unsigned domain_of(uintptr_t a) { return (unsigned)((a - 0x08870000u) >> 12); }
static uint32_t *slot(uintptr_t a)
{
    const unsigned d = domain_of(a), off = a & 0xfff;
    assert(d >= 1 && d <= 3);
    switch (off) {
    case 0: return &regs[d][0];
    case 4: return &regs[d][1];
    case 8: return &regs[d][2];
    case 0x18: return &regs[d][3];
    case 0x1c: return &regs[d][4];
    }
    assert(0);
    return 0;
}
static uint64_t model_now(void) { ns += 50; return ns * 24 / 1000; }
static uint32_t model_read(uintptr_t a)
{
    ns += 100;
    uint32_t *r = slot(a);
    const unsigned d = domain_of(a);
    if ((a & 0xfff) == 0 && ((regs[d][4] >> 24) & 7) == 0) {
        if ((*r & (1u << 26)) && !stuck_update && ++update_reads[d] >= 2) *r &= ~(1u << 26);
        else if (!(*r & (1u << 26)) && !(*r & (1u << 28)) && !stuck_lock && ++lock_reads_after_update[d] >= 2)
            *r |= 1u << 28;
        uint32_t v = *r;
        if ((v & (1u << 28)) && flicker && run[d]) { --flicker; v &= ~(1u << 28); }
        if (!(v & (1u << 26))) {
            if (!(v & (1u << 28))) run[d] = 0;
            else if (!run[d] || ns >= last_lock_ns[d] + 3000) {
                last_lock_ns[d] = ns;
                if (++run[d] == 3) third_ns[d] = ns;
            }
        }
        return v;
    }
    return *r;
}
static void model_write(uintptr_t a, uint32_t v)
{
    ns += 100;
    ++writes;
    uint32_t *r = slot(a);
    const unsigned d = domain_of(a);
    if ((a & 0xfff) == 0) {
        assert(((regs[d][4] >> 24) & 7) == 0 && ns >= park_ns[d] + 20000);
        *r = v & ~(1u << 28);
        update_reads[d] = lock_reads_after_update[d] = run[d] = 0;
        return;
    }
    assert((a & 0xfff) == 0x1c);
    const unsigned was = (*r >> 24) & 7, now = (v >> 24) & 7;
    if (!now && was) {
        park_ns[d] = ns;
        if (!orders || order[orders - 1] != d) order[orders++] = d;
    }
    if (now == 3 && !was) {
        assert(run[d] >= 3 && ns >= third_ns[d] + 20000);
        assert(!((v >> 16) & 3) && !((*r >> 16) & 3)); /* P = 0 written before the select. */
    }
    *r = v;
}
#define TOP_OPP_READ model_read
#define TOP_OPP_WRITE model_write
#define TOP_OPP_NOW() model_now()
#include "../boot/a733/top-opp-relock.c"

static void board(void)
{
    memset(regs, 0, sizeof regs);
    ns = 1000000;writes = orders = stuck_update = stuck_lock = flicker = 0;
    memset(run, 0, sizeof run);
    regs[1][0] = 0xf8802600;regs[1][4] = 0x03010000;
    regs[2][0] = 0xf8802700;regs[2][4] = 0x03000000;
    regs[3][0] = 0xf8802600;regs[3][4] = 0x03010105;
    for (unsigned d = 1; d <= 3; ++d) {regs[d][2] = 0x30000000;regs[d][3] = 0x40000001;}
}
static uint64_t record[16];
static uint64_t relock(uint64_t deadline_ticks)
{
    memset(record, 0, sizeof record);
    return top_opp_relock(record, deadline_ticks);
}
int main(void)
{
    const uint64_t far = UINT64_MAX / 2;
    /* Success on port 3-3's boot words, then with the lock reading clear
     * twice after it first sets. */
    for (unsigned f = 0; f < 2; ++f) {
        board();flicker = f ? 2 : 0;
        assert(relock(far) == TOP_OPP_DONE && record[TOP_OPP_MAGIC_WORD] == TOP_OPP_MAGIC);
        assert(regs[2][0] == 0xf8804d00 && regs[2][4] == 0x03000000);
        assert(regs[1][0] == 0xf8804500 && regs[1][4] == 0x03000000);
        assert(regs[3][0] == 0xf8803100 && regs[3][4] == 0x03000105);
        assert(orders == 3 && order[0] == 2 && order[1] == 1 && order[2] == 3 && !flicker);
        assert(record[TOP_OPP_FINAL_WORD] == ((uint64_t)0xf8804d00 << 32 | 0x03000000));
        assert(record[TOP_OPP_ENTRY_WORD + 1] == ((uint64_t)0xf8802600 << 32 | 0x03010000));
        for (unsigned i = 0; i < 3; ++i) {
            const uint64_t parked = record[TOP_OPP_PARKED_WORD + i] >> 32;
            assert(parked >= 2 * 480 + 2 * 72 && parked < 24 * 1000 && (record[TOP_OPP_PARKED_WORD + i] & 0xffffffff) >= 3);
        }
        printf("BUDGET relock flicker=%u writes=%u parked_ticks=%llu,%llu,%llu\n", f, writes,
               (unsigned long long)(record[10] >> 32), (unsigned long long)(record[11] >> 32),
               (unsigned long long)(record[12] >> 32));
        assert(writes == 15);
    }
    /* Refusals before any write. */
    board();regs[3][1] = 1u << 29;
    assert(relock(far) == TOP_OPP_REFUSED_MODULATION && !writes);
    board();regs[3][3] = 0xc0000001;
    assert(relock(far) == TOP_OPP_REFUSED_MODULATION && !writes);
    board();regs[1][4] = 0x00010000;
    assert(relock(far) == TOP_OPP_REFUSED_STATE && !writes);
    board();regs[2][0] = 0xe8802700;
    assert(relock(far) == TOP_OPP_REFUSED_STATE && !writes);
    board();regs[2][0] |= 1u << 26;
    assert(relock(far) == TOP_OPP_REFUSED_STATE && !writes);
    board();regs[1][0] = 0xf8801400;
    assert(relock(far) == TOP_OPP_REFUSED_RANGE && !writes);
    board();
    assert(relock(1) == TOP_OPP_REFUSED_DEADLINE && !writes);
    /* Faults after the first write: CPU-B stays parked, the other domains
     * untouched. */
    board();stuck_update = 1;
    assert(relock(far) == (TOP_OPP_FAULT | 0 << 4 | 2) && ((regs[2][4] >> 24) & 7) == 0);
    assert(regs[1][0] == 0xf8802600 && regs[3][4] == 0x03010105);
    board();stuck_lock = 1;
    assert(relock(far) == (TOP_OPP_FAULT | 0 << 4 | 3) && ((regs[2][4] >> 24) & 7) == 0);
    /* A deadline reached during CPU-L's lock wait ends there. */
    board();
    {
        uint64_t probe[16] = {0};
        uint64_t save = ns;
        (void)save;
        assert(top_opp_relock(probe, ns * 24 / 1000 + 3000) != TOP_OPP_DONE);
        assert(probe[TOP_OPP_RESULT_WORD] & TOP_OPP_FAULT);
    }
    puts("PASS top-OPP relock: port 3-3 boot words to CPU-B 2002, CPU-L 1794 and DSU 1274 MHz in that order, "
         "parked 20 us before N, three lock reads 3 us apart and 20 us before the select, P before the select, "
         "a flickering lock, five refusals with no write, update and lock timeouts, an expiring deadline");
    return 0;
}
