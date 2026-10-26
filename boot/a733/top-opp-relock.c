// CPU0's relock of the three CPU clock domains to the top operating points
// of speed bin vf0400 before the returning eight-core workload releases any
// secondary (docs/designs/cpu-top-opp.md, step 6). The ROM resets the CPU
// PLLs on every return, so the workload's own launch sets them; the CPU rail
// voltages, which the ROM keeps, were raised by bundle 25 earlier in the same
// lifetime and read back by bundle 15 after a ROM return.
//
// CPU-B goes first, with cores 6 and 7 still off, then CPU-L, then the DSU
// after both clusters, as the vendor's dsufreq orders it. Each domain follows
// the CPU clock driver's sequence (drivers/clock/a733_cpu_clock.c): park the
// mux on the 26 MHz oscillator and wait 20 us (boot0), write N and then N with
// the update bit, wait for the update to clear (10 ms bound) and for three
// lock reads at least 3 us apart (70 ms bound; ccu_common.c), wait 20 us,
// write P = 0, select the PLL and wait 20 us. Every mux write is read back.
// Every entry check runs before the first write, so a refusal leaves the
// boot clocks untouched. EL3, CPU0, interrupts masked; no stack beyond the
// caller's frame.
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

/* Domain order and targets, one byte each: CPU-B 2002 MHz (N 77), CPU-L
 * 1794 MHz (N 69), DSU 1274 MHz (N 49); every target uses P = 0 from the
 * 26 MHz oscillator. Immediates keep the routine in its own section. */
#define DOMAIN(i) ((0x030102u >> (8u * (i))) & 255u)
#define TARGET_N(i) ((0x31454du >> (8u * (i))) & 255u)

static inline __attribute__((always_inline)) uintptr_t pll_word(unsigned i)
{ return 0x08870000u + 0x1000u * DOMAIN(i); }

static inline __attribute__((always_inline)) int wait_until(uint64_t until, uint64_t deadline)
{
    for (;;) {
        const uint64_t now = TOP_OPP_NOW();
        if (now >= deadline) return 0;
        if (now >= until) return 1;
    }
}

__attribute__((section(".text.top_opp")))
uint64_t top_opp_relock(volatile uint64_t *record, uint64_t deadline)
{
    record[TOP_OPP_MAGIC_WORD] = TOP_OPP_MAGIC;
    record[TOP_OPP_START_WORD] = TOP_OPP_NOW();
    /* Entry: every domain on its locked PLL with no update pending, N in the
     * 26 MHz VCO range and the pattern and frequency-modulation enables clear. */
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
    for (unsigned i = 0; i < TOP_OPP_DOMAINS; ++i) {
        const uintptr_t pll = pll_word(i), sel = pll + 0x1c;
        const uint64_t fault = TOP_OPP_FAULT | (uint64_t)i << 4;
        const uint32_t mux = TOP_OPP_READ(sel), parked = mux & ~MUX;
        TOP_OPP_WRITE(sel, parked);
        if (TOP_OPP_READ(sel) != parked) return record[TOP_OPP_RESULT_WORD] = fault | 1;
        const uint64_t park = TOP_OPP_NOW();
        if (!wait_until(park + SETTLE_TICKS, deadline)) return record[TOP_OPP_RESULT_WORD] = fault | 6;
        const uint32_t word = (TOP_OPP_READ(pll) & ~(255u << 8)) | (uint32_t)TARGET_N(i) << 8;
        TOP_OPP_WRITE(pll, word);
        TOP_OPP_WRITE(pll, word | UPDATE);
        const uint64_t update = TOP_OPP_NOW();
        uint32_t value;
        while ((value = TOP_OPP_READ(pll)) & UPDATE)
            if (TOP_OPP_NOW() >= update + UPDATE_TICKS || TOP_OPP_NOW() >= deadline)
                return record[TOP_OPP_RESULT_WORD] = fault | 2;
        const uint64_t lock = TOP_OPP_NOW();
        unsigned locked = 0, reads = 0;
        uint64_t last = 0;
        while (locked < 3) {
            const uint64_t now = TOP_OPP_NOW();
            if (now >= lock + LOCK_TICKS || now >= deadline) return record[TOP_OPP_RESULT_WORD] = fault | 3;
            if (locked && now < last + GAP_TICKS) continue;
            value = TOP_OPP_READ(pll);
            ++reads;
            if (((value >> 8) & 255) != TARGET_N(i)) return record[TOP_OPP_RESULT_WORD] = fault | 3;
            if (value & LOCK) {
                ++locked;
                last = now;
            } else {
                locked = 0;
            }
        }
        if (!wait_until(TOP_OPP_NOW() + SETTLE_TICKS, deadline)) return record[TOP_OPP_RESULT_WORD] = fault | 6;
        const uint32_t divided = parked & ~DIVP, selected = divided | 3u << 24;
        TOP_OPP_WRITE(sel, divided);
        if (TOP_OPP_READ(sel) != divided) return record[TOP_OPP_RESULT_WORD] = fault | 4;
        TOP_OPP_WRITE(sel, selected);
        if (TOP_OPP_READ(sel) != selected) return record[TOP_OPP_RESULT_WORD] = fault | 4;
        record[TOP_OPP_PARKED_WORD + i] = (TOP_OPP_NOW() - park) << 32 | reads;
        if (!wait_until(TOP_OPP_NOW() + SETTLE_TICKS, deadline)) return record[TOP_OPP_RESULT_WORD] = fault | 6;
        const uint32_t final = TOP_OPP_READ(pll);
        record[TOP_OPP_FINAL_WORD + i] = (uint64_t)final << 32 | TOP_OPP_READ(sel);
        if ((final & (PLL_ON | UPDATE)) != PLL_ON || ((final >> 8) & 255) != TARGET_N(i))
            return record[TOP_OPP_RESULT_WORD] = fault | 5;
    }
    record[TOP_OPP_END_WORD] = TOP_OPP_NOW();
    return record[TOP_OPP_RESULT_WORD] = TOP_OPP_DONE;
}
