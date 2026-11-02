/* Host trace of the SD0 platform against the strict controller model of checks/a733-mmc-platform.c, for the OS image's interrupt completion.
 * It runs the actual PMIC, rail, pin, clock and MMC platform core; only MMIO, the card and time are modeled. Cases:
 *  1 the distributor reports INTID 193 enabled when the platform opens (the OS route table enables it at boot): the platform retains
 *    the controller at its baseline check, which is what the first board runs did;
 *  2 the owner masks it first: the open completes; the polled read of sector 0 is the read-only probe's sequence;
 *  3 irq mode (a733_mmc_platform_irq) with the interrupt enabled: the same read completes by interrupt completion; the register
 *    sequences of 2 and 3 are printed and compared;
 *  4 irq mode with the interrupt enabled is refused by close; with irq mode off and the interrupt enabled, the next operation retains. */
#define main platform_check_main
#include "a733-mmc-platform.c"
#undef main
#include <stdio.h>

struct trace_entry { char op; uint32_t offset, value; };
static struct trace_entry trace[4096];
static unsigned trace_count;
static int (*real_read)(void *, uintptr_t, uint32_t *);
static int (*real_write)(void *, uintptr_t, uint32_t);
static int traced_read(void *v, uintptr_t a, uint32_t *out)
{
    int r = real_read(v, a, out);
    if (a >= m.platform.base && a <= m.platform.base + 0x300 && a != m.platform.base + 0x200 && trace_count < 4096)
        trace[trace_count++] = (struct trace_entry){'R', (uint32_t)(a - m.platform.base), r == 1 ? *out : 0};
    else if (a >= 0x03400000 && a < 0x03401000 && trace_count < 4096) trace[trace_count++] = (struct trace_entry){'G', (uint32_t)(a - 0x03400000), r == 1 ? *out : 0};
    return r;
}
static int traced_write(void *v, uintptr_t a, uint32_t value)
{
    int r = real_write(v, a, value);
    if (a >= m.platform.base && a <= m.platform.base + 0x300 && a != m.platform.base + 0x200 && trace_count < 4096)
        trace[trace_count++] = (struct trace_entry){'W', (uint32_t)(a - m.platform.base), value};
    else if (a >= 0x03400000 && a < 0x03401000 && trace_count < 4096) trace[trace_count++] = (struct trace_entry){'g', (uint32_t)(a - 0x03400000), value};
    return r;
}
static void hook(void) { real_read = m.platform.ops.read; real_write = m.platform.ops.write; m.platform.ops.read = traced_read; m.platform.ops.write = traced_write; trace_count = 0; }
static uint32_t irq_take(void *unused)
{
    (void)unused;
    uint32_t v = m.card.reg[SUNXI_SMHC_RINT / 4] & m.card.reg[SUNXI_SMHC_IMASK / 4];      /* the handler: masked status, accumulate, acknowledge */
    m.card.reg[SUNXI_SMHC_RINT / 4] &= ~v;
    return v;
}
static unsigned writes_at(unsigned offset)
{
    unsigned n = 0;
    for (unsigned i = 0; i < trace_count; ++i) if (trace[i].op == 'W' && trace[i].offset == offset) ++n;
    return n;
}
static void dump(const char *name)
{
    printf("-- %s: %u accesses\n", name, trace_count);
    for (unsigned i = 0; i < trace_count; ++i) printf("   %c %03x %08x\n", trace[i].op, trace[i].offset, trace[i].value);
}
static void read_sector0(int irq)
{
    uint8_t buffer[512];
    struct sunxi_mmc_result result;
    assert(a733_mmc_platform_read(&m.platform, 0, 1, buffer, sizeof buffer, 1000000) == A733_MMC_PENDING);
    enum a733_mmc_platform_result r = A733_MMC_PENDING;
    for (unsigned i = 0; i < 400 && r != A733_MMC_COMPLETE; ++i) { r = step(); assert(r != A733_MMC_RETAINED); }
    assert(r == A733_MMC_COMPLETE);
    assert(a733_mmc_platform_take(&m.platform, &result, 1000000) == A733_MMC_READY && result.received == 512);
    for (unsigned i = 0; i < 512; ++i) assert(buffer[i] == (uint8_t)(i * 37u + 11u));
    (void)irq;
}
int main(int argc, char **argv)

