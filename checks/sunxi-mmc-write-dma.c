/* Strict model of the SMHC controller and an SD card for CMD25 writes through descriptor DMA, driven by
 * the real core (drivers/storage/sunxi_mmc.c). The model accepts only the register accesses the vendor
 * sequence uses (docs/designs/mmc-multiblock-write.md) and asserts on any other. It consumes the
 * descriptor chain in order, clears each OWN bit, moves the data into a media array, and raises the
 * command-done, data-done and automatic-stop-done interrupts and the DAT0 busy status on a schedule of
 * register reads. The sentinel pattern outside the written range must survive. */
#define _GNU_SOURCE
#include "sunxi_mmc.h"
#include "sunxi_smhc.h"
#include "storage_wait.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define SECTORS 600u
#ifndef SUNXI_SMHC_MISTA
#define SUNXI_SMHC_MISTA 0x34u          /* masked interrupt status */
#endif
struct model {
    uint32_t reg[0x304 / 4];
    uint8_t *media;
    unsigned kind_sd, sector_addressed;
    uint64_t now;
    /* current transfer */
    int cmd_active, data_cmd, dma_on, next_desc, descs, ticks_cc, ticks_dtc, ticks_acd, busy_left, start_pending_reads;
    uint64_t first, bytes;
    unsigned prg_polls_left, programming_state;
    /* injections */
    unsigned busy_polls, prg_polls, crc_error, card_error_cmd, card_error_stop, idst_error, never_done, hang_busy;
    unsigned revoke_after, authority_calls, cache_before, cache_after, sync_order_bad;
    unsigned thld_initial;
    unsigned rechecks;                    /* interrupt waits that ended on the recheck timer, not the interrupt */
    uint32_t recheck_us;                  /* the request's interrupt recheck: 0 selects the default (20 ms) */
    unsigned commands[16], command_count;
    /* interrupt mode: events fire by model time, the line is MISTA = RINT & IMASK, the handler acknowledges in the vendor order */
    unsigned timed, irq_mode, drop_irq, spurious, isr_runs, sleeps, polls, spins, notified, early_isr;
    uint64_t t_done, busy_until, dma_us, busy_us, tick;
    uint32_t pending_taken;
    unsigned writes_seen, reads_seen;
    unsigned samp_n, samp_value[8], samp_at[8];       /* writes of SAMP_DL (0x144): the value and how many commands had been issued */
};
static struct model m;
static struct sunxi_mmc h;
static volatile struct sunxi_mmc_dma_descriptor *desc;
static uint8_t *source;

static void *low_alloc(size_t bytes)
{
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    assert(p != MAP_FAILED && (uintptr_t)p < (UINT64_C(1) << 32) - bytes);
    return p;
}
static uint8_t sentinel(unsigned i) { return (uint8_t)(i * 131u + 7u); }
static uint8_t payload(unsigned i) { return (uint8_t)(i * 29u + 3u); }

static void check_chain(void)
{
    unsigned n = (unsigned)((m.bytes + 4095) / 4096);
    assert(m.reg[SUNXI_SMHC_DLBA / 4] == (uint32_t)((uintptr_t)desc >> 2));
    for (unsigned i = 0; i < n; ++i) {
        uint32_t want = SUNXI_SMHC_DES_OWN | SUNXI_SMHC_DES_CH | SUNXI_SMHC_DES_DIC;
        if (!i) want |= SUNXI_SMHC_DES_FD;
        if (i == n - 1) want = (want | SUNXI_SMHC_DES_LD) & ~SUNXI_SMHC_DES_DIC;
        size_t left = m.bytes - (size_t)i * 4096;
        assert(desc[i].config == want);
        assert(desc[i].size == (left < 4096 ? left : 4096));
        assert(desc[i].buffer == (uint32_t)(((uintptr_t)source + (size_t)i * 4096) >> 2));
        assert(desc[i].next == (uint32_t)((uintptr_t)&desc[i + 1] >> 2));
    }
    assert(!desc[n].config && !desc[n].size && !desc[n].buffer && !desc[n].next);
    m.descs = (int)n;
}

