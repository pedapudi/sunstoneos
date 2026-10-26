/* SPDX-License-Identifier: Apache-2.0 */
/* Host model of boot/a733/top-opp-relock.c, the CPU0 relock of the
 * workload's top-operating-point profile. The CPU PLL, mux and pattern words
 * are modeled with port 3-3's recorded boot values; time runs in
 * nanoseconds and the counter returns 24 MHz ticks. The model requires the
 * CPU clock driver's sequence: a PLL write only while its mux is parked and
 * at least 20 us after the park; the PLL selected again only after three
 * lock reads at least 3 us apart and 20 us after the third; P written before
 * the select; and the domains in the order CPU-B, CPU-L, DSU.
 *
 * The entry words outside the PLLs are modeled too: the oscillator, the SID
 * word, the S_TWI0 baseline and route, and the AXP8191 behind S_TWI0, which
 * answers each bus event at the second control read after it starts. No
 * clock word may be written before all seven PMIC reads have completed. */
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

/* Board words outside the PLLs (port 3-3, bundles 14 and 15) and the PMIC. */
#define TWI 0x07083000u
static const uintptr_t board_address[] = {0x07090160, 0x0300624c, 0x0701019c, 0x07025000, 0x0701000c, 0x07010010,
    0x02002000, TWI + 0x08, TWI + 0x0c, TWI + 0x10, TWI + 0x14, TWI + 0x18, TWI + 0x1c, TWI + 0x20, TWI + 0x200,
    TWI + 0x210, TWI + 0x214, TWI + 0x218, TWI + 0x21c, TWI + 0x314, TWI + 0x318, TWI + 0x31c};