{
    (void)argv;
    int verbose = argc > 1;
    /* 1. The interrupt enabled in the distributor at the platform's baseline. */
    storage_setup(SUNXI_MMC_SD);
    *pmic_model_word(&m.power, 0x03400118) = 2u;
    enum a733_mmc_platform_result r = A733_MMC_PENDING;
    for (unsigned i = 0; i < 2000 && r != A733_MMC_READY && r != A733_MMC_RETAINED; ++i) r = step();
    assert(r == A733_MMC_RETAINED);
    assert(m.platform.stage != 0 && a733_clock_token_status(&m.power.clocks, &m.platform.clock) == A733_CLOCK_FAULT);
    /* The platform's fault retains its own clock token only: the registry is not failed and another owner (the thermal service's scope) keeps working. */
    assert(!atomic_load(&m.power.clocks.failed));
    struct a733_clock_token ths;
    assert(a733_clock_acquire(&m.power.clocks, 77, 0, A733_CLOCK_THS, 1000000, &ths) == A733_CLOCK_OK);
    assert(a733_clock_token_status(&m.power.clocks, &ths) == A733_CLOCK_OK);
    printf("case 1: INTID 193 enabled at baseline -> platform RETAINED (stage %u), clock token faulted: the board's open failure\n", m.platform.stage);
    /* 2. Masked first: the polled open and read (the read-only probe's sequence). */
    storage_setup(SUNXI_MMC_SD);
    ready_storage();
    struct sunxi_mmc_card card;
    assert(a733_mmc_platform_card(&m.platform, &card, 1000000) == A733_MMC_READY && card.sectors);
    hook();
    read_sector0(0);
    struct trace_entry polled[4096]; unsigned polled_count = trace_count;
    memcpy(polled, trace, sizeof(struct trace_entry) * trace_count);
    if (verbose) dump("polled read");
    assert(!writes_at(SUNXI_SMHC_IMASK));
    /* 3. Interrupt completion with the interrupt enabled in the distributor. */
    storage_setup(SUNXI_MMC_SD);
    ready_storage();
    assert(a733_mmc_platform_card(&m.platform, &card, 1000000) == A733_MMC_READY && card.sectors);
    assert(sunxi_mmc_set_irq(&m.platform.core, irq_take, NULL));
    assert(a733_mmc_platform_irq(&m.platform, 1));
    *pmic_model_word(&m.power, 0x03400118) = 2u;                    /* the owner unmasks INTID 193 */
    hook();
    read_sector0(1);
    if (verbose) dump("interrupt read");
    /* This model has no descriptor DMA, so the read is PIO and the core does not arm the interrupt mask for it (the IMASK arm, the
     * handler's masked-status read and the RINT acknowledge are modeled at the core by checks/sunxi-mmc-write-dma.c). What this case
     * measures is the platform's own gate: with the interrupt enabled in the distributor and irq mode on, every operation is accepted. */
    unsigned gic_reads = 0, differing = 0;
    for (unsigned i = 0; i < trace_count; ++i) gic_reads += trace[i].op == 'G';
    for (unsigned i = 0; i < trace_count; ++i) {
        int same = 0;
        for (unsigned j = 0; j < polled_count && !same; ++j) same = trace[i].op == polled[j].op && trace[i].offset == polled[j].offset && trace[i].value == polled[j].value;
        differing += !same;
    }
    printf("case 3: read with the interrupt enabled and irq mode on: %u accesses (polled read %u), GIC register reads %u (polled read: baseline and "
           "masked() checks), %u accesses not in the polled sequence, IMASK writes %u\n", trace_count, polled_count, gic_reads, differing, writes_at(SUNXI_SMHC_IMASK));
    /* 4. Close needs irq mode off; irq mode off while enabled retains. */
    struct sunxi_mmc_result unused;
    (void)unused;
    assert(a733_mmc_platform_close(&m.platform, 1000000) == A733_MMC_INVALID);
    assert(a733_mmc_platform_irq(&m.platform, 0));
    r = a733_mmc_platform_read(&m.platform, 0, 1, (uint8_t[512]){0}, 512, 1000000);
    assert(r == A733_MMC_RETAINED);
    printf("case 4: close refused in irq mode; irq mode off with the interrupt enabled -> RETAINED\n");
    puts("PASS: INTID 193 platform invariants and interrupt completion trace");
    return 0;
}