static void dma_step(void)
{
    if (m.next_desc >= m.descs) return;
    volatile struct sunxi_mmc_dma_descriptor *d = &desc[m.next_desc];
    assert(d->config & SUNXI_SMHC_DES_OWN);
    memcpy(m.media + m.first * 512 + (size_t)m.next_desc * 4096, (void *)(uintptr_t)((uint64_t)d->buffer << 2), d->size);
    d->config &= ~SUNXI_SMHC_DES_OWN;
    ++m.next_desc;
}

static void timed_progress(void)
{
    if (!m.timed || !m.cmd_active || m.data_cmd != 25 || m.next_desc >= m.descs || m.now < m.t_done) return;
    while (m.next_desc < m.descs) dma_step();
    m.reg[SUNXI_SMHC_RINT / 4] |= SUNXI_SMHC_INT_DATA_DONE | SUNXI_SMHC_INT_AUTO_STOP_DONE;
    m.busy_until = m.now + m.busy_us;
}
static uint32_t irq_line(void) { return (m.reg[0] & SUNXI_SMHC_GCTRL_INT_ENABLE) ? m.reg[SUNXI_SMHC_RINT / 4] & m.reg[SUNXI_SMHC_IMASK / 4] : 0; }   /* GCTRL bit 4 gates the line */
/* The handler, in the board kernel's order (sunxi_mmc_irq): read the masked status, accumulate, write the raw bits back. */
static void isr(void)
{
    ++m.isr_runs;
    uint32_t mista = irq_line();
    m.pending_taken |= mista;
    m.reg[SUNXI_SMHC_RINT / 4] &= ~mista;
    m.notified = 1;
}
static uint32_t irq_taken(void *c) { (void)c; uint32_t v = m.pending_taken; m.pending_taken = 0; return v; }

static int read32(void *ctx, uint32_t off, uint32_t *out)
{
    (void)ctx; ++m.reads_seen;
    assert(!(off & 3) && off < 0x304);
    timed_progress();
    switch (off) {
    case SUNXI_SMHC_GCTRL: *out = m.reg[0] & ~7u; return 1;                /* resets self-clear */
    case SUNXI_SMHC_CMD:
        *out = m.reg[off / 4];
        if (m.start_pending_reads && !--m.start_pending_reads) m.reg[off / 4] &= ~SUNXI_SMHC_CMD_START;
        return 1;
    case SUNXI_SMHC_MISTA: *out = irq_line(); return 1;
    case SUNXI_SMHC_IMASK: *out = m.reg[off / 4]; return 1;
    case SUNXI_SMHC_RESP0: case SUNXI_SMHC_RESP1: case SUNXI_SMHC_DMAC: case SUNXI_SMHC_THLD:
        *out = off == SUNXI_SMHC_DMAC ? m.reg[off / 4] & ~SUNXI_SMHC_DMAC_RESET : m.reg[off / 4]; return 1;
    case SUNXI_SMHC_RINT:
        if (m.cmd_active && !m.timed) {
            if (m.ticks_cc) { if (!--m.ticks_cc) m.reg[off / 4] |= SUNXI_SMHC_INT_CMD_DONE; }
            if (m.data_cmd == 25 && m.dma_on) {
                if (m.next_desc < m.descs) dma_step();
                else if (m.never_done) {}
                else if (m.ticks_dtc) { if (!--m.ticks_dtc) { m.reg[off / 4] |= SUNXI_SMHC_INT_DATA_DONE; m.busy_left = (int)m.busy_polls; } }
                else if (m.ticks_acd) { if (!--m.ticks_acd) m.reg[off / 4] |= SUNXI_SMHC_INT_AUTO_STOP_DONE; }
            }
            if (m.crc_error && m.next_desc > 0) m.reg[off / 4] |= 1u << 7;
        }
        *out = m.reg[off / 4]; return 1;
    case SUNXI_SMHC_STATUS:
        *out = SUNXI_SMHC_STATUS_FIFO_EMPTY;
        if (m.timed && m.now < m.busy_until) *out |= 1u << 9;
        else if (m.busy_left > 0 || m.hang_busy) { *out |= 1u << 9; if (m.busy_left > 0) --m.busy_left; }
        else if (!m.timed && m.cmd_active && m.data_cmd == 25 && m.ticks_dtc) *out |= 1u << 10;
        return 1;
    case SUNXI_SMHC_CBCR: case 0x4c: *out = (uint32_t)((size_t)m.next_desc * 4096 < m.bytes ? (size_t)m.next_desc * 4096 : m.bytes); return 1;   /* TBC0 and TBC1 */
    case 0x90: *out = (uint32_t)(uintptr_t)desc + m.next_desc * (uint32_t)sizeof(struct sunxi_mmc_dma_descriptor); return 1;                     /* the current host descriptor */
    case SUNXI_SMHC_IDST:
        *out = m.reg[off / 4];
        if (m.dma_on && m.next_desc < m.descs) *out |= 1u << 13;          /* IDMAC state: busy */
        if (m.idst_error && m.next_desc > 0) *out |= 1u << 5;
        return 1;
    default: assert(!"unexpected controller read"); return 0;
    }
}