#define BOARD_WORDS (sizeof(board_address) / sizeof(*board_address))
static uint32_t board_word[BOARD_WORDS];
static uint8_t pmic[256];
static unsigned bus_stage, bus_pending, bus_countdown, bus_stopping, bus_data, bus_reg, bus_reads, bus_writes;
static unsigned clock_writes, nack_at, bus_silent;
static unsigned board_index(uintptr_t a)
{
    for (unsigned i = 0; i < BOARD_WORDS; ++i)
        if (board_address[i] == a) return i;
    assert(!"unmodeled board word");
    return 0;
}
#define BW(a) board_word[board_index(a)]
enum { IDLE, START, ADDRESS, REGISTER, RESTART, READ, RECEIVED };
static void bus_write(uintptr_t a, uint32_t v)
{
    ++bus_writes;
    assert(!clock_writes);
    if (a == TWI + 0x08) { bus_data = v & 255; return; }
    assert(a == TWI + 0x0c);
    if (v == 0x58) { /* STOP: after a received byte, or to release the bus after a failure. */
        bus_pending = 0;bus_stage = IDLE;bus_stopping = 2;BW(TWI + 0x0c) = 0x50;return;
    }
    assert(!bus_pending && !bus_stopping);
    unsigned status = 0;
    if (v == 0x68) {
        assert(bus_stage == IDLE || bus_stage == REGISTER);
        status = bus_stage == IDLE ? 0x08 : 0x10;bus_stage = bus_stage == IDLE ? START : RESTART;
        BW(TWI + 0x0c) = 0x60;
    } else {
        assert(v == 0x48);
        switch (bus_stage) {
        case START: assert(bus_data == 0x6c);status = 0x18;bus_stage = ADDRESS;break;
        case ADDRESS: bus_reg = bus_data;status = 0x28;bus_stage = REGISTER;break;
        case RESTART: assert(bus_data == 0x6d);status = 0x40;bus_stage = READ;break;
        case READ: BW(TWI + 0x08) = pmic[bus_reg];++bus_reads;status = 0x58;bus_stage = RECEIVED;break;
        default: assert(!"data phase out of order");
        }
        BW(TWI + 0x0c) = 0x40;
    }
    if (nack_at && bus_reads + 1 == nack_at && status == 0x18) status = 0x20; /* Address not acknowledged. */
    bus_pending = status;bus_countdown = 2;
}
static uint32_t bus_read(uintptr_t a)
{
    if (a == TWI + 0x0c && !bus_silent) {
        if (bus_pending && !--bus_countdown) {
            BW(TWI + 0x0c) = (BW(TWI + 0x0c) & ~0x20u) | 8;BW(TWI + 0x10) = bus_pending;bus_pending = 0;
        } else if (bus_stopping && !--bus_stopping) {
            BW(TWI + 0x0c) = 0x40;BW(TWI + 0x10) = 0xf8;
        }
    }
    return BW(a);
}
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
    if ((a >> 16) != 0x0887) return bus_read(a);
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
    if ((a >> 16) != 0x0887) { bus_write(a, v); return; }
    assert(bus_reads == 7 && bus_stage == IDLE && !bus_stopping); /* Both supplies read first. */
    ++clock_writes;
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
    static const uint32_t words[BOARD_WORDS] = {0x983fb0f7, 0x496, 0x10001, 0x22, 0, 0, 0, 0, 0x40, 0xf8, 0x29,
        0, 0, 0x30, 0, 0xc0, 0, 0, 0, 0, 0, 0};
    memcpy(board_word, words, sizeof words);
    memset(pmic, 0, sizeof pmic);
    pmic[0x0e] = 3;pmic[0x10] = 0xff;pmic[0x14] = 0xb7;pmic[0x16] = 0xb2;
    bus_stage = bus_pending = bus_countdown = bus_stopping = bus_data = bus_reg = bus_reads = bus_writes = 0;
    clock_writes = nack_at = bus_silent = 0;
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
        assert(record[TOP_OPP_BOARD_WORD] == ((uint64_t)0x983fb0f7 << 32 | 0x496));
        assert(record[TOP_OPP_PMIC_WORD] == 0x0000b2b7ff000300ull && record[TOP_OPP_BUS_WORD] == ((uint64_t)0x29 << 32 | 7u << 8 | 0xf8));
        assert(bus_reads == 7 && bus_writes == 7 * 10); /* Six events and a STOP: ten writes per read. */
        for (unsigned i = 0; i < 3; ++i) {
            const uint64_t parked = record[TOP_OPP_PARKED_WORD + i] >> 32;
            assert(parked >= 2 * 480 + 2 * 72 && parked < 24 * 1000 && (record[TOP_OPP_PARKED_WORD + i] & 0xffffffff) >= 3);
        }
        printf("BUDGET relock flicker=%u writes=%u parked_ticks=%llu,%llu,%llu\n", f, writes,
               (unsigned long long)(record[10] >> 32), (unsigned long long)(record[11] >> 32),
               (unsigned long long)(record[12] >> 32));
        assert(writes == 15);
        /* The completed path: every secondary OFF, CPU-B back to 1014 MHz in
         * the same sequence, five more clock writes and no PMIC access. */
        uint64_t lower[4] = {0};
        const unsigned before_bus = bus_writes;
        orders = 0;
        assert(top_opp_lower_b(lower, far) == TOP_OPP_DONE && lower[TOP_OPP_LOWER_RESULT_WORD] == TOP_OPP_DONE);
        assert(regs[2][0] == 0xf8802700 && regs[2][4] == 0x03000000 && writes == 20 && bus_writes == before_bus);
        assert(orders == 1 && order[0] == 2 && regs[1][0] == 0xf8804500 && regs[3][0] == 0xf8803100);
        assert(lower[TOP_OPP_LOWER_ENTRY_WORD] == ((uint64_t)0xf8804d00 << 32 | 0x03000000));
        assert(lower[TOP_OPP_LOWER_FINAL_WORD] == ((uint64_t)0xf8802700 << 32 | 0x03000000));
        assert((lower[TOP_OPP_LOWER_PARKED_WORD] & 0xffffffff) >= 3 && (lower[TOP_OPP_LOWER_PARKED_WORD] >> 32) < 24 * 1000);
    }
    /* The lowering refuses, with no write, CPU-B at any word but the relock's,
     * and a deadline already passed. */
    board();
    {
        uint64_t lower[4] = {0};
        assert(top_opp_lower_b(lower, far) == TOP_OPP_REFUSED_STATE && !writes); /* Still at the boot word. */
        assert(relock(far) == TOP_OPP_DONE);
        const unsigned w = writes;
        regs[2][3] = 0xc0000001;
        assert(top_opp_lower_b(lower, far) == TOP_OPP_REFUSED_STATE && writes == w);
        regs[2][3] = 0x40000001;
        assert(top_opp_lower_b(lower, 1) == TOP_OPP_REFUSED_DEADLINE && writes == w);
        stuck_lock = 1;
        assert(top_opp_lower_b(lower, far) == (TOP_OPP_FAULT | 3) && ((regs[2][4] >> 24) & 7) == 0);
    }
    /* Board and supply refusals before any clock write. */
    board();BW(0x07090160) &= ~(3u << 14); /* DCXO at 24 MHz. */
    assert(relock(far) == TOP_OPP_REFUSED_OSCILLATOR && !clock_writes && !bus_writes);
    board();BW(0x07090160) &= ~2u;
    assert(relock(far) == TOP_OPP_REFUSED_OSCILLATOR && !clock_writes);
    board();BW(0x0300624c) = 0x396; /* Code 0x03. */
    assert(relock(far) == TOP_OPP_REFUSED_BIN && !clock_writes);
    board();BW(0x0300624c) = 0x050496; /* dvfs_bak 0x05 overrides dvfs_ori 0x04. */
    assert(relock(far) == TOP_OPP_REFUSED_BIN && !clock_writes);
    board();BW(0x07025000) = 0x77; /* PL0 and PL1 not in the TWI function. */
    assert(relock(far) == TOP_OPP_REFUSED_BUS && !clock_writes && !bus_writes);
    board();BW(0x0701019c) = 0x1; /* S_TWI0 held in reset. */
    assert(relock(far) == TOP_OPP_REFUSED_BUS && !bus_writes);
    board();BW(TWI + 0x0c) = 0xc0; /* Interrupt enable set. */
    assert(relock(far) == TOP_OPP_REFUSED_BUS && !bus_writes);
    board();BW(TWI + 0x14) = 0x01; /* 1.3 MHz bus. */
    assert(relock(far) == TOP_OPP_REFUSED_BUS && !bus_writes);
    board();BW(0x07010010) = 0x04000000;BW(0x02002000) = 0; /* APBS1 on an invalid PLL_REF. */
    assert(relock(far) == TOP_OPP_REFUSED_BUS && !bus_writes);
    board();BW(0x07010010) = 0x04000000;BW(0x02002000) = 0xf8000000u | 103u << 16 | 95u << 8; /* 26 MHz x 96 / 104. */
    assert(relock(far) == TOP_OPP_DONE);
    board();nack_at = 5; /* The fifth read's address is not acknowledged. */
    assert(relock(far) == TOP_OPP_REFUSED_TRANSFER && !clock_writes && ((record[TOP_OPP_BUS_WORD] >> 8) & 255) == 4 &&
           (record[TOP_OPP_BUS_WORD] & 255) == 0x20);
    board();bus_silent = 1; /* No bus event completes. */
    assert(relock(far) == TOP_OPP_REFUSED_TRANSFER && !clock_writes);
    board();pmic[0x14] = 0x9e; /* DCDC3 back at 800 mV. */
    assert(relock(far) == TOP_OPP_REFUSED_SUPPLY && !clock_writes && bus_reads == 7);
    board();pmic[0x16] = 0x9e;
    assert(relock(far) == TOP_OPP_REFUSED_SUPPLY && !clock_writes);
    board();pmic[0x10] = 0xef; /* DCDC5 off. */
    assert(relock(far) == TOP_OPP_REFUSED_SUPPLY && !clock_writes);
    board();pmic[0x0e] = 2; /* Another PMIC. */
    assert(relock(far) == TOP_OPP_REFUSED_SUPPLY && !clock_writes);
    board();pmic[0xff] = 1; /* Another register page. */
    assert(relock(far) == TOP_OPP_REFUSED_SUPPLY && !clock_writes);
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
         "a flickering lock, its own DCDC3 1050 mV and DCDC5 1000 mV reading before any clock write, "
         "23 refusals with no clock write (oscillator, bin, bus, route, transfer, supply, PLL state, modulation, "
         "range, deadline), update and lock timeouts, an expiring deadline; CPU-B back to 1014 MHz on the completed "
         "path with no PMIC access, and its refusals and lock timeout");
    return 0;
}
