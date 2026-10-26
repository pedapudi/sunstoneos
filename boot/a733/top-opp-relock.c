// CPU0's relock of the three CPU clock domains to the top operating points
// of speed bin vf0400 before the returning eight-core workload releases any
// secondary (docs/designs/cpu-top-opp.md, step 6). The ROM resets the CPU-L
// and DSU PLLs on every return, so the workload's own launch sets them; it
// leaves CPU-B as the last image set it, which bundle 26 requires at the boot
// word. The CPU rail voltages, which the ROM keeps, were raised by bundle 25
// earlier in the same lifetime and read back by bundle 26 after a ROM return.
// On the completed path, after the secondaries are OFF, top_opp_lower_b
// returns CPU-B to 1014 MHz before the return.
//
// The routine does not rely on that history. Before its first clock write it
// requires the 26 MHz oscillator, speed-bin code 0x04, every domain on its
// locked PLL with modulation off, and DCDC3 at 1050 mV and DCDC5 at 1000 mV,
// which it reads from the AXP8191 itself over S_TWI0 with the PMIC
// provider's bus sequence and checks (drivers/power/a733_pmic.c). A refusal
// returns a code and leaves every clock at its boot point.
//
// CPU-B goes first, with cores 6 and 7 still off, then CPU-L, then the DSU
// after both clusters, as the vendor's dsufreq orders it. Each domain follows
// the CPU clock driver's sequence (drivers/clock/a733_cpu_clock.c): park the
// mux on the 26 MHz oscillator and wait 20 us (boot0), write N and then N with
// the update bit, wait for the update to clear (10 ms bound) and for three
// lock reads at least 3 us apart (70 ms bound; ccu_common.c), wait 20 us,
// write P = 0, select the PLL and wait 20 us. Every mux write is read back.
// EL3, CPU0, MMU off, interrupts masked; no stack beyond the caller's frame.
#include <stdint.h>
#include "top-opp-relock.h"