static void command(uint32_t value)
{
    unsigned index = value & 63u;
    assert(m.command_count < 16);
    m.commands[m.command_count++] = index;
    m.reg[SUNXI_SMHC_CMD / 4] = value;
    m.start_pending_reads = 1;
    m.cmd_active = 1; m.data_cmd = 0; m.reg[SUNXI_SMHC_RINT / 4] = 0; m.ticks_cc = 1;
    if (m.timed) m.reg[SUNXI_SMHC_RINT / 4] |= SUNXI_SMHC_INT_CMD_DONE;
    if (index == 13) {
        assert(value == (SUNXI_SMHC_CMD_START | SUNXI_SMHC_CMD_WAIT | SUNXI_SMHC_CMD_CRC | SUNXI_SMHC_CMD_RESP | 13u));
        assert(m.reg[SUNXI_SMHC_ARG / 4] == 0x12340000u);
        if (m.writes_seen && m.prg_polls_left) { --m.prg_polls_left; m.reg[SUNXI_SMHC_RESP0 / 4] = 7u << 9; }
        else m.reg[SUNXI_SMHC_RESP0 / 4] = 0x900;
        return;
    }
    assert(index == 25);
    assert(value == (SUNXI_SMHC_CMD_START | SUNXI_SMHC_CMD_WAIT | SUNXI_SMHC_CMD_AUTO_STOP | SUNXI_SMHC_CMD_WRITE |
                     SUNXI_SMHC_CMD_DATA | SUNXI_SMHC_CMD_CRC | SUNXI_SMHC_CMD_RESP | 25u));
    assert(m.reg[SUNXI_SMHC_BLKSZ / 4] == 512 && m.reg[SUNXI_SMHC_BCNT / 4] == m.bytes);
    assert(m.reg[SUNXI_SMHC_A12A / 4] == 0);
    assert(m.reg[SUNXI_SMHC_ARG / 4] == (m.sector_addressed ? m.first : m.first * 512));
    uint32_t thld = m.reg[SUNXI_SMHC_THLD / 4];
    assert(((thld >> 16) & 0xfff) == 512 && (thld & 4) && !(thld & 1));       /* vendor thld_ctl for a write */
    assert(m.reg[SUNXI_SMHC_FTRGL / 4] == SUNXI_SMHC_FTRGL_VALUE && m.reg[SUNXI_SMHC_IDIE / 4] == 0);
    assert(m.reg[SUNXI_SMHC_DMAC / 4] == SUNXI_SMHC_DMAC_ON && (m.reg[0] & SUNXI_SMHC_GCTRL_DMA_ENABLE));
    assert(!(m.reg[0] & SUNXI_SMHC_GCTRL_CPU_FIFO));
    assert(m.cache_before == 1 && !m.cache_after);                             /* cleaned before the command */
    check_chain();
    if (m.irq_mode) {
        /* Armed as the board kernel arms a data command with an automatic stop: errors and auto-stop done only. */
        assert(m.reg[SUNXI_SMHC_IMASK / 4] == (SUNXI_SMHC_MMC_ERRORS | SUNXI_SMHC_INT_AUTO_STOP_DONE));
    }
    m.data_cmd = 25; m.dma_on = 1; m.next_desc = 0; m.ticks_dtc = 2; m.ticks_acd = 2;
    if (m.timed) { m.t_done = m.now + m.dma_us; m.reg[SUNXI_SMHC_RINT / 4] |= SUNXI_SMHC_INT_CMD_DONE; }
    m.reg[SUNXI_SMHC_RESP0 / 4] = m.card_error_cmd ? 1u << 25 : 0x900;
    m.reg[SUNXI_SMHC_RESP1 / 4] = m.card_error_stop ? 1u << 25 : 0x900;
    ++m.writes_seen;
}

static int write32(void *ctx, uint32_t off, uint32_t value)
{
    (void)ctx;
    assert(!(off & 3) && off < 0x304);
    switch (off) {
    case SUNXI_SMHC_GCTRL: case SUNXI_SMHC_BLKSZ: case SUNXI_SMHC_BCNT: case SUNXI_SMHC_ARG: case SUNXI_SMHC_FTRGL:
    case SUNXI_SMHC_A12A: case SUNXI_SMHC_DLBA: case SUNXI_SMHC_IDIE: m.reg[off / 4] = value;
        if (off == SUNXI_SMHC_GCTRL && (value & SUNXI_SMHC_GCTRL_DMA_ENABLE)) assert(m.cache_before == 1);
        return 1;
    case 0x144: assert(m.samp_n < 8); m.samp_value[m.samp_n] = value; m.samp_at[m.samp_n++] = m.command_count; m.reg[off / 4] = value; return 1;
    case SUNXI_SMHC_THLD: m.reg[off / 4] = value; return 1;
    case SUNXI_SMHC_IMASK: m.reg[off / 4] = value; return 1;
    case SUNXI_SMHC_RINT: m.reg[off / 4] &= ~value; return 1;
    case SUNXI_SMHC_IDST: m.reg[off / 4] &= ~value; return 1;
    case SUNXI_SMHC_DMAC:
        if (value & SUNXI_SMHC_DMAC_RESET) { m.dma_on = 0; m.reg[off / 4] = value; return 1; }
        assert(value == SUNXI_SMHC_DMAC_ON); m.reg[off / 4] = value; return 1;
    case SUNXI_SMHC_CMD: command(value); return 1;
    default: assert(!"unexpected controller write"); return 0;
    }
}
static int no_fifo_read(void *c, uint32_t *w, unsigned n) { (void)c; (void)w; (void)n; assert(!"a DMA write used the FIFO"); return 0; }
static int no_fifo_write(void *c, const uint32_t *w, unsigned n) { (void)c; (void)w; (void)n; assert(!"a DMA write used the FIFO"); return 0; }
static int barrier(void *c) { (void)c; return 1; }
static int clock_now(void *c, uint64_t *out) { (void)c; m.now += m.tick ? m.tick : 50; *out = m.now; return 1; }
static int admit(void *c, uintptr_t token, enum sunxi_mmc_kind kind, uint32_t hz) { (void)c; assert(token == 77 && kind == SUNXI_MMC_SD && hz == 800000); return 1; }
static int retained(void *c, uintptr_t token) { (void)c; assert(token == 77); return 1; }
static int dma_sync(void *c, uint8_t *b, size_t n, unsigned phase)
{
    (void)c;
    assert(b == source && n == m.bytes);
    if (phase == SUNXI_MMC_DMA_BEFORE) { ++m.cache_before; assert(!m.cache_after); }
    else { ++m.cache_after; assert(m.cache_before == 1 && m.next_desc == m.descs); }   /* invalidate only after the chain closed */
    return 1;
}
static int authorization(void *c, const struct sunxi_mmc_write_identity *id, const struct sunxi_mmc_write_request *r)
{
    (void)c; ++m.authority_calls;
    assert(id->core == (uintptr_t)&h && id->session == 77 && id->kind == SUNXI_MMC_SD);
    if (m.revoke_after && m.authority_calls > m.revoke_after) return 0;
    return r->policy_token == 901 && r->first_sector == m.first && r->bytes == m.bytes && r->source == source;
}