#ifndef TOP_OPP_READ
uint64_t a733_smp_counter(void);
#define TOP_OPP_NOW() a733_smp_counter()
static inline __attribute__((always_inline)) uint32_t top_opp_read(uintptr_t address)
{
    __asm__ volatile("dsb sy" ::: "memory");
    const uint32_t value = *(volatile uint32_t *)address;
    __asm__ volatile("dsb sy" ::: "memory");
    return value;
}
static inline __attribute__((always_inline)) void top_opp_write(uintptr_t address, uint32_t value)
{
    __asm__ volatile("dsb sy" ::: "memory");
    *(volatile uint32_t *)address = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
#define TOP_OPP_READ top_opp_read
#define TOP_OPP_WRITE top_opp_write
#endif

#define AI static inline __attribute__((always_inline))
#define PLL_ON 0xf8000000u   /* Enable, LDO, lock enable, lock, output. */
#define UPDATE (1u << 26)
#define LOCK (1u << 28)
#define MUX (7u << 24)
#define DIVP (3u << 16)
#define TICKS_PER_US 24u
#define SETTLE_TICKS (20u * TICKS_PER_US + 1u)
#define GAP_TICKS (3u * TICKS_PER_US + 1u)
#define UPDATE_TICKS (10000u * TICKS_PER_US)
#define LOCK_TICKS (70000u * TICKS_PER_US)
#define OSC 0x07090160u
#define SID_DVFS 0x0300624cu
#define TWI 0x07083000u
#define BUS_TICKS (2000u * TICKS_PER_US) /* One bus event; a byte takes about 90 us at 100 kHz. */
#define PMIC_READS 7u

/* Domain order and targets, one byte each: CPU-B 2002 MHz (N 77), CPU-L
 * 1794 MHz (N 69), DSU 1274 MHz (N 49); every target uses P = 0 from the
 * 26 MHz oscillator. */
#define DOMAIN(i) ((0x030102u >> (8u * (i))) & 255u)
#define TARGET_N(i) ((0x31454du >> (8u * (i))) & 255u)

/* The PMIC provider's startup baseline of S_TWI0 (a733_pmic.c baseline()),
 * without its GIC words: the gate and reset, PL0 and PL1 in the TWI
 * function, an idle bus with the interrupt enable clear, and the driver
 * engine, DMA requests and FIFO interrupts off. Address, mask, value. */
__attribute__((section(".rodata.top_opp")))
static const uint32_t bus_baseline[][3] = {
    {0x0701019cu, 0x10001u, 0x10001u}, {0x07025000u, 0xffu, 0x22u},
    {TWI + 0x0c, 0xffu, 0x40u}, {TWI + 0x10, 0xffu, 0xf8u}, {TWI + 0x20, 0x35u, 0x30u},
    {TWI + 0x18, 1u, 0u}, {TWI + 0x1c, 0xffffffffu, 0u}, {TWI + 0x200, 0x8f000003u, 0u},
    {TWI + 0x210, 0xc3u, 0xc0u}, {TWI + 0x214, 0x000f0003u, 0u}, {TWI + 0x218, 0x01000100u, 0u},
    {TWI + 0x21c, 0x007f007fu, 0u}, {TWI + 0x314, 0xffffu, 0u}, {TWI + 0x318, 0xffffffu, 0u},
    {TWI + 0x31c, 0xffffffu, 0u},
};
/* One register read of the AXP8191 at 0x36, as the provider runs it: START,
 * write address, register, repeated START, read address, one byte without
 * acknowledgement. Data byte (0x100: none, 0x200: the register), control
 * word, then the status that must follow. */
__attribute__((section(".rodata.top_opp")))
static const uint16_t bus_phases[][3] = {
    {0x100, 0x68, 0x08}, {0x6c, 0x48, 0x18}, {0x200, 0x48, 0x28},
    {0x100, 0x68, 0x10}, {0x6d, 0x48, 0x40}, {0x100, 0x48, 0x58},
};
/* Page, chip, revision, output enables, DCDC3 and DCDC5 selectors, page. */
#define PMIC_REGISTER(i) ((0x00ff1614100f0effull >> (8u * (i))) & 255u)

static inline __attribute__((always_inline)) uintptr_t pll_word(unsigned i)
{ return 0x08870000u + 0x1000u * DOMAIN(i); }

AI int wait_until(uint64_t until, uint64_t deadline)
{
    for (;;) {
        const uint64_t now = TOP_OPP_NOW();
        if (now >= deadline) return 0;
        if (now >= until) return 1;
    }
}

/* The S_TWI0 route of a733_pmic.c route(), with the DCXO already checked: APBS0
 * and APBS1 on the oscillator or PLL_REF (a valid 24 MHz PLL_REF when
 * used), and a divider for a 10 to 400 kHz bus. */
AI int bus_route(uint32_t divider)
{
    const uint32_t apbs0 = TOP_OPP_READ(0x0701000cu) & 0x0700001fu, apbs1 = TOP_OPP_READ(0x07010010u) & 0x0700001fu;
    const unsigned source = apbs0 >> 24;
    if ((source != 0 && source != 4) || (apbs1 != 0 && apbs1 != 0x04000000u)) return 0;
    if (apbs1 || source == 4) {
        const uint32_t ref = TOP_OPP_READ(0x02002000u);
        const unsigned n = ((ref >> 8) & 255) + 1, m = ((ref >> 16) & 127) + 1;
        if ((ref & 0xf8000000u) != 0xf8000000u || n < 80 || n > 116 || 26000000ull * n != 24000000ull * m) return 0;
    }
    if (divider & ~0x1ffu) return 0;
    const unsigned m = (divider >> 3) & 15, n = divider & 7;
    const uint64_t denominator = (1ull << n) * (m + 1) * ((m == 0 && (divider & 0x180) == 0x80) ? 11u : 10u);
    const uint64_t parent = apbs1 ? 24000000u : 26000000u;
    return parent >= 10000u * denominator && parent <= 400000u * denominator;
}

/* One bus event: start it, then wait for INT_FLAG with BUS_EN alone among
 * the other control bits and the expected status (a733_pmic.c
 * await_status). */
AI int bus_event(unsigned data, unsigned reg, uint32_t control, uint32_t expected, uint64_t deadline, uint32_t *status)
{
    if (data != 0x100) TOP_OPP_WRITE(TWI + 0x08, data == 0x200 ? reg : data);
    TOP_OPP_WRITE(TWI + 0x0c, control);
    const uint64_t limit = TOP_OPP_NOW() + BUS_TICKS;
    for (;;) {
        const uint32_t c = TOP_OPP_READ(TWI + 0x0c);
        if (c & 8) {
            if ((c & 0xf4) != 0x40) return 0;
            *status = TOP_OPP_READ(TWI + 0x10);
            return *status == expected;
        }
        const uint64_t now = TOP_OPP_NOW();
        if (now >= limit || now >= deadline) return 0;
    }
}

/* STOP, then M_STP clear with BUS_EN alone and the idle status 0xf8. */
AI int bus_stop(uint64_t deadline, uint32_t *status)
{
    TOP_OPP_WRITE(TWI + 0x0c, 0x58);
    const uint64_t limit = TOP_OPP_NOW() + BUS_TICKS;
    for (;;) {
        const uint32_t c = TOP_OPP_READ(TWI + 0x0c);
        if (!(c & 0x10)) {
            if ((c & 0xff) != 0x40) return 0;
            if (((*status = TOP_OPP_READ(TWI + 0x10)) & 0xff) == 0xf8) return 1;
        }
        const uint64_t now = TOP_OPP_NOW();
        if (now >= limit || now >= deadline) return 0;
    }
}

/* One domain's change to N, P = 0, in the CPU clock driver's sequence. Writes
 * park-to-select ticks << 32 | lock reads to *parked and PLL << 32 | mux to
 * *final. Returns 0, or the fault step: 1 park readback, 2 update, 3 lock,
 * 4 select readback, 5 final words, 6 deadline in a settle. */
__attribute__((section(".text.top_opp.domain"), noinline))
static uint64_t top_opp_domain(uintptr_t pll, unsigned target, uint64_t deadline, volatile uint64_t *parked_word,
                               volatile uint64_t *final_word)
{
    const uintptr_t sel = pll + 0x1c;
    const uint32_t mux = TOP_OPP_READ(sel), parked = mux & ~MUX;
    TOP_OPP_WRITE(sel, parked);
    if (TOP_OPP_READ(sel) != parked) return 1;
    const uint64_t park = TOP_OPP_NOW();
    if (!wait_until(park + SETTLE_TICKS, deadline)) return 6;
    const uint32_t word = (TOP_OPP_READ(pll) & ~(255u << 8)) | (uint32_t)target << 8;
    TOP_OPP_WRITE(pll, word);
    TOP_OPP_WRITE(pll, word | UPDATE);
    const uint64_t update = TOP_OPP_NOW();
    uint32_t value;
    while ((value = TOP_OPP_READ(pll)) & UPDATE)
        if (TOP_OPP_NOW() >= update + UPDATE_TICKS || TOP_OPP_NOW() >= deadline) return 2;
    const uint64_t lock = TOP_OPP_NOW();
    unsigned locked = 0, reads = 0;
    uint64_t last = 0;
    while (locked < 3) {
        const uint64_t now = TOP_OPP_NOW();
        if (now >= lock + LOCK_TICKS || now >= deadline) return 3;
        if (locked && now < last + GAP_TICKS) continue;
        value = TOP_OPP_READ(pll);
        ++reads;
        if (((value >> 8) & 255) != target) return 3;
        if (value & LOCK) {
            ++locked;
            last = now;
        } else {
            locked = 0;
        }
    }
    if (!wait_until(TOP_OPP_NOW() + SETTLE_TICKS, deadline)) return 6;
    const uint32_t divided = parked & ~DIVP, selected = divided | 3u << 24;
    TOP_OPP_WRITE(sel, divided);
    if (TOP_OPP_READ(sel) != divided) return 4;
    TOP_OPP_WRITE(sel, selected);
    if (TOP_OPP_READ(sel) != selected) return 4;
    *parked_word = (TOP_OPP_NOW() - park) << 32 | reads;
    if (!wait_until(TOP_OPP_NOW() + SETTLE_TICKS, deadline)) return 6;
    const uint32_t final = TOP_OPP_READ(pll);
    *final_word = (uint64_t)final << 32 | TOP_OPP_READ(sel);
    if ((final & (PLL_ON | UPDATE)) != PLL_ON || ((final >> 8) & 255) != target) return 5;
    return 0;
}

__attribute__((section(".text.top_opp")))
uint64_t top_opp_relock(volatile uint64_t *record, uint64_t deadline)
{
    record[TOP_OPP_MAGIC_WORD] = TOP_OPP_MAGIC;
    record[TOP_OPP_START_WORD] = TOP_OPP_NOW();
    /* The board: the DCXO enabled and stable at 26 MHz, and the speed bin's
     * fuse code (bits 23:16 if nonzero, else bits 15:8) 0x04. */
    const uint32_t osc = TOP_OPP_READ(OSC), sid = TOP_OPP_READ(SID_DVFS);
    record[TOP_OPP_BOARD_WORD] = (uint64_t)osc << 32 | sid;
    if ((osc & 0x10000002u) != 0x10000002u || ((osc >> 14) & 3) != 2)
        return record[TOP_OPP_RESULT_WORD] = TOP_OPP_REFUSED_OSCILLATOR;
    if (((sid >> 16) & 255 ? (sid >> 16) & 255 : (sid >> 8) & 255) != 4)
        return record[TOP_OPP_RESULT_WORD] = TOP_OPP_REFUSED_BIN;
    /* Every domain on its locked PLL with no update pending, N in the 26 MHz
     * VCO range and the pattern and frequency-modulation enables clear. */
    for (unsigned i = 0; i < TOP_OPP_DOMAINS; ++i) {
        const uintptr_t pll = pll_word(i);
        const uint32_t word = TOP_OPP_READ(pll), mux = TOP_OPP_READ(pll + 0x1c);
        record[TOP_OPP_ENTRY_WORD + i] = (uint64_t)word << 32 | mux;
        const unsigned n = (word >> 8) & 255;
        if ((word & (PLL_ON | UPDATE)) != PLL_ON || ((mux >> 24) & 7) != 3)
            return record[TOP_OPP_RESULT_WORD] = TOP_OPP_REFUSED_STATE;
        if ((TOP_OPP_READ(pll + 4) & (3u << 29)) || (TOP_OPP_READ(pll + 8) >> 31) || (TOP_OPP_READ(pll + 0x18) >> 31))
            return record[TOP_OPP_RESULT_WORD] = TOP_OPP_REFUSED_MODULATION;
        if (n < 26 || n > 86)
            return record[TOP_OPP_RESULT_WORD] = TOP_OPP_REFUSED_RANGE;
    }
    if (TOP_OPP_NOW() >= deadline)
        return record[TOP_OPP_RESULT_WORD] = TOP_OPP_REFUSED_DEADLINE;
    /* The supplies: S_TWI0 idle in the provider's baseline, then seven PMIC
     * reads. */
    const uint32_t divider = TOP_OPP_READ(TWI + 0x14);
    record[TOP_OPP_BUS_WORD] = (uint64_t)divider << 32;
    for (unsigned i = 0; i < sizeof(bus_baseline) / sizeof(*bus_baseline); ++i)
        if ((TOP_OPP_READ(bus_baseline[i][0]) & bus_baseline[i][1]) != bus_baseline[i][2])
            return record[TOP_OPP_RESULT_WORD] = TOP_OPP_REFUSED_BUS;
    if (!bus_route(divider))
        return record[TOP_OPP_RESULT_WORD] = TOP_OPP_REFUSED_BUS;
    uint64_t bytes = 0;
    for (unsigned r = 0; r < PMIC_READS; ++r) {
        uint32_t status = 0;
        unsigned ok = 1;
        for (unsigned k = 0; ok && k < sizeof(bus_phases) / sizeof(*bus_phases); ++k)
            ok = bus_event(bus_phases[k][0], PMIC_REGISTER(r), bus_phases[k][1], bus_phases[k][2], deadline, &status);
        const uint32_t value = ok ? TOP_OPP_READ(TWI + 0x08) & 255 : 0;
        if (ok) ok = bus_stop(deadline, &status);
        else TOP_OPP_WRITE(TWI + 0x0c, 0x58); /* Release the bus; the run ends here. */
        record[TOP_OPP_BUS_WORD] = (uint64_t)divider << 32 | (r + ok) << 8 | (status & 255);
        if (!ok)
            return record[TOP_OPP_RESULT_WORD] = TOP_OPP_REFUSED_TRANSFER;
        bytes |= (uint64_t)value << (8 * r);
        record[TOP_OPP_PMIC_WORD] = bytes;
    }
    if ((TOP_OPP_READ(TWI + 0x20) & 0x35) != 0x30)
        return record[TOP_OPP_RESULT_WORD] = TOP_OPP_REFUSED_TRANSFER;
    /* Page 0 before and after, chip 3, revision 0, DCDC3 (bit 2) and DCDC5
     * (bit 4) enabled, DCDC3 code 55 (1050 mV) and DCDC5 code 50 (1000 mV):
     * 500 mV + 10 mV per code below 71. */
    const uint32_t enable = (bytes >> 24) & 255;
    if ((bytes & 0xffffff) != 0x000300 || (bytes >> 48) != 0 || (enable & 0x14) != 0x14 ||
        ((bytes >> 32) & 127) != 55 || ((bytes >> 40) & 127) != 50)
        return record[TOP_OPP_RESULT_WORD] = TOP_OPP_REFUSED_SUPPLY;
    if (TOP_OPP_NOW() >= deadline)
        return record[TOP_OPP_RESULT_WORD] = TOP_OPP_REFUSED_DEADLINE;
    for (unsigned i = 0; i < TOP_OPP_DOMAINS; ++i) {
        const uint64_t fault = top_opp_domain(pll_word(i), TARGET_N(i), deadline, &record[TOP_OPP_PARKED_WORD + i],
                                              &record[TOP_OPP_FINAL_WORD + i]);
        if (fault) return record[TOP_OPP_RESULT_WORD] = TOP_OPP_FAULT | (uint64_t)i << 4 | fault;
    }
    record[TOP_OPP_END_WORD] = TOP_OPP_NOW();
    return record[TOP_OPP_RESULT_WORD] = TOP_OPP_DONE;
}

/* CPU-B back to boot0's 1014 MHz (N 39) on the completed workload's path,
 * after all seven secondaries are OFF and before the return: the ROM return
 * resets CPU-L and the DSU but not CPU-B, and no image returns with CPU-B
 * raised. A lowering at the kept 1050 mV needs no voltage change. CPU-B must
 * read the relock's word, locked, with modulation off; otherwise nothing is
 * written. Its four-word record: result, entry and final PLL << 32 | mux,
 * and park-to-select ticks << 32 | lock reads. */
__attribute__((section(".text.top_opp.lower")))
uint64_t top_opp_lower_b(volatile uint64_t *lower, uint64_t deadline)
{
    const uintptr_t pll = 0x08872000u;
    const uint32_t word = TOP_OPP_READ(pll), mux = TOP_OPP_READ(pll + 0x1c);
    lower[TOP_OPP_LOWER_ENTRY_WORD] = (uint64_t)word << 32 | mux;
    if ((word & (PLL_ON | UPDATE)) != PLL_ON || ((word >> 8) & 255) != TARGET_N(0) || mux != 0x03000000u ||
        (TOP_OPP_READ(pll + 4) & (3u << 29)) || (TOP_OPP_READ(pll + 8) >> 31) || (TOP_OPP_READ(pll + 0x18) >> 31))
        return lower[TOP_OPP_LOWER_RESULT_WORD] = TOP_OPP_REFUSED_STATE;
    if (TOP_OPP_NOW() >= deadline)
        return lower[TOP_OPP_LOWER_RESULT_WORD] = TOP_OPP_REFUSED_DEADLINE;
    const uint64_t fault = top_opp_domain(pll, TOP_OPP_BOOT_B_N, deadline, &lower[TOP_OPP_LOWER_PARKED_WORD],
                                          &lower[TOP_OPP_LOWER_FINAL_WORD]);
    return lower[TOP_OPP_LOWER_RESULT_WORD] = fault ? TOP_OPP_FAULT | fault : TOP_OPP_DONE;
}