static void setup(unsigned sector_addressed, unsigned blocks, uint64_t first)
{
    memset(&m, 0, sizeof m);
    static uint8_t *media; if (!media) media = low_alloc(SECTORS * 512);
    if (!source) source = low_alloc(256 * 512 + 4096);
    if (!desc) desc = low_alloc(SUNXI_MMC_DMA_DESCRIPTORS * sizeof *desc);
    for (unsigned i = 0; i < SECTORS * 512; ++i) media[i] = sentinel(i);
    for (unsigned i = 0; i < 256 * 512; ++i) source[i] = payload(i);
    memset((void *)desc, 0, SUNXI_MMC_DMA_DESCRIPTORS * sizeof *desc);
    m.media = media; m.sector_addressed = sector_addressed; m.first = first; m.bytes = (uint64_t)blocks * 512;
    m.reg[SUNXI_SMHC_THLD / 4] = 0x0ff10001u;                              /* stale read threshold left by an earlier read */
    m.busy_polls = 3; m.prg_polls_left = 0;
    const struct sunxi_mmc_config c = {{read32, write32, barrier, clock_now, admit, retained, NULL, no_fifo_read, no_fifo_write, NULL, dma_sync},
        &m, 77, SUNXI_MMC_SD, 800000, 0x00300000, SUNXI_MMC_SD_LEGACY, desc};
    memset(&h, 0, sizeof h);
    assert(sunxi_mmc_init(&h, &c));
    /* An already identified, selected card (the identification suites run elsewhere). */
    h.card.sectors = SECTORS; h.card.rca = 0x1234; h.card.sector_addressed = (uint8_t)sector_addressed;
    h.card.ocr = (1u << 31) | (sector_addressed ? 1u << 30 : 0) | 0x00300000;
    h.setup = 6; h.admitted = 1; h.state = SUNXI_MMC_READY;
}
static void csd_writable(void)
{
    /* Card bit n is word (127-n)/32, bit (n%32); class 4 = bit 84+4, block length bits 25:22 = 9. */
    memset(h.card.csd, 0, sizeof h.card.csd);
    h.card.csd[(127 - 88) / 32] |= 1u << (88 % 32);
    for (unsigned b = 0; b < 4; ++b) if ((9u >> b) & 1u) h.card.csd[(127 - (22 + b)) / 32] |= 1u << ((22 + b) % 32);
}
static struct sunxi_mmc_write_request request(unsigned blocks)
{ return (struct sunxi_mmc_write_request){901, m.first, blocks, source, blocks * 512u, 1000000}; }
static int start(unsigned blocks)
{
    struct sunxi_mmc_write_request r = request(blocks);
    struct sunxi_mmc_write_authority a = {&m, authorization};
    return sunxi_mmc_write_dma(&h, &r, &a);
}
static void finish(void)
{
    for (unsigned n = 0; n < 20000 && h.state == SUNXI_MMC_WRITING; ++n) sunxi_mmc_poll(&h);
    assert(h.state == SUNXI_MMC_DONE || h.state == SUNXI_MMC_FAULT);
}
static void media_is(unsigned blocks)
{
    for (unsigned i = 0; i < SECTORS * 512; ++i) {
        int inside = i >= m.first * 512 && i < (m.first + blocks) * 512;
        assert(m.media[i] == (inside ? payload(i - (unsigned)m.first * 512) : sentinel(i)));
    }
}
static void media_untouched_outside(unsigned blocks)
{
    for (unsigned i = 0; i < SECTORS * 512; ++i)
        if (!(i >= m.first * 512 && i < (m.first + blocks) * 512)) assert(m.media[i] == sentinel(i));
}

/* The wait loop of the image (drivers/storage/storage_wait.c) against the interrupt model. */
static unsigned hints_irq, hints_timer;
static int w_poll(void *c) { (void)c; ++m.polls; sunxi_mmc_poll(&h); if (m.early_isr && irq_line()) isr(); return h.state != SUNXI_MMC_WRITING; }
static enum sunxi_mmc_wait last_kind;
static enum sunxi_mmc_wait w_hint(void *c, uint32_t *us)
{
    (void)c; enum sunxi_mmc_wait k = last_kind = sunxi_mmc_wait_hint(&h, us);
    if (k == SUNXI_MMC_WAIT_IRQ) ++hints_irq; else if (k == SUNXI_MMC_WAIT_TIMER) ++hints_timer; else ++m.spins;
    return k;
}
static uint64_t w_now(void *c) { (void)c; return m.now; }
static int w_live(void *c) { (void)c; return 1; }
static void w_sleep(void *c, uint64_t until)
{
    (void)c; ++m.sleeps;
    if (m.notified) { m.notified = 0; m.now += 5; timed_progress(); return; }          /* a notification that came before the sleep */
    uint64_t target = until;
    if (m.timed && m.next_desc < m.descs && m.t_done < target) target = m.t_done;
    if (target > m.now) m.now = target;
    timed_progress();
    if (!irq_line() && last_kind == SUNXI_MMC_WAIT_IRQ && !m.drop_irq) ++m.rechecks;                       /* the timer ended an interrupt wait */
    if (irq_line()) {
        if (m.drop_irq) { m.drop_irq = 0; if (m.now < until) m.now = until; }          /* the interrupt is lost: only the recheck finds it */
        else { isr(); m.notified = 0; }
    }
}
static void irq_case(const char *name, unsigned blocks, uint64_t dma_us, uint64_t busy_us, unsigned drop, unsigned early, unsigned stale)
{
    setup(1, blocks, 20); csd_writable();
    assert(sunxi_mmc_set_irq(&h, irq_taken, NULL) && !sunxi_mmc_set_irq(&h, irq_taken, NULL));
    m.timed = m.irq_mode = 1; m.tick = 1; m.dma_us = dma_us; m.busy_us = busy_us; m.drop_irq = drop; m.early_isr = early;
    hints_irq = hints_timer = 0;
    if (stale) { m.notified = 1; isr(); isr(); }                                       /* stale and spurious wakes before the start */
    assert(start(blocks));
    const struct storage_wait_ops ops = {NULL, w_poll, w_hint, w_now, w_sleep, w_live, m.recheck_us};
    uint64_t begin = m.now;
    int r = storage_wait_run(&ops, m.now + 5000000);
    if (h.state != SUNXI_MMC_DONE || r != 1) { fprintf(stderr, "%s: state %d result %d error %d now %llu deadline %llu polls %u sleeps %u isr %u step %u\n", name, h.state, r, h.result.error, (unsigned long long)m.now, (unsigned long long)h.deadline_us, m.polls, m.sleeps, m.isr_runs, h.command_step); exit(1); }
    assert(h.result.confirmed_blocks == blocks && m.now - begin >= dma_us + busy_us && m.reg[SUNXI_SMHC_IMASK / 4] == 0);   /* disarmed */
    media_is(blocks);
    /* A busy of hundreds of milliseconds costs a few hundred timed wakes, not a spin: polls and sleeps stay in step. */
    unsigned expected_max = (unsigned)(dma_us / 1000 + busy_us / 1500 + 60);
    printf("  %-22s %6llu us: %4u polls, %4u sleeps (irq %u, timer %u), %2u spins, %u interrupts\n", name,
           (unsigned long long)(m.now - begin), m.polls, m.sleeps, hints_irq, hints_timer, m.spins, m.isr_runs);
    assert(m.sleeps <= expected_max && m.polls <= 2 * expected_max + 200 && hints_irq >= 1 && hints_timer >= (busy_us ? 1u : 0u));
    assert(m.isr_runs >= (drop ? 0u : 1u));
    printf("    interrupt waits ended by the recheck timer: %u\n", m.rechecks);
    if (!drop && !early && !stale) assert(!m.rechecks);                        /* a normal completion comes before the safety net */
    { struct sunxi_mmc_result result; assert(sunxi_mmc_take(&h, &result) && result.error == SUNXI_MMC_OK); }
}

int main(void)
{
    static const unsigned counts[] = {1, 2, 7, 8, 9, 64, 255, 256};
    for (unsigned sa = 0; sa <= 1; ++sa)
        for (unsigned i = 0; i < sizeof counts / sizeof counts[0]; ++i) {
            unsigned blocks = counts[i];
            setup(sa, blocks, 40 + i);
            csd_writable();
            m.prg_polls = i % 3; m.prg_polls_left = m.prg_polls; m.busy_polls = 1 + i % 5;
            assert(start(blocks));
            assert(h.state == SUNXI_MMC_WRITING && h.dma_write);
            finish();
            assert(h.state == SUNXI_MMC_DONE);
            assert(h.result.confirmed_blocks == blocks && h.result.submitted == blocks * 512u && h.result.write_started);
            assert(m.cache_before == 1 && m.cache_after == 1 && m.command_count >= 3);
            assert(m.commands[0] == 13 && m.commands[1] == 25 && m.commands[m.command_count - 1] == 13);
            media_is(blocks);
            struct sunxi_mmc_result result;
            assert(sunxi_mmc_take(&h, &result) && h.state == SUNXI_MMC_READY && !h.borrowed && !h.dma_write);
            assert(result.error == SUNXI_MMC_OK && m.authority_calls > 3);
            /* The controller is left quiescent: no DMA active, resets complete. */
            assert(!m.dma_on && !(m.reg[0] & SUNXI_SMHC_GCTRL_DMA_ENABLE));
        }
    /* A tuned card's data write runs with the sample delay at SUNXI_MMC_WRITE_SAMPLE_DELAY and gets the sweep's delay back for the status commands: SAMP_DL is written once
     * before the CMD25 (after the first CMD13) and once after the data and the automatic stop, before the next CMD13; a legacy card never touches it. */
    setup(1, 8, 20); csd_writable();
    h.config.sd_profile = SUNXI_MMC_SD_UHS_SDR104; h.card.timing = 3; h.card.sample_delay = 41; h.card.signal_1v8 = 1;
    assert(start(8)); finish();
    assert(h.state == SUNXI_MMC_DONE && m.samp_n == 2);
    assert(m.samp_value[0] == (0x80u | SUNXI_MMC_WRITE_SAMPLE_DELAY) && m.samp_at[0] == 1 && m.commands[0] == 13 && m.commands[1] == 25);
    assert(m.samp_value[1] == (0x80u | 41u) && m.samp_at[1] == 2 && m.commands[2] == 13);
    { struct sunxi_mmc_result res; assert(sunxi_mmc_take(&h, &res)); }
    setup(1, 8, 20); csd_writable();
    h.config.sd_profile = SUNXI_MMC_SD_UHS_SDR104; h.card.timing = 1; h.card.signal_1v8 = 1;       /* SDR25 is not tuned: its delay is already 0 */
    assert(start(8)); finish(); assert(h.state == SUNXI_MMC_DONE && m.samp_n == 0);
    /* Validation is pure: no register access, no authority call, no borrow. */
    setup(1, 2, 10); csd_writable();
    struct sunxi_mmc_write_request r = request(2);
    struct sunxi_mmc_write_authority a = {&m, authorization};
    assert(!sunxi_mmc_write_dma(&h, &r, NULL));
    r.blocks = 0; r.bytes = 0; assert(!sunxi_mmc_write_dma(&h, &r, &a));
    r = request(2); r.blocks = 257; r.bytes = 257 * 512; assert(!sunxi_mmc_write_dma(&h, &r, &a));
    r = request(2); r.bytes = 1000; assert(!sunxi_mmc_write_dma(&h, &r, &a));
    r = request(2); r.source = source + 32; assert(!sunxi_mmc_write_dma(&h, &r, &a));                 /* not 64-byte aligned */
    r = request(2); r.first_sector = SECTORS - 1; assert(!sunxi_mmc_write_dma(&h, &r, &a));            /* past the card */
    r = request(2); r.policy_token = 0; assert(!sunxi_mmc_write_dma(&h, &r, &a));
    r = request(2); r.source = (const uint8_t *)&h; assert(!sunxi_mmc_write_dma(&h, &r, &a));
    r = request(2); r.source = (const uint8_t *)desc; assert(!sunxi_mmc_write_dma(&h, &r, &a));       /* overlaps the descriptors */
    r = request(2); r.deadline_us = 0; assert(!sunxi_mmc_write_dma(&h, &r, &a));
    h.config.dma = NULL; r = request(2); assert(!sunxi_mmc_write_dma(&h, &r, &a)); h.config.dma = desc;
    memset(h.card.csd, 0, sizeof h.card.csd); assert(!start(2));                                      /* not writable */
    assert(!m.reads_seen && !m.authority_calls && !h.borrowed && h.state == SUNXI_MMC_READY);
    /* Faults retain the operation and never write outside the range. */
    struct { const char *name; unsigned *knob; unsigned value; enum sunxi_mmc_error error; } faults[] = {
        {"data crc", &m.crc_error, 1, SUNXI_MMC_CONTROLLER},
        {"card error in the CMD25 response", &m.card_error_cmd, 1, SUNXI_MMC_CARD},
        {"card error in the CMD12 response", &m.card_error_stop, 1, SUNXI_MMC_CARD},
        {"IDMAC error", &m.idst_error, 1, SUNXI_MMC_CONTROLLER},
        {"data never completes", &m.never_done, 1, SUNXI_MMC_TIMEOUT},
        {"DAT0 stays busy", &m.hang_busy, 1, SUNXI_MMC_TIMEOUT},
    };
    for (unsigned i = 0; i < sizeof faults / sizeof faults[0]; ++i) {
        setup(1, 9, 100); csd_writable();
        *faults[i].knob = faults[i].value;
        struct sunxi_mmc_write_request fr = request(9); struct sunxi_mmc_write_authority fa = {&m, authorization};
        assert(sunxi_mmc_write_dma(&h, &fr, &fa));
        for (unsigned n = 0; n < 20000 && h.state == SUNXI_MMC_WRITING; ++n) {
            if (n == 3000) m.now += 2000000;                                                              /* deadline for the hang cases */
            sunxi_mmc_poll(&h);
        }
        if (!(h.state == SUNXI_MMC_FAULT)) { fprintf(stderr, "no fault: %s\n", faults[i].name); return 1; }
        if (h.result.error != faults[i].error) { fprintf(stderr, "%s: error %d expected %d\n", faults[i].name, h.result.error, faults[i].error); return 1; }
        assert(h.borrowed == source && h.result.confirmed_blocks == 0);
        /* A data error in the transfer says where it stopped: the bytes the card side took and the descriptor the IDMAC had reached. */
        if (faults[i].knob == &m.crc_error) assert(h.result.where_valid && h.result.tbc0 == (uint32_t)m.next_desc * 4096u && h.result.desc_index == m.next_desc && m.next_desc > 0);
        unsigned calls = m.reads_seen, auth = m.authority_calls;
        sunxi_mmc_poll(&h); assert(calls == m.reads_seen && auth == m.authority_calls);                    /* retained: no more access */
        media_untouched_outside(9);
    }
    /* The authority is asked on every poll and revocation stops the transfer between descriptors. */
    setup(1, 64, 200); csd_writable(); m.revoke_after = 8;
    assert(start(64)); finish();
    assert(h.state == SUNXI_MMC_FAULT && h.result.error == SUNXI_MMC_ADMISSION);
    media_untouched_outside(64);
    /* Interrupt completion: command done and data over never poll; the busy waits on a one-shot timer. */
    printf("interrupt-driven completion (model time, 1 us per clock read):\n");
    irq_case("normal 128 KiB", 256, 4000, 300000, 0, 0, 0);
    irq_case("board 128 KiB piece", 256, 6200, 8000, 0, 0, 0);                       /* 6.2 ms of transfer, as the board measured: the recheck must not fire first */
    irq_case("early interrupt", 64, 2000, 50000, 0, 1, 0);
    irq_case("lost interrupt", 64, 2000, 50000, 1, 0, 0);
    irq_case("stale+spurious wakes", 8, 500, 5000, 0, 0, 1);
    irq_case("no busy", 1, 100, 0, 0, 0, 0);
    puts("PASS: CMD25 descriptor-DMA writes of 1 to 256 blocks, vendor register sequence and threshold, descriptor chain, DAT0 busy and programming status, cache phases, refusals without access, six retained faults, revocation, and interrupt completion with early, lost and stale interrupts");
    return 0;
}
